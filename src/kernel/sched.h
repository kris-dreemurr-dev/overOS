#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_DEAD
} task_state_t;

typedef struct task {
    uint64_t        rsp;           // Смещение 0: текущий стек (для switch.asm)[cite: 16]
    uint64_t        cr3;           // Смещение 8: физ. адрес PML4 (для switch.asm)[cite: 16]
    uint64_t        kstack_top;    // Смещение 16: вершина стека[cite: 16]
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
    char            open83[12];    // файл, открытый sys_open (свой у каждой программы)
} task_t;

int  sched_tty_is_active(void);

void sched_init(void);
task_t* task_create_kernel(void (*entry)(void), const char* name);
void sched_yield(void);

// Функция диспетчера задач
void sched_dump_tasks(void);

extern void switch_to(task_t* prev, task_t* next);

task_t* sched_get_current_task(void);

uint64_t sched_alloc_pid(void);

task_t* sched_register_user_task(const char* name, uint64_t cr3, uint64_t mem_size);
void    sched_remove_user_task(task_t* task);

task_t* task_create_user_process(void (*entry_point)(void), uint64_t cr3_val, const char* name);

void sched_set_current_task(task_t* t);

void sched_set_foreground_task(task_t* t);
task_t* sched_get_foreground_task(void);

#endif