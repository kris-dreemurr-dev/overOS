#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>
#include "../fs/vfs.h"

// Минимальный набор сигналов (как просил план): оба пока не перехватываются —
// обработчиков ("signal handlers") в системе нет, поэтому и SIGKILL, и SIGINT
// всегда завершают задачу. Это ровно то, что нужно для `kill` и Ctrl+C.
#define SIGINT  2
#define SIGKILL 9

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_DEAD,
    TASK_ZOMBIE      // завершился, ждёт wait() от родителя
} task_state_t;

typedef struct task {
    uint64_t        rsp;           // Смещение 0: текущий стек (для switch.asm)[cite: 16]
    uint64_t        cr3;           // Смещение 8: физ. адрес PML4 (для switch.asm)[cite: 16]
    uint64_t        fpu;           // Смещение 16: адрес области FXSAVE (для switch.asm)
    uint64_t        kstack_top;    // вершина стека[cite: 16]
    uint64_t        kstack_bottom; // Физическое начало стека в RAM[cite: 16]
    uint64_t        mem_size;      // Выделенная память (в байтах)[cite: 16]
    uint64_t        pid;           // Идентификатор процесса[cite: 16]
    char            name[16];      // Имя задачи[cite: 16]
    task_state_t    state;         // Состояние[cite: 16]
    struct task*    next;          // Следующий в Round-Robin[cite: 16]
    
    // Границы динамической памяти (Heap / brk)
    uint64_t        heap_start;    // Начало кучи (сразу за кодом)
    uint64_t        heap_end;
    int             tty_id;        // -1 = не привязана к TTY

    // Состояние Ring 3 (у каждой задачи своё, переносится в sched_yield)
    uint64_t        saved_krsp;    // saved_kernel_rsp: низ ring0-стека для входа из Ring 3
    int             in_user;       // 1 = задача в Ring 3 или внутри своего сисколла
    
    // UNIX-WAY: Таблица файловых дескрипторов процесса вместо open83[64]
    file_descriptor_t* fd_table[MAX_FD];
    char               cwd[128];

    // Самостоятельный процесс Ring 3 (свой ядерный стек), см. sched_spawn_process
    struct task*    parent;        // кто ждёт завершения (оболочка, запустившая программу)
    int             exit_code;
    int             is_process;    // 1 = процесс Ring 3, а не поток ядра
    uint64_t        user_entry;    // стартовый RIP в Ring 3
    uint64_t        user_stack_top;// стартовый RSP в Ring 3

    // Учётные данные (UNIX DAC): наследуются при spawn/fork, меняются через setuid/login
    int             uid;
    int             gid;

    // Ожидание в TASK_SLEEPING: планировщик сам будит по времени и/или по сообщению IPC
    char            arg[128];      // аргумент запуска (путь к файлу для notepad и т.п.), sys_get_arg
    uint32_t        wake_tick;     // timer_ticks, когда проснуться (WAIT_TIMER)
    uint8_t         wait_flags;    // WAIT_*
    volatile uint8_t ipc_pending;  // в почтовом ящике есть сообщения
    uint8_t         mm_freed;      // память зомби уже отдана (см. sched_release_mm)
    // Сколько тиков таймера (а он идёт на 1000 Гц, то есть миллисекунд)
    // процессор отработал на этой задаче. Начисляет timer_isr_handler тому,
    // кто был на процессоре в момент прерывания, — так же считает загрузку
    // /proc/stat в Linux. Отсюда проценты в диспетчере задач.
    uint64_t        cpu_ticks;
    // Полный путь к исполняемому файлу (как /proc/<pid>/exe): по нему рабочий
    // стол берёт из .prg имя и значок программы -- для окна, панели и
    // уведомлений. Подделать нельзя: пишет его загрузчик, а не программа.
    char            exe[96];
    uint8_t         fpu_state[512] __attribute__((aligned(16)));
} task_t;

// switch.asm читает rsp, cr3 и fpu по жёстко зашитым смещениям — проверяем их
// на этапе компиляции, иначе новое поле тихо испортило бы переключение задач
typedef char task_offsets_check[
    (offsetof(task_t, rsp) == 0 && offsetof(task_t, cr3) == 8 && offsetof(task_t, fpu) == 16) ? 1 : -1];


#define WAIT_TIMER 0x01
#define WAIT_IPC   0x02

// Ядерный стек самостоятельного процесса (sched_spawn_process, sys_fork)
#define PROC_KSTACK_SIZE (64 * 1024)

int  sched_tty_is_active(void);

void sched_task_fpu_capture(task_t* t);
void sched_init(void);
task_t* task_create_kernel(void (*entry)(void), const char* name);
void sched_yield(void);

// Функция диспетчера задач
void sched_dump_tasks(void);

// Путь к исполняемому файлу процесса pid (sys_proc_exe). 0 -- нашёлся.
int  sched_proc_exe(uint64_t pid, char* out, int max);
// Запомнить путь к исполняемому файлу (относительный -- от каталога cwd)
void sched_set_exe(task_t* t, const char* path, const char* cwd);

extern void switch_to(task_t* prev, task_t* next);

task_t* sched_get_current_task(void);

uint64_t sched_alloc_pid(void);

task_t* sched_register_user_task(const char* name, uint64_t cr3, uint64_t mem_size);
void    sched_remove_user_task(task_t* task);

task_t* task_create_user_process(void (*entry_point)(void), uint64_t cr3_val, const char* name);

// Процессы (модель Unix: spawn + wait + exit)
void sched_start_process(task_t* t);   // пустить на выполнение созданный процесс
task_t* sched_spawn_process(const char* name, uint64_t cr3, uint64_t mem_size,
                            uint64_t user_entry, uint64_t user_stack_top, uint64_t heap_start);
int     sched_wait_child(task_t* child);   // родитель спит до завершения; освобождает всё, возвращает код выхода

// То же самое, но по PID (для sys_waitpid из Ring 3, где task_t* недоступен).
// Ждёт только СВОЕГО ребёнка (child->parent == вызывающая задача).
// -1, если такого PID среди детей вызывающей задачи нет (уже забран другим wait,
// никогда не существовал, или это не её ребёнок).
// Возвращает 1, если найден и собран СВОЙ ребёнок с данным pid (его код выхода
// кладётся в *out_code — он может быть отрицательным, если ребёнка убил kill(),
// это не признак ошибки). Возвращает 0 (и *out_code не трогает), если это не
// наш ребёнок / уже не существует — в отличие от предыдущей версии, "не нашли"
// и "код выхода -1" теперь не могут быть перепутаны: это разные каналы.
int sched_wait_pid(uint64_t pid, int* out_code);

// Посылает сигнал задаче с данным PID. Поскольку планировщик однопроцессорный
// и кооперативный, цель в момент вызова (из ДРУГОЙ задачи) физически не может
// сама сейчас исполняться — она READY или SLEEPING, поэтому можно завершить её
// немедленно, без очереди "отложенных" сигналов. Если pid — это сам вызывающий,
// он должен звать sched_exit_current() напрямую (себя так не "убьёшь" снаружи).
// Возвращает 0 при успехе, -1 если такого PID нет или он уже не жив.
int sched_send_signal(uint64_t pid, int sig);
void    sched_exit_current(int code);      // завершает текущий процесс (не возвращается)

// Добавить уже заполненную задачу в кольцо планировщика (под cli)
void sched_enqueue_task(task_t* t);

void sched_set_current_task(task_t* t);

void sched_set_foreground_task(task_t* t);
task_t* sched_get_foreground_task(void);

// Неблокирующий сбор завершившегося потомка (для sys_waitpid и очистки зомби)
// Запись таблицы задач для ps(1) в Ring 3
typedef struct {
    int      pid;
    int      uid;
    int      tty;          // -1 — поток ядра без TTY
    int      state;        // TASK_*
    int      is_process;   // 1 — процесс Ring 3, 0 — поток ядра
    uint32_t mem_kb;
    uint64_t cpu_ms;       // процессорное время с момента запуска (тик = 1 мс)
    char     name[16];
} sched_proc_info_t;

int sched_task_list(sched_proc_info_t* out, int max);

int sched_reap_zombie(task_t* parent, int target_pid, int* out_code);
// init-сборка сирот: зомби без живого родителя (возвращает, сколько собрал)
int sched_reap_orphans(void);
// Отдать память завершившегося процесса, не дожидаясь wait() от родителя
void sched_release_mm(task_t* t);
// Только адресное пространство и файлы (ядерный стек остаётся): зовётся на выходе
void sched_release_space(task_t* t);

int sched_pid_alive(uint64_t pid);
int sched_pid_uid(uint64_t pid);
int sched_pid_is_process(uint64_t pid);
// Пришло сообщение IPC: отметить и разбудить, если задача спит в ожидании сообщения
void sched_wake_ipc(uint64_t pid);
// Уснуть на ms миллисекунд (задача не выбирается планировщиком, процессор простаивает)
void sched_sleep_ms(uint32_t ms);

#endif