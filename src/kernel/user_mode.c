#include "user_mode.h"
#include "../fs/fs.h"
#include "sched.h"
#include "../drivers/mouse.h"
#include "../drivers/ethernet.h"
#include "../drivers/hda.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "prog_loader.h"
#include "tty.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"
#include "win_proto.h"
#include "config.h"

// Реестр окон ядра
static kernel_window_t g_windows[MAX_SYS_WINDOWS];
static int             g_window_count = 0;
static int             g_next_win_id = 1;

// --- Поверхности окон и очереди событий (оконный сервер) ---
#ifndef WEV_MOUSE_DOWN
typedef struct {
    int type;        // WEV_*
    int x, y;        // координаты внутри клиентской области окна
    int buttons;
    int key;         // скан-код для WEV_KEY
} win_event_t;
#define WEV_MOUSE_DOWN 1
#define WEV_MOUSE_UP   2
#define WEV_KEY        3
#endif

#define WIN_EV_QUEUE 32

typedef struct {
    uint8_t*    surf;        // виртуальный адрес поверхности (w*h*4 байт)
    uint64_t    surf_phys;   // физический адрес (для освобождения)
    uint32_t    surf_pages;
    uint32_t    seq;         // растёт при каждом sys_win_present
    win_event_t ev[WIN_EV_QUEUE];
    uint32_t    ev_head, ev_tail;
} win_priv_t;

static win_priv_t g_wpriv[MAX_SYS_WINDOWS];   // индекс совпадает с g_windows[]

// В ядре нет libc: при -ffreestanding GCC всё равно превращает циклы копирования/зануления
// и копии крупных структур в вызовы memcpy/memset, которых при линковке нет
// ("undefined reference to memcpy"). Поэтому все массовые операции — через rep movsb/stosb.
static inline void wm_copy(void* dst, const void* src, uint64_t bytes) {
    __asm__ volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(bytes) : : "memory");
}
static inline void wm_zero(void* dst, uint64_t bytes) {
    __asm__ volatile ("rep stosb" : "+D"(dst), "+c"(bytes) : "a"(0) : "memory");
}

static int wm_index_by_id(int id) {
    for (int i = 0; i < g_window_count; i++)
        if (g_windows[i].id == id) return i;
    return -1;
}

static void wm_free_priv(int i) {
    for (uint32_t k = 0; k < g_wpriv[i].surf_pages; k++)
        pmm_free_page((void*)(g_wpriv[i].surf_phys + (uint64_t)k * 4096));
    g_wpriv[i].surf_pages = 0;
    g_wpriv[i].surf = 0;
}


extern void     kputs(const char* str, uint32_t color);
extern int      strcmp(const char* s1, const char* s2);
extern void     put_pixel(int x, int y, uint32_t color);
extern void     clear_screen(uint32_t color);
extern void     flush_buffer(void);
extern void     flush_dirty(void);
extern void     update_mouse_state(void);
extern void     sleep_ms(uint32_t ms);
extern void     get_mouse_delta(int* out_dx, int* out_dy, uint8_t* out_buttons);
extern uint8_t  inb(uint16_t port);
extern int      get_mouse_x(void);
extern int      get_mouse_y(void);
extern int      get_mouse_btn(void);
extern void     sys_reboot(void);
extern void     acpi_power_off(void);
extern void     itoa(int n, char* str);
extern uint16_t screen_width;
extern uint16_t screen_height;
extern void*    lfb;
extern uint16_t screen_pitch;   // в kernel.c это uint16_t
extern volatile uint32_t g_lfb_gen;   // +1 при каждой записи в LFB не через /dev/fb0 (см. devfs.c)
extern volatile uint32_t g_fb_last_write_tick;   // "программа сейчас выводит кадры" (см. keyboard.c, kernel.c)
extern volatile int      g_fb_write_tty;
extern uint32_t back_buffer[];
extern uint64_t g_tsc_per_ms;
extern uint32_t timer_ticks;

// Чтение исполняемого файла с поддержкой полных путей (например "/desprogs/overfiles.prg")
static int kernel_read_file_by_path(const char* path, uint8_t* buf, uint32_t max_size) {

    if (!path || !path[0]) return -1;

    fs_go_root();

    const char* p = path;
    if (p[0] == '/') p++;

    // Ищем последний слэш
    const char* last_slash = 0;
    for (const char* s = p; *s; s++) {
        if (*s == '/') last_slash = s;
    }

    if (last_slash) {
        char dir_name[64];
        int dlen = (int)(last_slash - p);
        if (dlen >= (int)sizeof(dir_name)) dlen = sizeof(dir_name) - 1;
        for (int i = 0; i < dlen; i++) dir_name[i] = p[i];
        dir_name[dlen] = '\0';

        fs_change_dir(dir_name);
        int bytes = fs_read_file(last_slash + 1, buf, max_size);
        fs_go_root();
        return bytes;
    } else {
        int bytes = fs_read_file(p, buf, max_size);
        fs_go_root();
        return bytes;
    }
}

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

// Растяжка кадра 320x200 -> экран (sys_blit_frame). Таблица X строится один раз на разрешение.
static uint16_t blit_xmap[1920];
static uint32_t blit_xmap_w = 0;

static void blit_320x200_scaled(const uint32_t* user_fb, uint32_t* dst_buf) {
    const uint32_t W = screen_width, H = screen_height;

    if (blit_xmap_w != W) {
        uint32_t step = (320u << 16) / W, fp = 0;
        for (uint32_t x = 0; x < W; x++, fp += step) {
            uint32_t sx = fp >> 16;
            blit_xmap[x] = (uint16_t)(sx > 319 ? 319 : sx);
        }
        blit_xmap_w = W;
    }

    uint32_t step_y = (200u << 16) / H, y_fp = 0, prev_sy = 0xFFFFFFFFu;
    uint32_t* prev_row = 0;
    for (uint32_t y = 0; y < H; y++, y_fp += step_y) {
        uint32_t sy = y_fp >> 16;
        if (sy > 199) sy = 199;
        uint32_t* dst_row = dst_buf + (size_t)y * W;
        if (sy == prev_sy) {   // та же исходная строка: копируем готовую
            uint32_t* d = dst_row; const uint32_t* sp = prev_row; uint64_t n = W >> 1;
            __asm__ volatile ("rep movsq" : "+D"(d), "+S"(sp), "+c"(n) :: "memory");
            if (W & 1) *d = *sp;
        } else {
            const uint32_t* src_row = user_fb + sy * 320;
            for (uint32_t x = 0; x < W; x++) dst_row[x] = src_row[blit_xmap[x]];
        }
        prev_sy = sy;
        prev_row = dst_row;
    }
}

// 1 = растягивать кадр прямо в LFB построчно (через маленький буфер в L1-кэше), минуя буфер TTY:
//     нет записи 6 МБ в RAM и повторного чтения их же при flush_buffer().
// 0 = старый путь: растяжка в буфер TTY + flush_buffer() (если что-то читает буфер TTY как "экран").
#define BLIT_DIRECT_TO_LFB 1

static uint32_t blit_row[1920] __attribute__((aligned(64)));   // одна растянутая строка

static void blit_320x200_to_lfb(const uint32_t* user_fb) {
    const uint32_t W = screen_width, H = screen_height;
    uint8_t* lfb8 = (uint8_t*)lfb;
    if (W > 1920) return;   // размер blit_row / blit_xmap

    if (blit_xmap_w != W) {
        uint32_t step = (320u << 16) / W, fp = 0;
        for (uint32_t x = 0; x < W; x++, fp += step) {
            uint32_t sx = fp >> 16;
            blit_xmap[x] = (uint16_t)(sx > 319 ? 319 : sx);
        }
        blit_xmap_w = W;
    }

    uint32_t step_y = (200u << 16) / H, y_fp = 0, prev_sy = 0xFFFFFFFFu;
    for (uint32_t y = 0; y < H; y++, y_fp += step_y) {
        uint32_t sy = y_fp >> 16;
        if (sy > 199) sy = 199;
        if (sy != prev_sy) {   // новая исходная строка: растягиваем по X в L1-кэше
            const uint32_t* src_row = user_fb + sy * 320;
            for (uint32_t x = 0; x < W; x++) blit_row[x] = src_row[blit_xmap[x]];
            prev_sy = sy;
        }
        // одна и та же готовая строка уходит в LFB столько раз, сколько нужно по вертикали
        void* d = lfb8 + (size_t)y * screen_pitch;
        const void* sp = blit_row;
        uint64_t n = W >> 1;
        __asm__ volatile ("rep movsq" : "+D"(d), "+S"(sp), "+c"(n) :: "memory");
        if (W & 1) *(uint32_t*)d = *(const uint32_t*)sp;
    }
    __asm__ volatile ("sfence" ::: "memory");
    g_lfb_gen++;   // копия экрана в devfs.c (/dev/fb0) теперь устарела
}

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
            // Печатаем в консоль только если НЕ включен графический режим
            tty_t* cur_tty = (caller && caller->tty_id >= 0) ? tty_get(caller->tty_id) : NULL;
            if (cur_tty && !cur_tty->gfx_mode) {
                kputs("\n>>> [KERNEL 64] Syscall 0 (exit) received! Code: ", 0x55FF55);
                char ebuf[16];
                itoa((int)arg1, ebuf);
                kputs(ebuf, 0xFFFFFF);
                kputs("\n", 0x55FF55);
                flush_buffer();
            }
            if (caller && caller->is_process) sched_exit_current((int)arg1);
            kernel_exit_code = arg1;
            return arg1;

        case 1: // print(str, color)
            kputs((const char*)arg1, (uint32_t)(arg2 ? arg2 : 0x00FFFFFF));
            flush_dirty();
            return 0;

        case 2: // put_pixel(x, y, color)
            put_pixel((int)arg1, (int)arg2, (uint32_t)arg3);
            return 0;

        case 3: // flush()
            flush_buffer();
            return 0;

        case 4: { // sleep(ms)
            __asm__ volatile("sti");
            sleep_ms((uint32_t)arg1);
            __asm__ volatile("cli");
            return 0;
        }

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
        case 9: { // sys_get_kernel_info(kernel_info_t* out)
            if (!arg1) return (uint64_t)-1;

            typedef struct {
                char name[32];
                char arch[16];
                char version[32];
            } __attribute__((packed)) kinfo_t;

            kinfo_t* k = (kinfo_t*)arg1;

            // Копируем данные из src/kernel/config.h
            const char* s_name = OS_NAME;
            int i = 0;
            while (s_name[i] && i < 31) { k->name[i] = s_name[i]; i++; }
            k->name[i] = '\0';

            const char* s_arch = OS_ARCH;
            i = 0;
            while (s_arch[i] && i < 15) { k->arch[i] = s_arch[i]; i++; }
            k->arch[i] = '\0';

            const char* s_ver = OS_VERSION " devbuild";
            i = 0;
            while (s_ver[i] && i < 31) { k->version[i] = s_ver[i]; i++; }
            k->version[i] = '\0';

            return 0;
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
        case 16: { // sys_get_dir_files(path, buf, max_files)
            const char* path = (const char*)arg1;
            void* buf = (void*)arg2;
            int max_files = (int)arg3;
            fs_lock();
            int count = fs_get_dir_files(path, (fs_file_info_t*)buf, max_files);
            fs_unlock();
            return (uint64_t)count;
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

            g_fb_write_tty = caller->tty_id;          // этот TTY сейчас занят игрой:
            g_fb_last_write_tick = timer_ticks;       // Ctrl+C не убивает её, курсор консоли не рисуется

#if BLIT_DIRECT_TO_LFB
            (void)my_tty;
            blit_320x200_to_lfb(user_fb);   // TTY программы сейчас активен: рисуем сразу на экран
#else
            uint32_t* dst_buf = my_tty->buffer;   // буфер TTY, в котором запущена программа
            blit_320x200_scaled(user_fb, dst_buf);
            flush_buffer();   // TTY программы сейчас активен
#endif
            return paused;
        }
        case 22: return g_tsc_per_ms;
        // В диспетчере syscall ядра overOS:
        case 23: {
            // Вход: EBX = указатель на int dx, ECX = int dy, EDX = uint8_t buttons
            int* u_dx = (int*)arg1;       // вместо regs->rbx
            int* u_dy = (int*)arg2;       // вместо regs->rcx
            uint8_t* u_btn = (uint8_t*)arg3; // вместо regs->rdx
    
            // Фоновая программа мышь не трогает (и не съедает накопленное движение)
            int k_dx = 0, k_dy = 0;
            uint8_t k_btn = 0;
            if (visible) {
                extern void get_mouse_delta(int* dx, int* dy, uint8_t* buttons);   // mouse.c
                update_mouse_state();
                get_mouse_delta(&k_dx, &k_dy, &k_btn);   // движение с прошлого вызова + кнопки
            }

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

            /* kputs("[fork] new child PID=", 0x55FFFF);
            char fbuf[16]; itoa((int)child->pid, fbuf);
            kputs(fbuf, 0xFFFFFF); kputs(", parent PID=", 0x55FFFF);
            itoa((int)parent->pid, fbuf); kputs(fbuf, 0xFFFFFF); kputs("\n", 0x55FFFF);
            sched_dump_tasks(); */

            // Родителю возвращаем PID ребёнка
            return child->pid;
        }

        case 32: { // sys_waitpid(int pid, int* status, int options)
            int target_pid = (int)arg1;
            int* out_status = (int*)arg2;
            (void)arg3; // WNOHANG

            int code = 0;
            int reaped = sched_reap_zombie(caller, target_pid, &code);
            if (reaped > 0) {
                if (out_status) *out_status = code;
                return (uint64_t)reaped;
            }
            return 0;
        }   

        case 33: { // sys_kill(int pid, int sig)
            uint64_t target_pid = (uint64_t)arg1;
            int sig = (int)arg2;
            if (target_pid <= 2) return (uint64_t)-1; // Не трогать kernel (0), tty1 (1), desktop (2)

            return (uint64_t)sched_send_signal(target_pid, sig ? sig : SIGKILL);
        }

        case 31: { // sys_execve(const char* filename, const char* args)
            const char* ufn = (const char*)arg1;
            if (!ufn) return (uint64_t)-1;
            char kname[64];
            int ki = 0;
            while (ufn[ki] && ki < 63) { kname[ki] = ufn[ki]; ki++; }
            kname[ki] = '\0';
            const char* filename = kname;
            if (!filename) return (uint64_t)-1;

            extern uint8_t kernel_temp_buf[];
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
            current->mem_size   = ((uint64_t)num_image_pages + (uint64_t)num_stack_pages) * 4096 + 4096 + PROC_KSTACK_SIZE;

            // 6. Обновляем имя процесса в task_t для sched_dump_tasks
            const char* bname = filename;
            for (int i = 0; filename[i]; i++) {
                if (filename[i] == '/') bname = &filename[i + 1];
            }
            int np = 0;
            while (bname[np] && np < 15) {
                char c = bname[np];
                if (c >= 'a' && c <= 'z') c -= 32;
                caller->name[np] = c;
                np++;
            }
            caller->name[np] = '\0';

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
        case 40: { // sys_create_window(const win_desc_t* desc) -> int win_id
            const win_desc_t* desc = (const win_desc_t*)arg1;
            task_t* caller = sched_get_current_task();

            // 1. ПРОВЕРКА ЗАГОЛОВКА: обязательное поле!
            if (!desc || !desc->title || desc->title[0] == '\0') {
                kputs("\n[!] ОШИБКА WM: Заголовок приложения не найден!\n", 0x00FF5555);
                flush_buffer();
                return (uint64_t)-1;
            }

            if (g_window_count >= MAX_SYS_WINDOWS) {
                kputs("[!] Ошибка WM: Превышен лимит окон!\n", 0x00FF5555);
                return (uint64_t)-1;
            }

            // 2. РАЗМЕР ОКНА: если не задан, делаем стандартный квадрат 320x320
            int ww = (desc->w > 0) ? desc->w : DEFAULT_WIN_SIZE;
            int wh = (desc->h > 0) ? desc->h : DEFAULT_WIN_SIZE;
            if (ww > 1920 || wh > 1080) return (uint64_t)-1;

            // Поверхность окна (клиентская область, 32 bpp): в неё клиент делает sys_win_present
            uint32_t spages = (uint32_t)(((uint64_t)ww * (uint64_t)wh * 4 + 4095) / 4096);
            void* sphys = pmm_alloc_pages(spages);
            if (!sphys) {
                kputs("[!] Ошибка WM: нет памяти под поверхность окна!\n", 0x00FF5555);
                return (uint64_t)-1;
            }

            int widx = g_window_count++;
            kernel_window_t* win = &g_windows[widx];
            win->id = g_next_win_id++;
            win->pid = caller ? (int)caller->pid : 0;
            win->is_minimized = 0;
            win->is_active = 1;

            // Каскадная начальная позиция окна
            win->x = 80 + (win->id * 25);
            win->y = 60 + (win->id * 20);
            win->w = ww;
            win->h = wh;

            win_priv_t* pv = &g_wpriv[widx];
            pv->surf_phys  = (uint64_t)sphys;
            pv->surf       = (uint8_t*)PHYS_TO_VIRT(sphys);
            pv->surf_pages = spages;
            pv->seq        = 0;
            pv->ev_head = pv->ev_tail = 0;
            wm_zero(pv->surf, (uint64_t)spages * 4096);

            // Копируем заголовок
            int p = 0;
            while (desc->title[p] && p < 63) {
                win->title[p] = desc->title[p];
                p++;
            }
            win->title[p] = '\0';

            // 3. ИКОНКА ОКНА: если передана — копируем, иначе выставляем плейсхолдер
            if (desc->icon) {
                win->has_icon = 1;
                for (int i = 0; i < 16; i++) {
                    win->icon[i] = desc->icon[i];
                }
            } else {
                win->has_icon = 0; // Плейсхолдер
            }

            return (uint64_t)win->id;
        }

        case 41: { // sys_get_windows(kernel_window_t* out, int max_cnt) -> count
            kernel_window_t* out = (kernel_window_t*)arg1;
            int max_cnt = (int)arg2;
            if (!out || max_cnt <= 0) return 0;

            // Выбрасываем окна завершённых (ZOMBIE/DEAD) и уже несуществующих процессов,
            // иначе записи копятся, и после 16 запусков окна перестают создаваться.
            // pid == 0 — окно, принадлежащее ядру, оно всегда считается живым.
            int n = 0;
            for (int i = 0; i < g_window_count; i++) {
                if (g_windows[i].pid == 0 || sched_pid_alive((uint64_t)g_windows[i].pid)) {
                    if (n != i) {
                        wm_copy(&g_windows[n], &g_windows[i], sizeof(kernel_window_t));
                        wm_copy(&g_wpriv[n],   &g_wpriv[i],   sizeof(win_priv_t));
                    }
                    n++;
                } else {
                    wm_free_priv(i);   // процесс умер — освобождаем поверхность окна
                }
            }
            g_window_count = n;

            int count = g_window_count < max_cnt ? g_window_count : max_cnt;
            wm_copy(out, g_windows, (uint64_t)count * sizeof(kernel_window_t));
            return (uint64_t)count;
        }

        case 42: { // sys_update_window(int id, int x, int y, int is_minimized)
            int id = (int)arg1;
            int x  = (int)arg2;
            int y  = (int)arg3;
            int is_min = (int)frame[0]; // r15 / 4-й аргумент

            for (int i = 0; i < g_window_count; i++) {
                if (g_windows[i].id == id) {
                    g_windows[i].x = x;
                    g_windows[i].y = y;
                    g_windows[i].is_minimized = is_min;
                    return 0;
                }
            }
            return (uint64_t)-1;
        }

        case 43: { // sys_win_present(id, pixels) — только владелец окна
            int wi = wm_index_by_id((int)arg1);
            if (wi < 0 || !arg2 || arg2 >= 0x0000800000000000ULL) return (uint64_t)-1;
            if (!caller || g_windows[wi].pid != (int)caller->pid) return (uint64_t)-1;
            uint64_t nbytes = (uint64_t)g_windows[wi].w * (uint64_t)g_windows[wi].h * 4;
            wm_copy(g_wpriv[wi].surf, (const void*)arg2, nbytes);
            g_wpriv[wi].seq++;
            return 0;
        }

        case 44: { // sys_win_read(id, dst, uint32_t* seq) -> 1 = кадр обновлён, 0 = без изменений
            int wi = wm_index_by_id((int)arg1);
            uint32_t* seqp = (uint32_t*)arg3;
            if (wi < 0 || !arg2 || !seqp) return (uint64_t)-1;
            if (arg2 >= 0x0000800000000000ULL || arg3 >= 0x0000800000000000ULL) return (uint64_t)-1;
            if (*seqp == g_wpriv[wi].seq) return 0;
            uint64_t nbytes = (uint64_t)g_windows[wi].w * (uint64_t)g_windows[wi].h * 4;
            wm_copy((void*)arg2, g_wpriv[wi].surf, nbytes);
            *seqp = g_wpriv[wi].seq;
            return 1;
        }

        case 45: { // sys_win_poll_event(id, ev) -> 1 = событие получено — только владелец окна
            int wi = wm_index_by_id((int)arg1);
            if (wi < 0 || !arg2 || arg2 >= 0x0000800000000000ULL) return (uint64_t)-1;
            if (!caller || g_windows[wi].pid != (int)caller->pid) return (uint64_t)-1;
            win_priv_t* pv = &g_wpriv[wi];
            if (pv->ev_head == pv->ev_tail) return 0;
            wm_copy((void*)arg2, &pv->ev[pv->ev_tail % WIN_EV_QUEUE], sizeof(win_event_t));
            pv->ev_tail++;
            return 1;
        }

        case 46: { // sys_win_push_event(id, ev) — событие от desktop окну-клиенту
            int wi = wm_index_by_id((int)arg1);
            if (wi < 0 || !arg2 || arg2 >= 0x0000800000000000ULL) return (uint64_t)-1;
            win_priv_t* pv = &g_wpriv[wi];
            if (pv->ev_head - pv->ev_tail >= WIN_EV_QUEUE) return (uint64_t)-1;   // очередь полна
            wm_copy(&pv->ev[pv->ev_head % WIN_EV_QUEUE], (const void*)arg2, sizeof(win_event_t));
            pv->ev_head++;
            return 0;
        }
        case 50: { // sys_reboot()
            extern void sys_reboot(void);
            sys_reboot();
            return 0;
        }

        case 51: { // sys_shutdown()
            extern void acpi_power_off(void);
            acpi_power_off();
            return 0;
        }

        case 60: { // sys_get_net_info(sys_net_info_t* info)
            if (!arg1) return (uint64_t)-1;
            __asm__ volatile ("sti");
            extern int net_get_status_info(void* out_buf);
            int res = net_get_status_info((void*)arg1);
            __asm__ volatile ("cli");
            return (uint64_t)res;
        }
        case 70: { // sys_audio(cmd, arg1, arg2)
            uint64_t cmd = arg1; // подкоманда из RBX
            switch (cmd) {
                case 0: // get_status
                    return (uint64_t)hda_get_status();

                case 1: { // get_outputs (arg2 = указатель на hda_outputs_t)
                    if (!arg2) return (uint64_t)-2;
                    return (uint64_t)check_audio_outputs((hda_outputs_t*)arg2);
                }

                case 2: // set_output (arg2 = mode: 0 AUTO, 1 SPEAKERS, 2 HEADPHONES, 3 HDMI)
                    return (uint64_t)hda_set_output((int)arg2);

                case 3: // get_output
                    return (uint64_t)hda_get_output();

                case 4: // play_test
                    hda_play_test_tone();
                    return 0;

                case 5: // stop
                    hda_stop();
                    return 0;

                case 6: // jack_poll
                    return (uint64_t)hda_jack_poll();

                default:
                    return (uint64_t)-1;
            }
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