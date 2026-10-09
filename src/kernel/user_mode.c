#include "user_mode.h"
#include "../fs/fs.h"
#include "sched.h"
#include "../drivers/mouse.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "prog_loader.h"
#include "tty.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"


extern void     kputs(const char* str, uint32_t color);
extern int      strcmp(const char* s1, const char* s2);
extern void     put_pixel(int x, int y, uint32_t color);
extern void     clear_screen(uint32_t color);
extern void     flush_buffer(void);
extern void     update_mouse_state(void);
extern void     sleep_ms(uint32_t ms);
extern void     get_mouse_delta(int* out_dx, int* out_dy, uint8_t* out_buttons);
extern uint8_t  inb(uint16_t port);
extern int      get_mouse_x(void);
extern int      get_mouse_y(void);
extern int      get_mouse_btn(void);
extern void     itoa(int n, char* str);
extern uint16_t screen_width;
extern uint16_t screen_height;
extern void*    lfb;
extern uint32_t screen_pitch;
extern uint32_t back_buffer[];
extern uint64_t g_tsc_per_ms;
extern uint32_t timer_ticks;

static uint32_t now_ms(void) {
    if (!g_tsc_per_ms) return timer_ticks;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint32_t)((((uint64_t)hi << 32) | lo) / g_tsc_per_ms);
}

volatile int g_user_mode_active = 0;

uint64_t saved_kernel_rsp = 0;
volatile uint64_t kernel_exit_code = 0; // Безопасное хранилище кода выхода
__attribute__((used)) uint8_t  ring3_app_stack[512 * 1024] __attribute__((aligned(16)));

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) gdt_entry64_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) dtr64_t;

static gdt_entry64_t gdt[6];
static dtr64_t       gdt_ptr;
static tss_entry_t   tss;

static uint32_t g_doom_palette[256];

// Относительные дельты мыши для get_mouse() (case 7): считаем их диффом от
// абсолютной позиции mouse.c (get_mouse_x/y), а не из собственного разбора
// пакетов — порт 0x60/0x64 читает только ps2_hw_service() (keyboard.c), вызывается
// безусловно из простоя kernel_main. Клавиатура для .prg идёт через очередь
// своего TTY (tty_kbd_pop), её туда раскладывает тот же ps2_hw_service.
// NB: case 23 (get_mouse_delta) — отдельный, более старый путь опроса мыши,
// я его не трогаю, не видя mouse.c целиком.
static int g_mouse_last_init = 0;
static int g_mouse_last_x = 0, g_mouse_last_y = 0;

static void str_to_fat83(const char* in, char* out83) {
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    out83[11] = '\0';
    int i = 0;
    while (*in && *in != '.' && i < 8) {
        char c = *in++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[i++] = c;
    }
    while (*in && *in != '.') in++;
    if (*in == '.') in++;
    int j = 8;
    while (*in && j < 11) {
        char c = *in++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[j++] = c;
    }
}

static void fork_child_resume(void);   // определена ниже, нужна форвард-декларация для case 30

// frame указывает на 20 слотов (160 байт), которые syscall_isr_stub сохранил на стеке:
// frame[0..13]  = r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rdx,rcx,rbx,rax
// frame[14]     = rbp вызывающего
// frame[15..19] = RIP,CS,RFLAGS,RSP,SS — аппаратно сохранённые CPU при входе через int 0x80
// Нужен только sys_fork: остальные сисколлы его не трогают.
uint64_t syscall_dispatcher(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t* frame) {
    task_t* caller = sched_get_current_task();
    int visible = (!caller || caller->tty_id < 0 || caller->tty_id == tty_get_active_id());
    // Фоновая программа (её TTY не на экране) не рисует в чужой экран
    if (!visible && (num == 1 || num == 2 || num == 3 || num == 5)) return 0;

    switch (num) {
        case 0: // exit(code)
            kputs("\n>>> [KERNEL 64] Syscall 0 (exit) received! Code: ", 0x55FF55);
            char ebuf[16];
            itoa((int)arg1, ebuf);
            kputs(ebuf, 0xFFFFFF);
            flush_buffer();
            // Настоящий процесс: становится ZOMBIE, будит родителя (wait) и не возвращается
            if (caller && caller->is_process) sched_exit_current((int)arg1);
            // Старый путь (run_in_user_mode): возврат в оболочку через saved_kernel_rsp
            kernel_exit_code = arg1; // Сохраняем код выхода в глобальную переменную
            return arg1;

        case 1: // print(str, color)
            kputs((const char*)arg1, (uint32_t)(arg2 ? arg2 : 0x00FFFFFF));
            flush_buffer();
            return 0;

        case 2: // put_pixel(x, y, color)
            put_pixel((int)arg1, (int)arg2, (uint32_t)arg3);
            return 0;

        case 3: // flush()
            flush_buffer();
            return 0;

        case 4: // sleep(ms)
            sleep_ms((uint32_t)arg1);
            return 0;

        case 5: // clear(color)
            clear_screen((uint32_t)arg1);
            flush_buffer();
            return 0;

        case 6: { // get_key() -> uint8_t
            if (!visible) return 0;   // фоновая программа клавиатуру не трогает
            if (!caller || caller->tty_id < 0) return 0;
            uint8_t sc;
            if (tty_kbd_pop(caller->tty_id, &sc)) return (uint64_t)sc;
            return 0;
        }

        case 7: { // sys_get_mouse(user_mouse_t* out) -> int
            if (!visible) return 0;

            update_mouse_state();

            if (arg1) {
                typedef struct {
                    int x;
                    int y;
                    uint8_t buttons;
                    int last_dx;
                    int last_dy;
                } __attribute__((packed)) user_mouse_t;

                extern int get_mouse_last_dx(void);
                extern int get_mouse_last_dy(void);

                user_mouse_t* ums = (user_mouse_t*)arg1;
                ums->x = get_mouse_x();
                ums->y = get_mouse_y();
                ums->buttons = get_mouse_buttons_raw();
                ums->last_dx = get_mouse_last_dx();
                ums->last_dy = get_mouse_last_dy();
                return 1;
            }
            return 0;
        }
        case 8: { // sys_get_time(rtc_time_t* out) -> int
            if (!arg1) return 0;

            typedef struct {
                uint8_t sec, min, hour;
                uint8_t day, month, year;
            } __attribute__((packed)) user_rtc_time_t;

            extern uint8_t inb(uint16_t port);
            extern void outb(uint16_t port, uint8_t val);

            // Ждём готовности RTC (в Ring 0 inb/outb полностью легальны)
            outb(0x70, 0x0A);
            while (inb(0x71) & 0x80);

            #define BCD2BIN(v) (((v) & 0x0F) + (((v) >> 4) * 10))

            outb(0x70, 0x00); uint8_t s = BCD2BIN(inb(0x71));
            outb(0x70, 0x02); uint8_t m = BCD2BIN(inb(0x71));
            outb(0x70, 0x04); uint8_t h_raw = inb(0x71);
            outb(0x70, 0x07); uint8_t d = BCD2BIN(inb(0x71));
            outb(0x70, 0x08); uint8_t mo = BCD2BIN(inb(0x71));
            outb(0x70, 0x09); uint8_t y = BCD2BIN(inb(0x71));

            uint8_t is_pm = h_raw & 0x80;
            uint8_t h = BCD2BIN(h_raw & 0x7F);
            if (is_pm) h = (h + 12) % 24;

            user_rtc_time_t* ut = (user_rtc_time_t*)arg1;
            ut->sec = s;
            ut->min = m;
            ut->hour = h;
            ut->day = d;
            ut->month = mo;
            ut->year = y;
            return 1;
        }

        case 10: { // sys_open(const char* path, uint32_t flags)
            return (uint64_t)(int64_t)vfs_sys_open((const char*)arg1, (uint32_t)arg2);
        }

        case 11: { // sys_read(int fd, void* buf, size_t count)
            return (uint64_t)vfs_sys_read((int)arg1, (void*)arg2, arg3);
        }

        case 12: { // sys_close(int fd)
            return (uint64_t)(int64_t)vfs_sys_close((int)arg1);
        }

        case 13: { // sys_write(int fd, const void* buf, size_t count)
            return (uint64_t)vfs_sys_write((int)arg1, (const void*)arg2, arg3);
        }
        case 14: { // sys_brk(new_brk) -> uint64_t
            task_t* curr = sched_get_current_task();   // куча принадлежит задаче, а не «главному» процессу
            if (!curr) return (uint64_t)-1;

            uint64_t new_brk = arg1;

            // Если передали 0 — программа запрашивает текущую границу кучи
            if (new_brk == 0) {
                return curr->heap_end;
            }

            // Нельзя опускать кучу ниже начального адреса
            if (new_brk < curr->heap_start) {
                return (uint64_t)-1;
            }

            // Выделяем и мапим новые страницы для кучи процесса
            uint64_t cur_page = (curr->heap_end + 4095) & ~0xFFFULL;
            uint64_t req_page = (new_brk + 4095) & ~0xFFFULL;

            while (cur_page < req_page) {
                void* phys = pmm_alloc_page();
                if (!phys) {
                    kputs("[!] sys_brk: Out of physical memory!\n", 0x00FF5555);
                    return (uint64_t)-1;
                }
                if (!vmm_map_page((uint64_t*)curr->cr3, cur_page, (uint64_t)phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE)) {
                    pmm_free_page(phys);   // не замапилась (нет памяти или адрес в identity-области ядра) — не теряем страницу
                    return (uint64_t)-1;
                }
                curr->mem_size += 4096;
                cur_page += 4096;
            }

            curr->heap_end = new_brk;
            return curr->heap_end;
        }
        case 15: { // sys_ioctl(int fd, uint64_t req, uint64_t arg)
            return (uint64_t)(int64_t)vfs_sys_ioctl((int)arg1, arg2, arg3);
        }

        case 20: // sys_set_palette
            if (arg1) {
                const uint32_t* pal = (const uint32_t*)arg1;
                for (int i = 0; i < 256; i++) g_doom_palette[i] = pal[i];
            }
            return 0;

        case 21: { // sys_blit_frame(fb) -> сколько мс задача простояла на паузе (пауза часов)
            if (!arg1 || arg1 >= 0x0000800000000000ULL) return 0;
            if (!caller || caller->tty_id < 0 || screen_width == 0 || screen_height == 0) return 0;
            tty_t* my_tty = tty_get(caller->tty_id);
            if (!my_tty || !my_tty->buffer) return 0;
            const uint32_t* user_fb = (const uint32_t*)arg1;

            uint32_t paused = 0;
            if (tty_get_active_id() != caller->tty_id) {
                // TTY программы не на экране: стоим и отдаём процессор другим задачам
                uint32_t t0 = now_ms();
                while (tty_get_active_id() != caller->tty_id) sched_yield();
                paused = now_ms() - t0;

                // Alt-up прочитала оболочка другого TTY: шлём релизы в НАШУ очередь TTY,
                // чтобы клавиши не залипли (синтетические байты, в обход ps2_hw_service —
                // прямиком в очередь, как раньше ps2_pump писал прямо в g_kbd_buf)
                static const uint8_t rel[3] = {0xB8, 0x9D, 0xAA};   // Alt, Ctrl, Shift up
                for (int i = 0; i < 3; i++) {
                    tty_kbd_push(caller->tty_id, rel[i]);
                }
            }

            uint32_t* dst_buf = my_tty->buffer;   // буфер TTY, в котором запущена программа
            uint32_t step_x = ((uint32_t)320 << 16) / screen_width;
            uint32_t step_y = ((uint32_t)200 << 16) / screen_height;
            uint32_t src_y_fp = 0, prev_sy = 0xFFFFFFFFu;
            uint32_t* prev_row = 0;

            for (uint32_t dst_y = 0; dst_y < screen_height; dst_y++) {
                uint32_t src_y = src_y_fp >> 16;
                if (src_y >= 200) src_y = 199;
                uint32_t* dst_row = &dst_buf[dst_y * screen_width];

                if (src_y == prev_sy) {   // та же исходная строка: копируем уже готовую
                    uint32_t* d = dst_row; const uint32_t* sp = prev_row; uint64_t n = screen_width;
                    __asm__ volatile ("rep movsl" : "+D"(d), "+S"(sp), "+c"(n) :: "memory");
                } else {
                    const uint32_t* src_row = user_fb + (src_y * 320);
                    uint32_t src_x_fp = 0;
                    for (uint32_t dst_x = 0; dst_x < screen_width; dst_x++) {
                       dst_row[dst_x] = src_row[src_x_fp >> 16];
                       src_x_fp += step_x;
                    }
                }
                prev_sy = src_y;
                prev_row = dst_row;
                src_y_fp += step_y;
            }

            flush_buffer();   // TTY программы сейчас активен
            return paused;
        }
        case 22: return g_tsc_per_ms;
        // В диспетчере syscall ядра overOS:
        case 23: {
            // Вход: EBX = указатель на int dx, ECX = int dy, EDX = uint8_t buttons
            int* u_dx = (int*)arg1;       // вместо regs->rbx
            int* u_dy = (int*)arg2;       // вместо regs->rcx
            uint8_t* u_btn = (uint8_t*)arg3; // вместо regs->rdx
    
            update_mouse_state();

            int k_dx, k_dy;
            uint8_t k_btn;
            //get_mouse_delta(&k_dx, &k_dy, &k_btn);
    
            if (u_dx) *u_dx = k_dx;
            if (u_dy) *u_dy = k_dy;
            if (u_btn) *u_btn = k_btn;
            break;
        }
        case 30: { // sys_fork()
            task_t* parent = sched_get_current_task();
            if (!parent) return (uint64_t)-1;

            // 1. Выделяем память под структуру task_t ребенка
            task_t* child = (task_t*)pmm_alloc_page();
            if (!child) return (uint64_t)-1;

            // 2. Копируем базовые поля процесса и сбрасываем состояние задач
            child->pid = sched_alloc_pid();
            child->state = TASK_READY;
            child->parent = parent;
            child->exit_code = 0;
            child->is_process = 1;        // ребёнок — такой же процесс Ring 3, как и родитель
            child->tty_id = parent->tty_id;
            child->heap_start = parent->heap_start;
            child->heap_end   = parent->heap_end;
            child->user_entry = 0;
            child->user_stack_top = 0;

            // Очищаем таблицу дескрипторов перед аллокациями памяти
            for (int fd = 0; fd < MAX_FD; fd++) {
                child->fd_table[fd] = NULL;
            }

            // Копируем имя родительского процесса
            int i = 0;
            while (parent->name[i] && i < 15) { 
                child->name[i] = parent->name[i]; 
                i++; 
            }
            child->name[i] = '\0';

            // Наследуем текущую рабочую директорию (CWD)
            int c = 0;
            while (parent->cwd[c] && c < (int)sizeof(child->cwd) - 1) {
                child->cwd[c] = parent->cwd[c];
                c++;
            }
            child->cwd[c] = '\0';

            // 3. Клонируем виртуальное адресное пространство (память User Space)
            uint64_t* child_cr3 = vmm_clone_address_space((uint64_t*)parent->cr3);
            if (!child_cr3) {
                pmm_free_page(child);
                return (uint64_t)-1;
            }
            child->cr3 = (uint64_t)child_cr3;

            // 4. Выделяем собственный ядерный стек процесса (64 КБ)
            void* kst_phys = pmm_alloc_pages(PROC_KSTACK_SIZE / 4096);
            if (!kst_phys) {
                vmm_destroy_address_space(child_cr3);
                pmm_free_page(child);
                return (uint64_t)-1;
            }
            uint8_t* kstack = (uint8_t*)PHYS_TO_VIRT(kst_phys);
            child->kstack_bottom = (uint64_t)kstack;
            child->kstack_top    = (uint64_t)(kstack + PROC_KSTACK_SIZE);
            child->mem_size = 4096 + PROC_KSTACK_SIZE;

            // 5. UNIX-WAY: Клонирование таблицы файловых дескрипторов
            // Выполняется строго после всех аллокаций, чтобы при OOM не повреждать ref_count
            for (int fd = 0; fd < MAX_FD; fd++) {
                child->fd_table[fd] = parent->fd_table[fd];
                if (child->fd_table[fd]) {
                    child->fd_table[fd]->ref_count++;
                }
            }

            // 6. Копируем на вершину стека ребёнка сохранённый кадр прерывания (160 байт).
            //    Подменяем слот RAX на 0 (для ребёнка fork() возвращает 0)
            uint64_t* tf = (uint64_t*)(kstack + PROC_KSTACK_SIZE - 160);
            for (int k = 0; k < 20; k++) tf[k] = frame[k];
            tf[13] = 0;   // frame[13] = RAX дочернего процесса

            // 7. Трамплин для switch_to: 6 callee-saved регистров + fork_child_resume
            uint64_t* stk = tf;
            *(--stk) = (uint64_t)fork_child_resume;
            *(--stk) = 0; // rbp
            *(--stk) = 0; // rbx
            *(--stk) = 0; // r12
            *(--stk) = 0; // r13
            *(--stk) = 0; // r14
            *(--stk) = 0; // r15
            child->rsp = (uint64_t)stk;

            child->in_user = 1;
            child->saved_krsp = child->kstack_top;

            // 8. Добавляем ребёнка в планировщик
            sched_enqueue_task(child);

            kputs("[fork] new child PID=", 0x55FFFF);
            char fbuf[16]; itoa((int)child->pid, fbuf);
            kputs(fbuf, 0xFFFFFF); kputs(", parent PID=", 0x55FFFF);
            itoa((int)parent->pid, fbuf); kputs(fbuf, 0xFFFFFF); kputs("\n", 0x55FFFF);
            sched_dump_tasks();

            // Родителю возвращаем PID ребёнка
            return child->pid;
        }

        case 32: { // sys_waitpid(pid, int* out_status) -> 1 если дождались и собрали, 0 иначе
            // out_status — указатель В АДРЕСНОМ ПРОСТРАНСТВЕ ВЫЗЫВАЮЩЕГО. CR3 на время
            // сисколла не меняется (higher-half и так общий), поэтому писать в него отсюда
            // безопасно — ровно как в случаях 2/11 (put_pixel/sys_read).
            int* out_status = (int*)arg2;
            int code = 0;
            int ok = sched_wait_pid((uint64_t)arg1, &code);

            char wbuf[16];
            kputs("[waitpid] pid=", 0x55FFFF);
            itoa((int)arg1, wbuf); kputs(wbuf, 0xFFFFFF);
            if (ok) {
                if (out_status) *out_status = code;
                kputs(": reaped, exit_code=", 0x55FFFF);
                itoa(code, wbuf); kputs(wbuf, 0xFFFFFF); kputs("\n", 0x55FFFF);
            } else {
                kputs(": FAILED (not our child or already reaped)\n", 0x55FFFF);
            }
            sched_dump_tasks();
            return (uint64_t)ok;
        }

        case 33: { // sys_kill(pid, sig)
            uint64_t target_pid = arg1;
            int sig = (int)arg2;
            if (caller && target_pid == caller->pid) {
                // Самоубийство: обычный путь выхода, с отрицательным кодом (как Unix WIFSIGNALED)
                flush_buffer();
                sched_exit_current(-sig);   // не возвращается
            }
            int kr = sched_send_signal(target_pid, sig);
            kputs("[kill] pid=", 0x55FFFF);
            char kbuf[16]; itoa((int)target_pid, kbuf); kputs(kbuf, 0xFFFFFF);
            kputs(" sig=", 0x55FFFF); itoa(sig, kbuf); kputs(kbuf, 0xFFFFFF);
            kputs(kr == 0 ? ": delivered\n" : ": FAILED (no such pid, or it's you)\n", 0x55FFFF);
            sched_dump_tasks();
            return (uint64_t)(int64_t)kr;
        }

        case 31: { // sys_execve(const char* filename, const char* args) — args пока не используется
            const char* filename = (const char*)arg1;
            if (!filename) return (uint64_t)-1;

            extern uint8_t kernel_temp_buf[];      // общий с prog_loader.c/sys_loader.c — под fs_lock
            fs_lock();
            int bytes = fs_read_file(filename, kernel_temp_buf, 1024 * 1024 * 4);
            fs_unlock();
            if (bytes <= 0) return (uint64_t)-1;

            task_t* current = sched_get_current_task();
            if (!current) return (uint64_t)-1;

            // 1. Разбор формата: нативный DPRG или плоский бинарник
            devos_prg_header_t* hdr = (devos_prg_header_t*)kernel_temp_buf;
            uint64_t load_base = PROG_LOAD_BASE;
            uint64_t entry_vaddr = PROG_LOAD_BASE;
            uint64_t payload_offset = 0;
            uint64_t payload_bytes = (uint64_t)bytes;
            uint64_t total_image_bytes = payload_bytes;
            uint32_t stack_size = 512 * 1024;

            if (hdr->magic[0] == 'D' && hdr->magic[1] == 'P' && hdr->magic[2] == 'R' && hdr->magic[3] == 'G') {
                load_base = hdr->load_vaddr ? hdr->load_vaddr : PROG_LOAD_BASE;
                entry_vaddr = hdr->entry_point ? hdr->entry_point : load_base;
                payload_offset = sizeof(devos_prg_header_t);
                payload_bytes = hdr->code_size;
                total_image_bytes = hdr->code_size + hdr->bss_size;
                if (hdr->stack_size) stack_size = (uint32_t)hdr->stack_size;
            }

            // 2. Новое адресное пространство создаём раньше уничтожения старого
            uint64_t* new_pml4 = vmm_create_address_space();
            if (!new_pml4) return (uint64_t)-1;

            uint32_t num_image_pages = (total_image_bytes + 4095) / 4096;
            if (num_image_pages == 0) num_image_pages = 1;
            for (uint32_t pg = 0; pg < num_image_pages; pg++) {
                uint64_t v_addr = load_base + (uint64_t)pg * 4096;
                void* p_addr = pmm_alloc_page();
                if (!p_addr) { vmm_destroy_address_space(new_pml4); return (uint64_t)-1; }
                vmm_map_page(new_pml4, v_addr, (uint64_t)p_addr, VMM_FLAG_USER | VMM_FLAG_WRITABLE);
            }

            // 3. Выделяем стек под новый образ
            uint32_t num_stack_pages = (stack_size + 4095) / 4096;
            uint64_t stack_top = 0x00007FFFFFFF0000ULL;
            uint64_t stack_base = stack_top - (uint64_t)num_stack_pages * 4096;
            for (uint32_t pg = 0; pg < num_stack_pages; pg++) {
                void* p_addr = pmm_alloc_page();
                if (p_addr) vmm_map_page(new_pml4, stack_base + (uint64_t)pg * 4096,
                                         (uint64_t)p_addr, VMM_FLAG_USER | VMM_FLAG_WRITABLE);
            }

            // 4. Копируем тело и зануляем .bss
            vmm_switch_directory(new_pml4);

            uint8_t* target = (uint8_t*)load_base;
            uint8_t* src = kernel_temp_buf + payload_offset;
            for (uint64_t i = 0; i < payload_bytes; i++) target[i] = src[i];
            for (uint64_t i = payload_bytes; i < (uint64_t)num_image_pages * 4096; i++) target[i] = 0;

            // 5. Старое пространство заменяем новым и освобождаем его
            uint64_t old_cr3 = current->cr3;
            current->cr3 = (uint64_t)new_pml4;
            vmm_destroy_address_space((uint64_t*)old_cr3);

            current->heap_start = load_base + (uint64_t)num_image_pages * 4096;
            current->heap_end   = current->heap_start;

            // 6. Обновляем имя процесса в task_t для sched_dump_tasks
            const char* base_name = filename;
            for (int p = 0; filename[p]; p++) {
                if (filename[p] == '/') base_name = &filename[p + 1];
            }
            int ni = 0;
            while (base_name[ni] && ni < 15) {
                current->name[ni] = base_name[ni];
                ni++;
            }
            current->name[ni] = '\0';

            // 7. UNIX-WAY VFS: очистка дескрипторов старого образа
            // Дескрипторы 0, 1, 2 (stdin, stdout, stderr) сохраняются для нового бинарника.
            // Пользовательские файлы (FD >= 3), открытые старой программой, закрываются.
            for (int fd = 3; fd < MAX_FD; fd++) {
                if (current->fd_table[fd]) {
                    vfs_release_fd(current->fd_table[fd]);
                    current->fd_table[fd] = NULL;
                }
            }

            // Гарантируем привязку стандартных потоков (0, 1, 2), если они были утеряны
            vfs_node_t* tty_node = devfs_get_node("tty");
            if (tty_node) {
                if (!current->fd_table[0]) current->fd_table[0] = vfs_allocate_fd(tty_node, O_RDONLY);
                if (!current->fd_table[1]) current->fd_table[1] = vfs_allocate_fd(tty_node, O_WRONLY);
                if (!current->fd_table[2]) current->fd_table[2] = vfs_allocate_fd(tty_node, O_WRONLY);
            }

            // 8. Подменяем RIP/RSP в кадре прерывания для перехода в новую программу
            frame[15] = entry_vaddr;   // RIP
            frame[18] = stack_top;     // RSP

            return 0;
        }
        default:
            return 0;
    }
}

// Первая "точка возврата" для ребёнка sys_fork: на стеке под ней уже лежит
// копия кадра прерывания родителя (см. case 30). Эти 15 pop + iretq —
// побайтово то же самое, что .L_normal_iretq ниже: ребёнок продолжит
// исполнение Ring 3 с той же RIP/RSP, что и родитель в момент fork(),
// но с RAX = 0 (это значение подставлено в кадр заранее).
__attribute__((naked)) static void fork_child_resume(void) {
    __asm__ volatile (
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rbx\n\t"
        "pop %rax\n\t"
        "pop %rbp\n\t"
        "iretq\n\t"
    );
}

__attribute__((naked)) static void syscall_isr_stub(void) {
    __asm__ volatile (
        "push %rbp\n\t"
        "mov %rsp, %rbp\n\t"
        
        "push %rax\n\t"
        "push %rbx\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"

        "mov %rax, %r12\n\t"

        // 5-й аргумент (SysV: r8) — указатель на начало только что сохранённого
        // блока регистров (r15 был запушен последним, значит лежит по текущему rsp).
        // Берём ДО перестановки остальных регистров под аргументы 1-4: этот mov
        // не трогает rsp, так что адрес кадра остаётся верным.
        "mov %rsp, %r8\n\t"

        "mov %rdx, %r9\n\t"
        "mov %rax, %rdi\n\t"
        "mov %rbx, %rsi\n\t"
        "mov %rcx, %rdx\n\t"
        "mov %r9,  %rcx\n\t"

        "call syscall_dispatcher\n\t"

        "mov %rax, 104(%rsp)\n\t"

        "cmpq $0, %r12\n\t"
        "jne .L_normal_iretq\n\t"

        // ЕСЛИ ЭТО EXIT: восстанавливаем оригинальный стек ядра
        "movq saved_kernel_rsp(%rip), %rsp\n\t"

        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rbx\n\t"
        "pop %rbp\n\t"

        "movq $0, g_user_mode_active\n\t"
        "movq kernel_exit_code(%rip), %rax\n\t" // Загружаем точный код выхода в RAX
        "ret\n\t"

    ".L_normal_iretq:\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rbx\n\t"
        "pop %rax\n\t"
        
        "pop %rbp\n\t"
        
        "iretq\n\t"
    );
}

// Структуры IDT и TSS
typedef struct {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t base_middle;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry64_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr64_t;

static idt_entry64_t idt64[256] __attribute__((aligned(16)));
static idt_ptr64_t   idtr64;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss64_t;

static tss64_t kernel_tss;

extern void timer_isr_asm(void); // Обработчик таймера PIT/IRQ0 (interrupts.asm) — нужен ниже для idt64[32]

// Настоящие обработчики исключений (interrupts.asm): каждый кладёт номер своего
// вектора на стек перед общим кадром и зовёт default_exception_handler (bsod.c) —
// именно оттуда bsod.c берёт exc_no/fault_eip/fault_esp (frame[15]/[17]/[20]).
// Раньше здесь на все 32 вектора стоял один default_isr_stub, который номер
// вектора вообще не знал и в Ring 0 просто делал hlt без всякой диагностики.
extern void isr_0(void);  extern void isr_1(void);  extern void isr_2(void);  extern void isr_3(void);
extern void isr_4(void);  extern void isr_5(void);  extern void isr_6(void);  extern void isr_7(void);
extern void isr_8(void);  extern void isr_9(void);  extern void isr_10(void); extern void isr_11(void);
extern void isr_12(void); extern void isr_13(void); extern void isr_14(void); extern void isr_15(void);
extern void isr_16(void); extern void isr_17(void); extern void isr_18(void); extern void isr_19(void);
extern void isr_20(void); extern void isr_21(void); extern void isr_22(void); extern void isr_23(void);
extern void isr_24(void); extern void isr_25(void); extern void isr_26(void); extern void isr_27(void);
extern void isr_28(void); extern void isr_29(void); extern void isr_30(void); extern void isr_31(void);

static void (*const exc_isr_table[32])(void) = {
    isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7,
    isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15,
    isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23,
    isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31,
};

extern uint64_t gdt64_tss[];

typedef struct {
    uint16_t limit_low;
    uint16_t base_low16;
    uint8_t  base_mid8;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_high8;
    uint32_t base_upper32;
    uint32_t reserved;
} __attribute__((packed)) gdt_tss_desc_t;

// Увеличиваем стек ядра до безопасных 16 КБ (выравнивание 16 байт по ABI)
static uint8_t kernel_interrupt_stack[16384] __attribute__((aligned(16)));

// Заглушка для фантомных и необработанных аппаратных прерываний (IRQ 7, IRQ 15 и др.)
__attribute__((naked)) static void spurious_irq_stub(void) {
    __asm__ volatile ("iretq");
}

void init_user_mode(void) {
    // Временно маскируем все IRQ на контроллере PIC
    __asm__ volatile ("outb %0, $0x21" : : "a"((uint8_t)0xFF));
    __asm__ volatile ("outb %0, $0xA1" : : "a"((uint8_t)0xFF));

    // Очереди клавиатуры по TTY уже обнулены в tty_init_core/tty_spawn_second;
    // состояние мыши — в mouse.c, туда отдельного сброса тоже не нужно.

    for (int i = 0; i < (int)sizeof(kernel_tss); i++) ((uint8_t*)&kernel_tss)[i] = 0;
    
    // Вершина проверенного статического стека ядра
    uint64_t stack_base = (uint64_t)((uintptr_t)kernel_interrupt_stack);
    kernel_tss.rsp0 = stack_base + sizeof(kernel_interrupt_stack);
    kernel_tss.iomap_base = sizeof(kernel_tss);

    gdt_tss_desc_t* tss_desc = (gdt_tss_desc_t*)gdt64_tss;
    uint64_t tss_base = (uint64_t)&kernel_tss;
    uint32_t tss_limit = sizeof(kernel_tss) - 1;

    tss_desc->limit_low    = tss_limit & 0xFFFF;
    tss_desc->base_low16   = tss_base & 0xFFFF;
    tss_desc->base_mid8    = (tss_base >> 16) & 0xFF;
    tss_desc->access       = 0x89;
    tss_desc->flags_limit  = (tss_limit >> 16) & 0x0F;
    tss_desc->base_high8   = (tss_base >> 24) & 0xFF;
    tss_desc->base_upper32 = (tss_base >> 32);
    tss_desc->reserved     = 0;

    __asm__ volatile("ltr %0" : : "r"((uint16_t)0x28));

    // Обнуляем все 256 дескрипторов IDT
    for (int i = 0; i < 256; i++) {
        idt64[i].base_low    = 0;
        idt64[i].selector    = 0;
        idt64[i].ist         = 0;
        idt64[i].type_attr   = 0;
        idt64[i].base_middle = 0;
        idt64[i].base_high   = 0;
        idt64[i].reserved    = 0;
    }

    // 1. Исключения процессора (векторы 0..31, DPL 0) — настоящий isr_N на каждый
    for (int i = 0; i < 32; i++) {
        uint64_t exc_handler = (uint64_t)exc_isr_table[i];
        idt64[i].base_low    = exc_handler & 0xFFFF;
        idt64[i].selector    = 0x08;
        idt64[i].ist         = 0;
        idt64[i].type_attr   = 0x8E;
        idt64[i].base_middle = (exc_handler >> 16) & 0xFFFF;
        idt64[i].base_high   = (exc_handler >> 32) & 0xFFFFFFFF;
        idt64[i].reserved    = 0;
    }

    // 2. Все аппаратные линии IRQ 0..15 (векторы 32..47)
    // Закрываем безопасной заглушкой iretq от спонтанных всплесков реального чипсета
    uint64_t spurious_handler = (uint64_t)spurious_irq_stub;
    for (int i = 32; i < 48; i++) {
        idt64[i].base_low    = spurious_handler & 0xFFFF;
        idt64[i].selector    = 0x08;
        idt64[i].ist         = 0;
        idt64[i].type_attr   = 0x8E;
        idt64[i].base_middle = (spurious_handler >> 16) & 0xFFFF;
        idt64[i].base_high   = (spurious_handler >> 32) & 0xFFFFFFFF;
        idt64[i].reserved    = 0;
    }

    // 3. Вектор 32 — рабочий таймер PIT/IRQ0
    uint64_t timer_handler = (uint64_t)timer_isr_asm;
    idt64[32].base_low    = timer_handler & 0xFFFF;
    idt64[32].selector    = 0x08;
    idt64[32].ist         = 0;
    idt64[32].type_attr   = 0x8E;
    idt64[32].base_middle = (timer_handler >> 16) & 0xFFFF;
    idt64[32].base_high   = (timer_handler >> 32) & 0xFFFFFFFF;
    idt64[32].reserved    = 0;

    // 4. Вектор 0x80 — шлюз системных вызовов (DPL 3 для Ring 3)
    uint64_t syscall_handler = (uint64_t)syscall_isr_stub;
    idt64[0x80].base_low    = syscall_handler & 0xFFFF;
    idt64[0x80].selector    = 0x08;
    idt64[0x80].ist         = 0;
    idt64[0x80].type_attr   = 0xEE; // Разрешён вызов из User Mode
    idt64[0x80].base_middle = (syscall_handler >> 16) & 0xFFFF;
    idt64[0x80].base_high   = (syscall_handler >> 32) & 0xFFFFFFFF;
    idt64[0x80].reserved    = 0;

    // Загружаем обновлённую таблицу дескрипторов прерываний
    idtr64.limit = sizeof(idt64) - 1;
    idtr64.base  = (uint64_t)&idt64;

    __asm__ volatile("lidt %0" : : "m"(idtr64));

    g_user_mode_active = 0;
}

// Вершина стека ядра для входа из Ring 3: у каждой задачи свой (sched_yield переставляет при переключении)
void set_tss_rsp0(uint64_t rsp0) {
    if (rsp0) kernel_tss.rsp0 = rsp0;
}

void reset_tss_to_default(void) {
    kernel_tss.rsp0 = (uint64_t)kernel_interrupt_stack + sizeof(kernel_interrupt_stack);
}

__attribute__((naked)) int run_in_user_mode(void (*entry_point)(void), void* user_stack_top) {
    __asm__ volatile (
        "push %rbp\n\t"
        "push %rbx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"

        "movq %rdi, %r12\n\t"            // entry_point
        "movq %rsi, %r13\n\t"            // user_stack_top
        "movq %rsp, saved_kernel_rsp(%rip)\n\t"

        // rsp0 этой задачи = область прямо под сохранёнными регистрами (свой стек входа в ядро)
        "movq %rsp, %rdi\n\t"
        "subq $8, %rsp\n\t"              // выравнивание 16 байт перед call
        "call set_tss_rsp0\n\t"
        "addq $8, %rsp\n\t"

        "movq $1, g_user_mode_active\n\t"

        "pushq $0x23\n\t"                // User SS
        "pushq %r13\n\t"                 // User RSP
        "pushfq\n\t"
        "popq %rax\n\t"
        "orq $0x200, %rax\n\t"           // Включаем прерывания (IF = 1)
        "pushq %rax\n\t"                 // RFLAGS
        "pushq $0x1B\n\t"                // User CS
        "pushq %r12\n\t"                 // User RIP
        "iretq\n\t"
    );
}

void return_from_user_mode(int exit_code) {
    (void)exit_code;
}