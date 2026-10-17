#include "sched.h"
#include "smp.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"

#define KERNEL_STACK_SIZE (64 * 1024)

extern void kputs(const char* str, uint32_t color);
extern void flush_buffer(void);
extern void itoa(int n, char* str);
extern void set_tss_rsp0(uint64_t rsp0);
extern void reset_tss_to_default(void);
extern uint64_t saved_kernel_rsp;
extern volatile int g_user_mode_active;

extern uint32_t timer_ticks;
extern void ipc_task_exit(int pid);
extern void lib_release_pid(uint64_t pid);   // слоты .ll завершившегося процесса

static void task_fpu_clean(task_t* t) {
    for (int i = 0; i < 512; i++) t->fpu_state[i] = 0;
    t->fpu_state[0] = 0x7F; t->fpu_state[1] = 0x03;        // FCW
    t->fpu_state[24] = 0x80; t->fpu_state[25] = 0x1F;      // MXCSR
    t->fpu = (uint64_t)t->fpu_state;
}

// Снимок живого состояния текущей задачи — им наследует ребёнок при fork
void sched_task_fpu_capture(task_t* t) {
    if (!t) return;
    t->fpu = (uint64_t)t->fpu_state;
    __asm__ volatile("fxsave (%0)" :: "r"(t->fpu_state) : "memory");
}

static void task_init_um(task_t* t) {
    task_fpu_clean(t);
    t->arg[0] = '\0';
    t->wake_tick = 0;
    t->wait_flags = 0;
    t->ipc_pending = 0;
    t->saved_krsp = 0;
    t->in_user = 0;
    t->heap_start = 0; 
    t->heap_end = 0; 
    t->parent = NULL; 
    t->exit_code = 0; 
    t->is_process = 0; 
    t->user_entry = 0; 
    t->user_stack_top = 0; 
    t->uid = 0;                        // ядро и его потоки — root
    t->gid = 0;
    t->cwd[0] = '/'; t->cwd[1] = '\0';
    for (int i = 0; i < MAX_FD; i++) t->fd_table[i] = NULL;
}

static task_t* current_task = NULL;
static task_t* task_list_head = NULL;
static task_t* init_task = NULL;      // kernel_main (pid 0) — "init": принимает сирот
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

// Поток ядра вернулся из своей функции. Раньше он становился TASK_DEAD и висел
// в кольце планировщика навсегда -- со своей страницей задачи и стеком на 64 КБ.
// Именно такие записи и оставались в ps. Теперь это обычный зомби: его соберёт
// init (sched_reap_orphans), как и завершившийся процесс.
static void task_exit_stub(void) {
    current_task->exit_code = 0;
    current_task->state = TASK_ZOMBIE;
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
    init_task = root_task;
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

    // Потоку ядра нужны именно таблицы ядра. Раньше здесь брался ТЕКУЩИЙ CR3, а
    // pty_open зовут из сисколла overTerm -- поток оболочки навсегда оставался
    // жить в адресном пространстве программы, и когда та завершалась, её таблицы
    // освобождались прямо под работающим потоком.
    t->cr3 = (uint64_t)vmm_get_kernel_pml4_phys();

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

// Пустить на выполнение процесс, созданный sched_spawn_process
void sched_start_process(task_t* t) {
    if (!t) return;
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    if (t->state == TASK_SLEEPING && !t->wait_flags) t->state = TASK_READY;
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
}

void sched_yield(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    if (task_list_head && current_task) {
        task_t* prev = current_task;
        task_t* next = prev->next;
        while (next != prev) {
            // Спящую по таймеру/сообщению задачу будим здесь же, когда её условие выполнилось
            if (next->state == TASK_SLEEPING && next->wait_flags) {
                uint32_t now = *(volatile uint32_t*)&timer_ticks;
                if (((next->wait_flags & WAIT_IPC) && next->ipc_pending) ||
                    ((next->wait_flags & WAIT_TIMER) && (int32_t)(now - next->wake_tick) >= 0))
                    next->state = TASK_READY;
            }
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

// Снимок таблицы задач для ps(1) в Ring 3 (раньше список умел печатать только
// встроенный taskmgr ядра). Возвращает, сколько записей заполнено.
int sched_task_list(sched_proc_info_t* out, int max) {
    if (!out || max <= 0) return 0;
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    int n = 0;
    task_t* c = task_list_head;
    if (c) {
        do {
            if (n >= max) break;
            out[n].pid   = (int)c->pid;
            out[n].uid   = c->uid;
            out[n].tty   = c->tty_id;
            out[n].state = (int)c->state;
            out[n].is_process = c->is_process;
            out[n].mem_kb = (uint32_t)(c->mem_size / 1024);
            out[n].cpu_ms = c->cpu_ticks;
            int i = 0;
            while (c->name[i] && i < (int)sizeof(out[n].name) - 1) { out[n].name[i] = c->name[i]; i++; }
            out[n].name[i] = '\0';
            n++;
            c = c->next;
        } while (c != task_list_head);
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return n;
}

void sched_set_exe(task_t* t, const char* path, const char* cwd) {
    if (!t || !path) return;
    int n = 0;
    if (path[0] != '/' && cwd && cwd[0]) {
        while (cwd[n] && n < (int)sizeof(t->exe) - 2) { t->exe[n] = cwd[n]; n++; }
        if (n && t->exe[n - 1] != '/') t->exe[n++] = '/';
    }
    const char* p = path;
    if (p[0] == '.' && p[1] == '/') p += 2;
    while (*p && n < (int)sizeof(t->exe) - 1) t->exe[n++] = *p++;
    t->exe[n] = '\0';
}

int sched_proc_exe(uint64_t pid, char* out, int max) {
    if (!out || max <= 0) return -1;
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    int rc = -1;
    task_t* c = task_list_head;
    if (c) {
        do {
            if (c->pid == pid && c->state != TASK_DEAD) {
                int i = 0;
                while (c->exe[i] && i < max - 1) { out[i] = c->exe[i]; i++; }
                out[i] = '\0';
                rc = c->exe[0] ? 0 : -1;
                break;
            }
            c = c->next;
        } while (c != task_list_head);
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return rc;
}

void sched_dump_tasks(void) {
    kputs("\n====================== [ overOS TASK MANAGER ] ======================\n", 0x55FFFF);
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
    // Процесс рождается ПРИОСТАНОВЛЕННЫМ: состояние "спит без причины пробуждения"
    // планировщик не выбирает никогда. Иначе ребёнок успевал побежать раньше, чем
    // запускающий проставит ему передний план TTY, и первый же sys_open("/dev/fb0")
    // получал отказ -- именно так рабочий стол молча умирал с кодом 1, когда
    // загрузка с флешки шла через xHCI (другая скорость -- другой исход гонки).
    // Будит его sched_start_process, когда вызывающий всё настроил.
    t->state = TASK_SLEEPING;
    t->wait_flags = 0;
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
    t->uid = current_task ? current_task->uid : 0;   // процесс наследует пользователя запустившего
    t->gid = current_task ? current_task->gid : 0;

    // --- Инициализация VFS для нового процесса ---
    if (current_task && current_task->cwd[0] != '\0') {
        kstrncpy(t->cwd, current_task->cwd, sizeof(t->cwd));
    } else {
        t->cwd[0] = '/';
        t->cwd[1] = '\0';
    }

    // Привязка стандартных потоков (stdin: 0, stdout: 1, stderr: 2) к /dev/tty
    vfs_node_t* tty_node = devfs_get_node("tty");
    if (tty_node) {
        t->fd_table[0] = vfs_allocate_fd(tty_node, O_RDONLY);
        t->fd_table[1] = vfs_allocate_fd(tty_node, O_WRONLY);
        t->fd_table[2] = vfs_allocate_fd(tty_node, O_WRONLY);
    }

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

int sched_wait_pid(uint64_t pid, int* out_code) {
    task_t* me = current_task;

    // Ищем задачу с этим PID среди СВОИХ детей. Сканируем под cli: список может
    // поменяться (exit другого ребёнка), а pid не должен успеть переиспользоваться
    // посреди поиска (next_pid только растёт, переиспользования PID нет).
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    task_t* found = NULL;
    if (task_list_head) {
        task_t* c = task_list_head;
        do {
            if (c->pid == pid && c->parent == me) { found = c; break; }
            c = c->next;
        } while (c != task_list_head);
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");

    if (!found) return 0;   // не наш ребёнок, уже собран кем-то другим, или не существует

    int code = sched_wait_child(found);
    if (out_code) *out_code = code;
    return 1;
}

// Сироты, как в UNIX: умирающий родитель передаёт детей init (kernel_main).
// Без этого ребёнок оставался зомби навсегда, а его parent указывал в освобождённую
// страницу — kill/exit ребёнка потом писал в чужую память.
static void reparent_children(task_t* dying) {
    if (!task_list_head) return;
    task_t* c = task_list_head;
    do {
        if (c->parent == dying) c->parent = init_task;
        c = c->next;
    } while (c != task_list_head);
}

int sched_send_signal(uint64_t pid, int sig) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");

    task_t* target = NULL;
    if (task_list_head) {
        task_t* c = task_list_head;
        do {
            if (c->pid == pid) { target = c; break; }
            c = c->next;
        } while (c != task_list_head);
    }

    if (!target || target == current_task ||
        target->state == TASK_ZOMBIE || target->state == TASK_DEAD) {
        __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
        return -1;   // нет такого PID, это мы сами (себя так не убить), или уже не жив
    }

    // Ни SIGKILL, ни SIGINT не перехватываются — цель просто становится ZOMBIE,
    // будим её родителя, если он спит в wait() (как и при обычном exit()).
    // Отрицательный exit_code — по аналогии с Unix WIFSIGNALED: "убит сигналом sig".
    target->exit_code = -sig;
    if (target->is_process) { ipc_task_exit((int)target->pid); lib_release_pid(target->pid); smp_stop_space(target->cr3); }
    reparent_children(target);
    target->state = TASK_ZOMBIE;
    if (target->parent && target->parent->state == TASK_SLEEPING) target->parent->state = TASK_READY;

    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return 0;
}

void sched_exit_current(int code) {
    __asm__ volatile("cli");
    task_t* me = current_task;
    me->exit_code = code;
    reparent_children(me);
    me->state = TASK_ZOMBIE;
    if (me->is_process) {                              // ящик, имена, shm, .ll — сразу, не дожидаясь wait()
        ipc_task_exit((int)me->pid);
        lib_release_pid(me->pid);
    }
    if (me->parent && me->parent->state == TASK_SLEEPING) me->parent->state = TASK_READY;

    // Память отдаём прямо здесь, а не ждём сборщика: сначала уходим на таблицы
    // ядра (рушить то пространство, по которому работаем, нельзя), и только
    // потом освобождаем своё. Ядерный стек задачи лежит в прямой карте ядра,
    // поэтому смена CR3 из-под нас его не уносит; сам стек отдаст сборщик.
    if (me->is_process && me->cr3) {
        uint64_t kcr3 = (uint64_t)vmm_get_kernel_pml4_phys();
        if (kcr3) __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    }
    sched_release_space(me);

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

    // --- VFS: закрываем и освобождаем все открытые файловые дескрипторы ---
    for (int i = 0; i < MAX_FD; i++) {
        if (child->fd_table[i]) {
            vfs_release_fd(child->fd_table[i]);
            child->fd_table[i] = NULL;
        }
    }

    // Освобождаем ресурсы процесса (он уже не исполняется: ушёл в ZOMBIE и отдал CPU)
    uint64_t cur_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    if (child->cr3 && child->cr3 != cur_cr3)
        vmm_destroy_address_space((uint64_t*)child->cr3);

    uint64_t kst_phys = virt_to_phys(child->kstack_bottom);
    for (uint32_t i = 0; i < PROC_KSTACK_SIZE / 4096; i++)
        pmm_free_page((void*)(kst_phys + (uint64_t)i * 4096));

    reset_tss_to_default();            // rsp0 больше не указывает в освобождённый стек
    pmm_free_page(child);
    return code;
}

// Неблокирующий сбор завершившегося зомби из кольца планировщика
int sched_reap_zombie(task_t* parent, int target_pid, int* out_code) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");

    task_t* found = NULL;
    if (task_list_head) {
        task_t* c = task_list_head;
        do {
            if (c->state == TASK_ZOMBIE) {
                // Если запрошен конкретный PID (target_pid > 0)
                if (target_pid > 0 && (int)c->pid != target_pid) {
                    c = c->next;
                    continue;
                }
                // Если передан parent — проверяем, что это его ребёнок
                // (если parent == NULL — собираем любого зомби / сироту)
                if (parent && c->parent != parent) {
                    c = c->next;
                    continue;
                }
                found = c;
                break;
            }
            c = c->next;
        } while (c != task_list_head);
    }

    if (!found) {
        __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
        return 0; // Зомби не найдены
    }

    int reaped_pid = (int)found->pid;
    int code = found->exit_code;
    if (out_code) *out_code = code;

    // Убираем задачу из кольцевого списка Round-Robin
    task_t* c = task_list_head;
    do {
        if (c->next == found) {
            c->next = found->next;
            if (task_list_head == found) {
                task_list_head = (found->next == found) ? NULL : c;
            }
            break;
        }
        c = c->next;
    } while (c != task_list_head);

    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");

    // Освобождаем файловые дескрипторы VFS
    for (int i = 0; i < MAX_FD; i++) {
        if (found->fd_table[i]) {
            vfs_release_fd(found->fd_table[i]);
            found->fd_table[i] = NULL;
        }
    }

    // Освобождаем виртуальное адресное пространство процесса (CR3)
    uint64_t cur_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    if (found->cr3 && found->cr3 != cur_cr3) {
        vmm_destroy_address_space((uint64_t*)found->cr3);
    }

    // Освобождаем ядерный стек процесса
    if (found->kstack_bottom) {
        uint64_t kst_phys = virt_to_phys(found->kstack_bottom);
        uint32_t npages = (found->is_process ? PROC_KSTACK_SIZE : KERNEL_STACK_SIZE) / 4096;
        for (uint32_t i = 0; i < npages; i++) {
            pmm_free_page((void*)(kst_phys + (uint64_t)i * 4096));
        }
    }

    if (current_task && current_task->is_process) set_tss_rsp0(current_task->kstack_top);
    else reset_tss_to_default();
    pmm_free_page(found);

    return reaped_pid;
}

int sched_pid_alive(uint64_t pid) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    int alive = 0;
    task_t* c = task_list_head;
    if (c) do {
        if (c->pid == pid) { alive = (c->state != TASK_ZOMBIE && c->state != TASK_DEAD); break; }
        c = c->next;
    } while (c != task_list_head);
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return alive;
}
// uid живого процесса pid, -1 — такого нет (для проверки прав в sys_kill)
int sched_pid_uid(uint64_t pid) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    int uid = -1;
    task_t* c = task_list_head;
    if (c) do {
        if (c->pid == pid) {
            if (c->state != TASK_ZOMBIE && c->state != TASK_DEAD) uid = c->uid;
            break;
        }
        c = c->next;
    } while (c != task_list_head);
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return uid;
}

// 1 — живой процесс Ring 3 (не поток ядра вроде оболочки TTY)
int sched_pid_is_process(uint64_t pid) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    int r = 0;
    task_t* c = task_list_head;
    if (c) do {
        if (c->pid == pid) { r = c->is_process && c->state != TASK_ZOMBIE && c->state != TASK_DEAD; break; }
        c = c->next;
    } while (c != task_list_head);
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return r;
}

void sched_wake_ipc(uint64_t pid) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    task_t* c = task_list_head;
    if (c) do {
        if (c->pid == pid) {
            c->ipc_pending = 1;
            if (c->state == TASK_SLEEPING && (c->wait_flags & WAIT_IPC)) c->state = TASK_READY;
            break;
        }
        c = c->next;
    } while (c != task_list_head);
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
}

void sched_sleep_ms(uint32_t ms) {
    if (!current_task || ms == 0) return;
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    uint32_t until = *(volatile uint32_t*)&timer_ticks + ms;
    while ((int32_t)(*(volatile uint32_t*)&timer_ticks - until) < 0) {
        current_task->wake_tick = until;
        current_task->wait_flags = WAIT_TIMER;
        current_task->state = TASK_SLEEPING;
        __asm__ volatile("sti");
        sched_yield();
        __asm__ volatile("cli");
        current_task->wait_flags = 0;
        if (current_task->state == TASK_SLEEPING) current_task->state = TASK_RUNNING;
        if (current_task->state == TASK_ZOMBIE || current_task->state == TASK_DEAD) break;
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
}

// Память завершившегося процесса отдаём, НЕ дожидаясь wait() от родителя.
// Так делает и Linux: в zombie остаётся только карточка задачи с кодом выхода,
// а адресное пространство рушится сразу в exit(). У нас композитор запускает
// приложения через sys_spawn и wait() не зовёт никогда, поэтому каждое
// закрытое окно оставляло висеть всё своё адресное пространство -- по 30-50 МБ
// на программу. Именно это и видно было в ps как мёртвые процессы с памятью.
// Адресное пространство и файлы завершившейся задачи. Отдельно от ядерного
// стека: при выходе задача ещё стоит на этом стеке, а вот пространство отдать
// уже можно и НУЖНО сразу -- у плеера это 80 МБ вместе с ffmpeg.ll, а wait()
// от композитора не придёт никогда.
void sched_release_space(task_t* t) {
    if (!t || t->mm_freed) return;

    uint64_t cur_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    // Рушить пространство, по которому процессор сейчас ходит, нельзя:
    // вызывающий либо уже ушёл на таблицы ядра, либо мы придём в следующий раз.
    if (t->is_process && t->cr3 == cur_cr3) return;

    t->mm_freed = 1;

    for (int i = 0; i < MAX_FD; i++) {
        if (t->fd_table[i]) {
            vfs_release_fd(t->fd_table[i]);
            t->fd_table[i] = NULL;
        }
    }

    if (t->is_process && t->cr3) {
        vmm_destroy_address_space((uint64_t*)t->cr3);
        t->cr3 = 0;
    }

    // Остался ядерный стек (его отдаст сборщик) и страница самой задачи
    t->mem_size = 4096 + (t->kstack_bottom ? (t->is_process ? PROC_KSTACK_SIZE : KERNEL_STACK_SIZE) : 0);
}

void sched_release_mm(task_t* t) {
    if (!t) return;
    sched_release_space(t);
    if (!t->mm_freed) return;            // пространство ещё занято -- стек тоже рано

    if (t->kstack_bottom) {
        // У процесса стек PROC_KSTACK_SIZE, у потока ядра -- KERNEL_STACK_SIZE
        uint64_t kst_phys = virt_to_phys(t->kstack_bottom);
        uint32_t npages = (t->is_process ? PROC_KSTACK_SIZE : KERNEL_STACK_SIZE) / 4096;
        for (uint32_t i = 0; i < npages; i++)
            pmm_free_page((void*)(kst_phys + (uint64_t)i * 4096));
        t->kstack_bottom = 0;
        t->kstack_top = 0;
        if (current_task && current_task->is_process) set_tss_rsp0(current_task->kstack_top);
        else reset_tss_to_default();
    }

    t->mem_size = 4096;                     // осталась только карточка задачи
}

// init собирает сирот: зомби, которых никто не ждёт (родитель умер раньше или это
// поток ядра без родителя). Вызывается из главного цикла kernel_main.
int sched_reap_orphans(void) {
    int reaped = 0;

    // Сначала освобождаем память у всех зомби -- и у тех, чьего родителя
    // ждать бессмысленно (композитор за своими детьми не приходит).
    // Собираем список зомби и проходим по нему: раньше цикл на первой же
    // неудаче (задача ещё на своём CR3) обрывался, и все зомби за ней ждали
    // следующего круга -- а если первый застревал надолго, то и навсегда.
    {
        task_t* zb[32];
        int zn = 0;
        uint64_t fl;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
        if (task_list_head) {
            task_t* c = task_list_head;
            do {
                if (c->state == TASK_ZOMBIE && c != current_task &&
                    (!c->mm_freed || c->kstack_bottom) && zn < 32) zb[zn++] = c;
                c = c->next;
            } while (c != task_list_head);
        }
        __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
        for (int i = 0; i < zn; i++) sched_release_mm(zb[i]);
    }

    while (1) {
        uint64_t fl;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
        int pid = -1;
        if (task_list_head) {
            task_t* c = task_list_head;
            do {
                if (c->state == TASK_ZOMBIE && c != current_task && c != init_task &&
                    (c->parent == NULL || c->parent == init_task)) {
                    pid = (int)c->pid;
                    break;
                }
                c = c->next;
            } while (c != task_list_head);
        }
        __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
        if (pid < 0) break;
        int code = 0;
        if (!sched_reap_zombie(NULL, pid, &code)) break;
        reaped++;
    }
    return reaped;
}
