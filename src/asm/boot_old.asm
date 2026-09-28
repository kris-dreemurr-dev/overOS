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

    ; 1. Включение A20 через BIOS
    mov ax, 0x2401
    int 0x15

    ; 2. Проверка LBA расширений
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc halt

    ; 3. Переход в Unreal Mode (Flat Real Mode)
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp $+2

    mov ax, 0x10
    mov fs, ax
    mov es, ax          ; Расширяем лимит ES до 4 ГБ

    mov eax, cr0
    and al, 0xFE        ; Возврат в реальный режим с сохранением лимита ES
    mov cr0, eax
    jmp 0x0000:.unreal_ok

.unreal_ok:
    xor ax, ax
    mov es, ax
    mov fs, ax
    mov ds, ax
    sti

    ; 4. ЗАГРУЗКА ЯДРА НА 1 МБ (До VBE, как в рабочем бинарнике!)
    mov cx, 0x40        ; 64 итерации по 16 секторов = 1024 сектора (512 КБ)
    mov edi, 0x100000   ; Целевой физический адрес в 32-битной памяти

.read_loop:
    push cx
    mov word [dap + 2], 16          ; Читаем по 16 секторов за раз
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    jc halt

    ; Копируем из буфера 0x7E00 в 0x100000 через плоский ES
    mov esi, 0x7E00
    mov ecx, (16 * 512) / 4         ; 2048 двойных слов
    a32 rep movsd

    ; Сдвигаем LBA в DAP на 16 секторов вперед
    add dword [dap_lba], 16
    pop cx
    loop .read_loop

    ; 5. ИНИЦИАЛИЗАЦИЯ VBE 1024x768x32 ПОСЛЕ ЧТЕНИЯ ЯДРА
    xor ax, ax
    mov es, ax
    mov di, 0x7000
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne halt

    mov ax, [0x700E]
    mov [VBE_PTR_OFF], ax
    mov ax, [0x7010]
    mov [VBE_PTR_SEG], ax

find_vbe_mode:
    mov es, [VBE_PTR_SEG]
    mov si, [VBE_PTR_OFF]
    mov cx, [es:si]
    cmp cx, 0xFFFF
    je halt

    add word [VBE_PTR_OFF], 2

    xor ax, ax
    mov es, ax
    mov di, 0x8000
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne find_vbe_mode

    mov ax, [0x8000]
    test ax, 0x0090
    jz find_vbe_mode

    cmp dword [0x8028], 0
    jz find_vbe_mode

    mov bl, [0x8019]
    cmp bl, 32
    jne find_vbe_mode

    cmp word [0x8012], 1024
    jne find_vbe_mode

    cmp word [0x8014], 768
    jne find_vbe_mode

    or cx, 0x4000
    mov ax, 0x4F02
    mov bx, cx
    int 0x10
    cmp ax, 0x004F
    jne halt

    ; 6. ПЕРЕХОД В ПОЛНОЦЕННЫЙ PROTECTED MODE
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm_start

halt:
    cli
    hlt
    jmp halt

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

; --- ТАБЛИЦА ДЕСКРИПТОРОВ GDT ---
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF, 0, 0x9A00, 0x00CF
gdt_data:
    dw 0xFFFF, 0, 0x9200, 0x00CF
gdt_desc:
    dw $ - gdt_start - 1
    dd gdt_start

; --- ПЕРЕМЕННЫЕ И DAP ---
boot_drive:  db 0x80
VBE_PTR_OFF  dw 0
VBE_PTR_SEG  dw 0

align 4
dap:
    db 0x10
    db 0
    dw 16
    dw 0x7E00
    dw 0x0000
dap_lba:
    dq 1                ; Старт с LBA 1

times 510 - ($ - $$) db 0
dw 0xAA55
