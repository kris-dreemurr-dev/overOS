#include "user_mode.h"
#include "../fs/fs.h"
#include "sched.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "prog_loader.h"
#include "tty.h"

extern void     kputs(const char* str, uint32_t color);
extern int      strcmp(const char* s1, const char* s2);
extern void     put_pixel(int x, int y, uint32_t color);
extern void     clear_screen(uint32_t color);
extern void     flush_buffer(void);
extern void     sleep_ms(uint32_t ms);
extern uint8_t  inb(uint16_t port);
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

#define KBD_BUF_SIZE 64
static uint8_t g_kbd_buf[KBD_BUF_SIZE];
static int g_kbd_head = 0;
static int g_kbd_tail = 0;

static mouse_state_t g_mouse_state = {0, 0, 0};
static uint8_t g_mouse_cycle = 0;
static uint8_t g_mouse_bytes[3];

static void ps2_pump(void) {
    while (inb(0x64) & 1) {
        uint8_t status = inb(0x64);
        uint8_t data = inb(0x60);
        if (status & 0x20) {
            if (g_mouse_cycle == 0) {
                if (!(data & 0x08)) continue;
            }
            g_mouse_bytes[g_mouse_cycle++] = data;
            if (g_mouse_cycle == 3) {
                g_mouse_cycle = 0;
                g_mouse_state.buttons = g_mouse_bytes[0] & 0x07;
                int32_t dx = (int32_t)g_mouse_bytes[1];
                int32_t dy = (int32_t)g_mouse_bytes[2];
                if (g_mouse_bytes[0] & 0x10) dx |= ~0xFF;
                if (g_mouse_bytes[0] & 0x20) dy |= ~0xFF;
                g_mouse_state.x += dx;
                g_mouse_state.y += dy;
            }
        } else {
            if (tty_check_hotkey(data)) { g_kbd_head = g_kbd_tail = 0; continue; }   // Alt+F1/F2
            int next = (g_kbd_head + 1) % KBD_BUF_SIZE;
            if (next != g_kbd_tail) {
                g_kbd_buf[g_kbd_head] = data;
                g_kbd_head = next;
            }
        }
    }
}

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

uint64_t syscall_dispatcher(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
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
            ps2_pump();
            if (g_kbd_head != g_kbd_tail) {
                uint8_t sc = g_kbd_buf[g_kbd_tail];
                g_kbd_tail = (g_kbd_tail + 1) % KBD_BUF_SIZE;
                return (uint64_t)sc;
            }
            return 0;
        }

        case 7: { // get_mouse(mouse_state_t* out) -> int
            if (!visible) return 0;
            ps2_pump();
            if (arg1) {
                mouse_state_t* user_ms = (mouse_state_t*)arg1;
                user_ms->buttons = g_mouse_state.buttons;
                user_ms->x = g_mouse_state.x;
                user_ms->y = g_mouse_state.y;
                g_mouse_state.x = 0;
                g_mouse_state.y = 0;
                return 1;
            }
            return 0;
        }

        case 10: { // sys_open(filename)
            if (!arg1) return (uint64_t)-1;
            //fs_go_root();
            const char* filename = (const char*)arg1;
            if (fs_file_exists(filename)) {
                int idx = 0;
                while (filename[idx] && idx < 63) {
                    caller->open83[idx] = filename[idx]; // Буфер для открытого файла в task_t
                    idx++;
                }
                caller->open83[idx] = '\0';
                return 1;
            }
            return (uint64_t)-1;
        }

        case 11: // sys_read
            if ((int)arg1 <= 0 || arg2 == 0 || arg3 == 0 || caller->open83[0] == '\0') return 0;
            //fs_go_root();
            return (uint64_t)fs_read_file(caller->open83, (void*)arg2, (uint32_t)arg3);

        case 12: // sys_close
            caller->open83[0] = '\0';
            return 0;

        case 13: { // sys_write(filename, buffer, size)
            if (arg1 && arg2) {
                //fs_go_root();
                const char* filename = (const char*)arg1;
                return (uint64_t)fs_write_file(filename, (const void*)arg2, (uint32_t)arg3);
            }
            return 0;
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

        // Alt-up прочитала оболочка другого TTY: шлём релизы, чтобы клавиши не залипли
        static const uint8_t rel[3] = {0xB8, 0x9D, 0xAA};   // Alt, Ctrl, Shift up
        for (int i = 0; i < 3; i++) {
            int nx = (g_kbd_head + 1) % KBD_BUF_SIZE;
            if (nx != g_kbd_tail) { g_kbd_buf[g_kbd_head] = rel[i]; g_kbd_head = nx; }
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
        case 30: { // sys_fork()
            task_t* parent = sched_get_current_task();
            if (!parent) return (uint64_t)-1;

            // 1. Выделяем память под структуру task_t ребенка
            task_t* child = (task_t*)pmm_alloc_page();
            if (!child) return (uint64_t)-1;

            // 2. Копируем базовые поля из родителя
            child->pid = sched_alloc_pid();
            child->state = TASK_READY;
            
            // Копируем имя родителя с припиской _fork
            int i = 0;
            while (parent->name[i] && i < 11) { child->name[i] = parent->name[i]; i++; }
            child->name[i] = '\0';

            // 3. Клонируем виртуальное адресное пространство (память)
            uint64_t* child_cr3 = vmm_clone_address_space((uint64_t*)parent->cr3);
            if (!child_cr3) {
                pmm_free_page(child);
                return (uint64_t)-1;
            }
            child->cr3 = (uint64_t)child_cr3;

            // 4. Выделяем новый стек ядра для контекста задачи
            void* kstack = pmm_alloc_page();
            if (!kstack) {
                vmm_destroy_address_space(child_cr3);
                pmm_free_page(child);
                return (uint64_t)-1;
            }
            child->kstack_bottom = (uint64_t)kstack;
            child->mem_size = 8192;
            
            uint64_t* stk = (uint64_t*)((uintptr_t)kstack + 4096);
            child->kstack_top = (uint64_t)stk;
            child->rsp = (uint64_t)stk;

            // Добавляем ребенка в кольцевой список планировщика Round-Robin
            // (здесь интеграция зависит от структуры task_list_head в sched.c)

            // Возвращаем PID ребенка для родителя (а ребенок получит 0)
            return child->pid;
        }

        case 31: { // sys_execve(const char* filename)
            const char* filename = (const char*)arg1;
            if (!filename) return (uint64_t)-1;

            fs_go_root();
            extern uint8_t kernel_temp_buf[];
            int bytes = fs_read_file(filename, kernel_temp_buf, 1024 * 1024 * 4);
            if (bytes <= 0) return (uint64_t)-1;

            task_t* current = sched_get_current_task();
            if (!current) return (uint64_t)-1;

            vmm_destroy_address_space((uint64_t*)current->cr3);

            uint64_t* new_pml4 = vmm_create_address_space();
            current->cr3 = (uint64_t)new_pml4;
            vmm_switch_directory(new_pml4);

            uint32_t num_pages = (8 * 1024 * 1024) / 4096;
            for (uint32_t p = 0; p < num_pages; p++) {
                uint64_t v_addr = PROG_LOAD_BASE + (p * 4096);
                void* p_addr = pmm_alloc_page();
                vmm_map_page(new_pml4, v_addr, (uint64_t)p_addr, VMM_FLAG_USER | VMM_FLAG_WRITABLE);
            }

            uint8_t* target = (uint8_t*)PROG_LOAD_BASE;
            for (int b = 0; b < bytes; b++) {
                target[b] = kernel_temp_buf[b];
            }

            return 0;
        }
        default:
            return 0;
    }
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

extern void default_exception_handler(void);
extern void timer_isr_asm(void); // Обработчик таймера PIT/IRQ0 (interrupts.asm) — нужен ниже для idt64[32]

static void print_hex64(uint64_t val) {
    const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[18] = '\0';
    kputs(buf, 0x55FFFF);
}

// Честный перехватчик сбоя процесса: печатает реальный RIP и безопасно завершает процесс
void handle_user_crash_c(uint64_t fault_rip, uint64_t fault_rsp, uint64_t err_code) {
    (void)err_code;
    kputs("\n\n[devOS Guard] Crash caught in User Mode (Ring 3)!\n", 0x00FF5555);
    kputs(" * Fault RIP: ", 0x00AAAAAA);
    print_hex64(fault_rip);
    kputs("  Fault RSP: ", 0x00AAAAAA);
    print_hex64(fault_rsp);
    kputs("\n * Action:    Process killed. Safely returning to kernel shell.\n\n", 0x0055FF55);
    flush_buffer();

    // Процесс с собственным стеком: завершаем как exit(-1), родитель получит код из wait
    task_t* t = sched_get_current_task();
    if (t && t->is_process) sched_exit_current(-1);   // не возвращается
}

__attribute__((naked)) static void default_isr_stub(void) {
    __asm__ __volatile__(
        "cli\n\t"

        // Если сбой произошел в Ring 3 (User Mode):
        "cmpq $1, g_user_mode_active(%rip)\n\t"
        "jne .L_kernel_fatal\n\t"

        // Стек x86_64 при входе из Ring 3:
        // [rsp+0]  = Error Code (или RIP)
        // [rsp+8]  = RIP
        // [rsp+32] = RSP
        "movq (%rsp), %rdx\n\t"       // arg3: err_code
        "movq 8(%rsp), %rdi\n\t"      // arg1: fault_rip
        "movq 32(%rsp), %rsi\n\t"     // arg2: fault_rsp
        "call handle_user_crash_c\n\t"

        // БЕЗОПАСНЫЙ ВЫХОД В ШЕЛЛ (как при Syscall 0 exit):
        "movq saved_kernel_rsp(%rip), %rsp\n\t"

        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rbx\n\t"
        "pop %rbp\n\t"

        "movq $0, g_user_mode_active(%rip)\n\t"
        "movq $-1, %rax\n\t"
        "ret\n\t"

    ".L_kernel_fatal:\n\t"
        "hlt\n\t"
    );
}

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

    g_kbd_head = g_kbd_tail = 0;
    g_mouse_cycle = 0;
    g_mouse_state.x = g_mouse_state.y = g_mouse_state.buttons = 0;

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

    // 1. Исключения процессора (векторы 0..31, DPL 0)
    uint64_t exc_handler = (uint64_t)default_isr_stub;
    for (int i = 0; i < 32; i++) {
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