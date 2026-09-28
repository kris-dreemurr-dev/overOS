[BITS 64]
global _start
extern main

section .text
_start:
    xor rbp, rbp
    mov rdi, 0          ; argc = 0
    mov rsi, 0          ; argv = NULL

    call main           ; Запуск Си-программы

    mov rbx, rax        ; Код возврата из main
    mov rax, 0          ; Сисколл exit (0)
    int 0x80

.halt:
    jmp .halt           ; Стопор, если ядро вернет управление