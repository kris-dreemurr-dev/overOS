#include "sched.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "tty.h"

#define KERNEL_STACK_SIZE (64 * 1024)

extern void kputs(const char* str, uint32_t color);
extern void flush_buffer(void);
extern void itoa(int n, char* str);
extern void set_tss_rsp0(uint64_t rsp0);
extern void reset_tss_to_default(void);
extern uint64_t saved_kernel_rsp;
extern volatile int g_user_mode_active;

static void task_init_um(task_t* t) {
    t->saved_krsp = 0;
    t->in_user = 0;
    t->open83[0] = '\0';
    t->heap_start = 0;
    t->heap_end = 0;
    t->parent = NULL;
    t->exit_code = 0;
    t->is_process = 0;
    t->user_entry = 0;
    t->user_stack_top = 0;
}

static task_t* current_task = NULL;
static task_t* task_list_head = NULL;
static uint64_t next_pid = 1;

static void kstrncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    while (src && src[i] && i + 1 < n) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void print_hex64(uint64_t val) {
    const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[18] = '\0';
    kputs(buf, 0xFFFF55);
}

static void task_exit_stub(void) {
    current_task->state = TASK_DEAD;
    while (1) {
        sched_yield();
    }
}

void sched_init(void) {
    task_t* root_task = (task_t*)pmm_alloc_page();
    
    root_task->pid = 0;
    root_task->tty_id = -1;
    task_init_um(root_task);
    root_task->state = TASK_RUNNING;
    kstrncpy(root_task->name, "kernel_main", 16);
    
    uint64_t cur_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    root_task->cr3 = cur_cr3;
    root_task->rsp = 0;
    root_task->kstack_bottom = 0x100000;
    root_task->kstack_top    = 0x600000;
    root_task->mem_size      = 8192; // 4 КБ структура + 4 КБ базовый стек

    root_task->next = root_task;
    
    current_task = root_task;
    task_list_head = root_task;
}

// Первая "точка возврата" для новой задачи: включает прерывания и передаёт управление в entry.
// switch_to стартует задачу через ret, а sched_yield вызывает его под cli.
__attribute__((naked)) static void task_start_sti(void) {
    __asm__ volatile("sti\n\tret");
}

task_t* task_create_kernel(void (*entry)(void), const char* name) {
    task_t* t = (task_t*)pmm_alloc_page();
    void* stack = (void*)PHYS_TO_VIRT(pmm_alloc_pages(KERNEL_STACK_SIZE / 4096));

    t->pid = next_pid++;
    t->state = TASK_READY;
    t->tty_id = -1;
    task_init_um(t);
    kstrncpy(t->name, name, 16);

    uint64_t cur_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    t->cr3 = cur_cr3;

    t->kstack_bottom = (uint64_t)stack;
    t->mem_size = 4096 + KERNEL_STACK_SIZE;

    uint64_t* stk = (uint64_t*)((uintptr_t)stack + KERNEL_STACK_SIZE);
    t->kstack_top = (uint64_t)stk;

    *(--stk) = (uint64_t)task_exit_stub;
    *(--stk) = (uint64_t)entry;
    *(--stk) = (uint64_t)task_start_sti;

    *(--stk) = 0; // rbp
    *(--stk) = 0; // rbx
    *(--stk) = 0; // r12
    *(--stk) = 0; // r13
    *(--stk) = 0; // r14
    *(--stk) = 0; // r15

    t->rsp = (uint64_t)stk;

    t->next = task_list_head->next;
    task_list_head->next = t;
    return t;
}

void sched_yield(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    if (task_list_head && current_task) {
        task_t* prev = current_task;
        task_t* next = prev->next;
        while (next != prev) {
            if ((next->state == TASK_READY || next->state == TASK_RUNNING) && next->rsp != 0) break;
            next = next->next;
        }
        if (next != prev && next->rsp != 0) {
            // Глобальное «ring-3 состояние» переезжает вместе с задачей
            prev->saved_krsp   = saved_kernel_rsp;
            prev->in_user      = g_user_mode_active;
            saved_kernel_rsp   = next->saved_krsp;
            g_user_mode_active = next->in_user;

            current_task = next;
            if (next->in_user) set_tss_rsp0(next->saved_krsp);   // свой стек входа в ядро
            if (next->cr3 && next->cr3 != prev->cr3) vmm_switch_directory((uint64_t*)next->cr3);
            switch_to(prev, next);
        }
    }

    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

void sched_dump_tasks(void) {
    kputs("\n====================== [ devOS TASK MANAGER ] ======================\n", 0x55FFFF);
    kputs("PID  NAME           STATE      RAM STACK ADDR      CR3 (PML4)   MEM\n", 0xAAAAAA);
    kputs("--------------------------------------------------------------------\n", 0x555555);

    task_t* curr = task_list_head;
    if (!curr) return;

    do {
        // PID
        char pid_buf[16];
        itoa((int)curr->pid, pid_buf);
        kputs(pid_buf, 0xFFFFFF);
        int len = 0; while (pid_buf[len]) len++;
        for (int s = 0; s < 5 - len; s++) kputs(" ", 0xFFFFFF);

        // Имя задачи
        kputs(curr->name, 0x55FF55);
        len = 0; while (curr->name[len]) len++;
        for (int s = 0; s < 15 - len; s++) kputs(" ", 0xFFFFFF);

        // Состояние
        const char* st_str = "READY   ";
        uint32_t st_col = 0xFFFF55;
        if (curr->state == TASK_RUNNING)       { st_str = "RUNNING "; st_col = 0x55FF55; }
        else if (curr->state == TASK_SLEEPING) { st_str = "SLEEP   "; st_col = 0x55FFFF; }
        else if (curr->state == TASK_DEAD)     { st_str = "DEAD    "; st_col = 0xFF5555; }
        else if (curr->state == TASK_ZOMBIE)   { st_str = "ZOMBIE  "; st_col = 0xFF5555; }
        kputs(st_str, st_col);
        kputs("  ", 0xFFFFFF);

        // Физический адрес стека в оперативной памяти
        print_hex64(curr->kstack_bottom);
        kputs("  ", 0xFFFFFF);

        // Каталог страниц (CR3)
        print_hex64(curr->cr3);
        kputs("  ", 0xFFFFFF);

        // Объем памяти
        char mem_buf[16];
        itoa((int)(curr->mem_size / 1024), mem_buf);
        kputs(mem_buf, 0xFFFFFF);
        kputs(" KB\n", 0xAAAAAA);

        curr = curr->next;
    } while (curr != task_list_head);

    kputs("====================================================================\n\n", 0x55FFFF);
    flush_buffer();
}

task_t* sched_get_current_task(void) {
    return current_task;
}

uint64_t sched_alloc_pid(void) {
    return next_pid++;
}

task_t* task_create_user_process(void (*entry_point)(void), uint64_t cr3_val, const char* name) {
    task_t* t = (task_t*)pmm_alloc_page();
    void* stack = pmm_alloc_page(); // Стек ядра для прерываний этого процесса
    
    t->pid = sched_alloc_pid();
    t->state = TASK_READY;
    t->tty_id = -1;
    task_init_um(t);
    
    // Копируем имя задачи
    int i = 0;
    while (name && name[i] && i < 15) { t->name[i] = name[i]; i++; }
    t->name[i] = '\0';
    
    t->cr3 = cr3_val; // Изолированный PML4 процесса
    t->kstack_bottom = (uint64_t)stack;
    t->mem_size = 8192;

    uint64_t* stk = (uint64_t*)((uintptr_t)stack + 4096);
    t->kstack_top = (uint64_t)stk;

    // Настраиваем стек так, чтобы при первом переключении 
    // планировщик запустил точку входа программы в Ring 3
    // (или обертку вызова run_in_user_mode)
    *(--stk) = (uint64_t)task_exit_stub;
    *(--stk) = (uint64_t)entry_point;
    
    // Сохраняем пустые callee-saved регистры
    for(int r = 0; r < 6; r++) *(--stk) = 0;

    t->rsp = (uint64_t)stk;

    // Добавляем в кольцевой список планировщика Round-Robin
    extern task_t* task_list_head;
    if (task_list_head) {
        t->next = task_list_head->next;
        task_list_head->next = t;
    } else {
        t->next = t;
        task_list_head = t;
    }

    return t;
}

task_t* sched_register_user_task(const char* name, uint64_t cr3, uint64_t mem_size) {
    task_t* t = (task_t*)pmm_alloc_page();
    if (!t) return NULL;

    t->pid = next_pid++;
    t->state = TASK_RUNNING;
    t->tty_id = -1;
    task_init_um(t);
    kstrncpy(t->name, name, 16);
    t->cr3 = cr3;
    t->rsp = 0;
    t->kstack_bottom = 0;
    t->kstack_top    = 0;
    t->mem_size = mem_size;

    if (task_list_head) {
        t->next = task_list_head->next;
        task_list_head->next = t;
    } else {
        t->next = t;
        task_list_head = t;
    }
    return t;
}

void sched_remove_user_task(task_t* t) {
    if (!t || !task_list_head) return;
    task_t* curr = task_list_head;
    do {
        if (curr->next == t) {
            curr->next = t->next;
            if (task_list_head == t) {
                task_list_head = (t->next == t) ? NULL : curr;
            }
            pmm_free_page(t);
            return;
        }
        curr = curr->next;
    } while (curr != task_list_head);
}

void sched_set_current_task(task_t* t) {
    current_task = t;
}

static task_t* foreground_task = NULL;

void sched_set_foreground_task(task_t* t) {
    foreground_task = t;
}

task_t* sched_get_foreground_task(void) {
    return foreground_task;
}

// ============================================================================
// Процессы Ring 3 (модель Unix): у процесса СВОЙ ядерный стек и своё адресное
// пространство. Родитель запускает его (spawn), засыпает в wait, процесс при
// завершении становится ZOMBIE, а родитель освобождает его ресурсы.
// ============================================================================
// Первый код нового процесса: уже на СВОЁМ ядерном стеке и в СВОЁМ адресном пространстве.
// Входит в Ring 3 через iretq; дальше все входы в ядро идут на rsp0 = вершина этого стека.
static void user_task_entry(void) {
    task_t* me = current_task;
    set_tss_rsp0(me->kstack_top);
    g_user_mode_active = 1;
    uint64_t rsp = me->user_stack_top;
    uint64_t rip = me->user_entry;
    __asm__ volatile(
        "cli\n\t"
        "pushq $0x23\n\t"          // User SS
        "pushq %0\n\t"             // User RSP
        "pushq $0x202\n\t"         // RFLAGS: IF = 1
        "pushq $0x1B\n\t"          // User CS
        "pushq %1\n\t"             // User RIP
        "iretq\n\t"
        :: "r"(rsp), "r"(rip) : "memory");
    __builtin_unreachable();
}

task_t* sched_spawn_process(const char* name, uint64_t cr3, uint64_t mem_size,
                            uint64_t user_entry, uint64_t user_stack_top, uint64_t heap_start) {
    task_t* t = (task_t*)pmm_alloc_page();
    if (!t) return NULL;
    void* kst_phys = pmm_alloc_pages(PROC_KSTACK_SIZE / 4096);
    if (!kst_phys) { pmm_free_page(t); return NULL; }
    void* stack = (void*)PHYS_TO_VIRT(kst_phys);

    t->pid = next_pid++;
    t->state = TASK_READY;
    task_init_um(t);
    kstrncpy(t->name, name, 16);

    t->parent = current_task;                                  // тот, кто запускает и будет ждать
    t->tty_id = current_task ? current_task->tty_id : -1;      // процесс работает на TTY оболочки
    t->cr3 = cr3;
    t->kstack_bottom = (uint64_t)stack;
    t->mem_size = mem_size + 4096 + PROC_KSTACK_SIZE;
    t->heap_start = heap_start;
    t->heap_end   = heap_start;
    t->is_process = 1;
    t->user_entry = user_entry;
    t->user_stack_top = user_stack_top;

    uint64_t* stk = (uint64_t*)((uintptr_t)stack + PROC_KSTACK_SIZE);
    t->kstack_top = (uint64_t)stk;
    t->in_user = 1;                    // sched_yield выставит rsp0 = saved_krsp при переключении на неё
    t->saved_krsp = t->kstack_top;

    // Как у task_create_kernel: switch_to стартует задачу через ret
    *(--stk) = (uint64_t)task_exit_stub;
    *(--stk) = (uint64_t)user_task_entry;
    *(--stk) = (uint64_t)task_start_sti;
    for (int r = 0; r < 6; r++) *(--stk) = 0;
    t->rsp = (uint64_t)stk;

    sched_enqueue_task(t);
    return t;
}

void sched_enqueue_task(task_t* t) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    t->next = task_list_head->next;
    task_list_head->next = t;
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
}

void sched_exit_current(int code) {
    __asm__ volatile("cli");
    task_t* me = current_task;
    me->exit_code = code;
    me->state = TASK_ZOMBIE;
    if (me->parent && me->parent->state == TASK_SLEEPING) me->parent->state = TASK_READY;
    while (1) sched_yield();           // планировщик ZOMBIE не выбирает — сюда не вернёмся
}

int sched_wait_child(task_t* child) {
    if (!child) return -1;
    task_t* me = current_task;

    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    while (child->state != TASK_ZOMBIE) {
        me->state = TASK_SLEEPING;     // exit ребёнка разбудит нас
        sched_yield();
    }
    me->state = TASK_READY;
    int code = child->exit_code;

    // Убираем ребёнка из кольца планировщика
    task_t* c = task_list_head;
    do {
        if (c->next == child) {
            c->next = child->next;
            if (task_list_head == child) task_list_head = c;
            break;
        }
        c = c->next;
    } while (c != task_list_head);
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");

    // Освобождаем ресурсы процесса (он уже не исполняется: ушёл в ZOMBIE и отдал CPU)
    uint64_t cur_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    if (child->cr3 && child->cr3 != cur_cr3)
        vmm_destroy_address_space((uint64_t*)child->cr3);

    uint64_t kst_phys = child->kstack_bottom - 0xFFFFFFFF80000000ULL;
    for (uint32_t i = 0; i < PROC_KSTACK_SIZE / 4096; i++)
        pmm_free_page((void*)(kst_phys + (uint64_t)i * 4096));

    reset_tss_to_default();            // rsp0 больше не указывает в освобождённый стек
    pmm_free_page(child);
    return code;
}