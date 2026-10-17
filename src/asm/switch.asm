bits 64
section .text

global switch_to
; Прототип C: void switch_to(task_t* prev, task_t* next);
; Аргументы по System V ABI:
;   RDI = prev
;   RSI = next

switch_to:
    ; 0. Сохраняем состояние FPU/SSE уходящей задачи (prev->fpu, смещение 16)
    mov rax, [rdi + 16]
    test rax, rax
    jz .no_fpu_save
    fxsave [rax]
.no_fpu_save:

    ; 1. Сохраняем callee-saved регистры текущей задачи на её стеке
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; 2. Сохраняем текущий RSP в prev->rsp (смещение 0 в структуре task_t)
    mov [rdi], rsp

    ; 3. Загружаем RSP следующей задачи из next->rsp
    mov rsp, [rsi]

    ; 4. Переключаем виртуальную память (CR3), если адресное пространство отличается
    mov rax, [rsi + 8]          ; next->cr3 (смещение 8)
    mov rdx, cr3
    cmp rax, rdx
    je .skip_cr3
    test rax, rax
    jz .skip_cr3
    mov cr3, rax
.skip_cr3:

    ; 5. Восстанавливаем сохраненные регистры новой задачи
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; 6. Восстанавливаем состояние FPU/SSE возобновляемой задачи (next->fpu).
    ;    RSI здесь всё ещё указывает на next: регистр caller-saved и не менялся.
    mov rax, [rsi + 16]
    test rax, rax
    jz .no_fpu_restore
    fxrstor [rax]
.no_fpu_restore:

    ; 7. Переход на точку возобновления (RIP с вершины стека)
    ret