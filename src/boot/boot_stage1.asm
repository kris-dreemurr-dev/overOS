[ORG 0x7C00]
[BITS 16]

start:
    cli
    cld
    mov [boot_drive], dl

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; 1. Проверка LBA расширений
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .fail

    ; 2. Читаем Stage 2 (32 сектора = 16 КБ, LBA 1..32) в 0x0000:0x1000
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, stage2_dap
    int 0x13
    jc .fail

    ; Переход в Stage 2
    mov dl, [boot_drive]
    jmp 0x0000:0x1000

.fail:
    mov si, msg_s1_err
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x000C
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

boot_drive: db 0x80

align 4
stage2_dap:
    db 0x10
    db 0
    dw 32           ; 32 сектора
    dw 0x1000       ; Смещение 0x1000
    dw 0x0000       ; Сегмент 0x0000 (физический 0x1000)
    dq 1            ; LBA 1

msg_s1_err: db "[ s1 ] Disk Read Error!", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55