bits 64
section .text

extern default_exception_handler
extern timer_isr_handler
global timer_isr_asm

; Макрос для исключений БЕЗ кода ошибки
%macro ISR_NOERRCODE 1
global isr_%1
isr_%1:
    push qword 0    ; Фейковый код ошибки
    push qword %1   ; Номер вектора прерывания
    jmp interrupt_common
%endmacro

; Макрос для исключений С кодом ошибки
%macro ISR_ERRCODE 1
global isr_%1
isr_%1:
    push qword %1   ; Номер вектора прерывания
    jmp interrupt_common
%endmacro

; Общий обработчик контекста
interrupt_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call default_exception_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax

    add rsp, 16
    iretq

; Объявляем стандартные исключения x86_64
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13  ; General Protection Fault
ISR_ERRCODE   14  ; Page Fault
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19

%assign i 20
%rep 12
    ISR_NOERRCODE i
%assign i i+1
%endrep

; Обработчик для таймера PIT (Вектор 32)
timer_isr_asm:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov al, 0x20
    out 0x20, al         ; Сигнал EOI контроллеру PIC

    call timer_isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq