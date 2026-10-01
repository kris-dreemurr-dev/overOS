; boot_stage2.asm - Stage 2 (лёгкая версия)
;
; Stage 1 (MBR) читает 32 сектора (LBA 1..32 = 16 КиБ) в 0x1000 и прыгает сюда. Образ состоит из двух частей:
;
;   0x1000 - 0x1FFF  этот файл (asm): вход, баннер, вызов частей, переход в защищённый режим
;                    + boot_fat32.asm (%include): A20, Unreal Mode, загрузка /sys/kernel.bin из FAT32
;   0x2000 - 0x4FFF  vbe_menu.c, скомпилированный gcc -m16 (incbin "boot_menu.bin"):
;                    опрос видеорежимов VBE, меню выбора разрешения, установка режима
;
; Порядок работы (как раньше): ядро грузится ДО опроса VBE, потому что буфер диска (0x7E00-0x9DFF)
; пересекается с VBE-блоками 0x7000/0x8000, а ядро ждёт по 0x8000 ModeInfoBlock выбранного режима.

[ORG 0x1000]
[BITS 16]

MENU_ENTRY      equ 0x2000          ; menu_main() из vbe_menu.c (первая функция в stage2_menu.ld)

stage2_start:
    cli
    cld
    mov [boot_drive], dl

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7C00                 ; ВЕСЬ ESP: C-код (gcc -m16) адресует стек 32-битными регистрами
    sti

    ; Скрываем мигающий курсор
    mov ah, 0x01
    mov ch, 0x20
    int 0x10

    ; Очистка экрана
    mov ax, 0x0003
    int 0x10

    mov si, msg_banner
    call print
    mov cx, 0x0004
    mov dx, 0x0000
    call sleep_real

    mov si, msg_init_sys
    call print

    ; A20 + Unreal Mode (boot_fat32.asm)
    call enable_a20
    call enter_unreal

    ; Загрузка ядра ИЗ ФАЙЛА /sys/kernel.bin -> 0x100000 (boot_fat32.asm)
    mov si, msg_fat_load
    call print
    call fat_load_kernel

    mov si, msg_ok
    call print
    mov cx, 0x0003
    mov dx, 0x0000
    call sleep_real

    ; Меню VBE и установка видеорежима (vbe_menu.c). Возврат через 32-битный RET (retl):
    ; "call dword" кладёт в стек 4 байта адреса возврата. Остановка при ошибке — внутри.
    call dword MENU_ENTRY

    ; Переход в защищённый режим и прыжок в ядро
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm_start

; -----------------------------------------------------------------------------
; Вывод строки через BIOS и пауза
; -----------------------------------------------------------------------------
print:
    push ax
    push bx
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

sleep_real:
    push ax
    mov ah, 0x86
    int 0x15
    pop ax
    ret

%include "boot_fat32.asm"

[BITS 32]
pm_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x90000
    jmp 0x100000

align 16
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF, 0, 0x9A00, 0x00CF
gdt_data:
    dw 0xFFFF, 0, 0x9200, 0x00CF
gdt_desc:
    dw $ - gdt_start - 1
    dd gdt_start

; --- Данные ---
boot_drive:      db 0x80

msg_banner:      db "[ overOS Stage 2 Initializing ]", 13, 10, 0
msg_init_sys:    db "[ asm ] Preparing system & loading kernel...", 13, 10, 0
msg_ok:          db " [ OK ]", 13, 10, 0

; asm-часть занимает 0x1000-0x1FFF. Если не влезет — NASM остановит сборку ("TIMES value is negative").
times 0x1000 - ($ - $$) db 0

; C-часть, слинкованная на 0x2000 (см. stage2_menu.ld). Если она больше 12 КиБ — ошибка на следующей строке.
incbin "boot_menu.bin"

; Stage 2 = ровно 32 сектора (LBA 1..32, их читает Stage 1); сборка падает, если больше 16 КиБ.
times 16384 - ($ - $$) db 0
