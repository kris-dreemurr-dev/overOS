[ORG 0x1000]
[BITS 16]

stage2_start:
    cli
    cld
    mov [boot_drive], dl

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
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

    ; -------------------------------------------------------------------------
    ; 1-3. Инициализация (А20, Unreal, Ядро)
    ; -------------------------------------------------------------------------
    mov si, msg_init_sys
    call print

    ; A20
    mov ax, 0x2401
    int 0x15

    ; Unreal Mode (ES=4GB)
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp $+2
    mov ax, 0x10
    mov fs, ax
    mov es, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp 0x0000:.unreal_ok
.unreal_ok:
    xor ax, ax
    mov es, ax
    mov fs, ax
    mov ds, ax
    sti

    ; Загрузка ядра (2048 секторов = 1 МБ)
    mov cx, 0x40
    mov edi, 0x100000
.read_loop:
    push cx
    mov word [dap + 2], 16
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    jc disk_fail

    mov esi, 0x7E00
    mov ecx, (16 * 512) / 4
    a32 rep movsd

    add dword [dap_lba], 16
    pop cx
    loop .read_loop

    mov si, msg_ok
    call print
    mov cx, 0x0003
    mov dx, 0x0000
    call sleep_real

    ; -------------------------------------------------------------------------
    ; 4. Опрос VBE режимов и заполнение массива
    ; -------------------------------------------------------------------------
    mov si, msg_vbe_probe
    call print

    xor ax, ax
    mov es, ax
    mov di, 0x7000
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne vbe_fail

    mov ax, [0x700E]
    mov [VBE_PTR_OFF], ax
    mov ax, [0x7010]
    mov [VBE_PTR_SEG], ax

    mov word [max_width], 0
    mov word [max_height], 0
    mov word [mode_max], 0xFFFF

find_vbe_mode:
    mov es, [VBE_PTR_SEG]
    mov si, [VBE_PTR_OFF]
    mov cx, [es:si]
    cmp cx, 0xFFFF
    jne .not_end_modes
    jmp near start_menu_loop
.not_end_modes:

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

    mov ax, [0x8012]            ; Ширина
    mov dx, [0x8014]            ; Высота

    cmp ax, [max_width]
    jb .check_table
    ja .new_max
    cmp dx, [max_height]
    jbe .check_table

.new_max:
    mov [max_width], ax
    mov [max_height], dx
    mov [mode_max], cx

.check_table:
    push cx
    mov bx, 0
    mov cx, 8
.tbl_loop:
    push cx
    mov si, bx
    mov cx, [res_w + si]
    cmp ax, cx
    jne .tbl_next_pop
    mov cx, [res_h + si]
    cmp dx, cx
    jne .tbl_next_pop

    pop cx
    pop dx
    mov [res_m + si], dx
    push dx
    jmp .tbl_done

.tbl_next_pop:
    pop cx
    add bx, 2
    dec cx
    jnz .tbl_loop

.tbl_done:
    pop cx
    jmp find_vbe_mode

    ; -------------------------------------------------------------------------
    ; 5. Отрисовка Меню с чистым экраном
    ; -------------------------------------------------------------------------
start_menu_loop:
redraw_menu:
    mov ax, 0x0003
    int 0x10                    ; Исправлено: корректное прерывание BIOS для очистки экрана

    ; Центрированная шапка (колонка 26)
    mov ah, 0x02
    xor bh, bh
    mov dh, 2
    mov dl, 26
    int 0x10
    mov si, menu_title
    mov dl, 0x0F
    call print_bios_string

    ; Верхняя граница коробки (строка 4, колонка 3)
    mov ah, 0x02
    mov dh, 4
    mov dl, 3
    int 0x10
    mov si, box_top
    mov dl, 0x07
    call print_bios_string

    ; Отрисовка 8 пунктов внутри коробки (строки 5-12)
    mov cx, 8
    xor bx, bx
    mov word [current_row], 5

.print_item_loop:
    push cx
    push bx

    ; Выбираем цвет подсветки (0x70 — инверсия GRUB, 0x07 — обычный)
    mov ax, [current_row]
    sub ax, 5
    cmp ax, [selected_item]
    je .sel_col
    mov byte [row_attr], 0x07
    jmp .do_draw
.sel_col:
    mov byte [row_attr], 0x70

.do_draw:
    ; Левая граница коробки (колонка 3)
    mov ah, 0x02
    xor bh, bh
    mov dh, byte [current_row]
    mov dl, 3
    int 0x10
    mov al, 0xB3                ; '│'
    mov [current_attr], byte 0x07
    call print_char_attr_direct

    ; Текст пункта (колонка 6)
    mov ah, 0x02
    mov dh, byte [current_row]
    mov dl, 6
    int 0x10

    mov al, [row_attr]
    mov [current_attr], al

    mov di, line_buffer
    mov byte [di], '['
    inc di
    mov byte [di], ' '
    inc di
    mov ax, [current_row]
    sub ax, 4
    add al, '0'
    mov [di], al
    inc di
    mov byte [di], ' '
    inc di
    mov byte [di], ']'
    inc di
    mov byte [di], ' '
    inc di
    mov byte [di], ' '
    inc di

    mov bx, [esp]
    mov ax, [res_w + bx]
    call int_to_str
    
    mov byte [di], 'x'
    inc di

    mov bx, [esp]
    mov ax, [res_h + bx]
    call int_to_str

    ; --- Авто-выравнивание пробелами до фиксированной позиции статуса ---
.pad_res:
    mov ax, di
    sub ax, line_buffer
    cmp ax, 21                  ; Жесткая колонка для начала статуса
    jge .pad_res_done
    mov byte [di], ' '
    inc di
    jmp .pad_res
.pad_res_done:

    mov bx, [esp]
    mov ax, [res_m + bx]
    cmp ax, 0xFFFF
    je .st_fail

    mov si, str_status_ok
    call strcpy
    jmp .print_text

.st_fail:
    mov si, str_status_fail
    call strcpy

.print_text:
    mov byte [di], 0
    mov si, line_buffer
    call print_bios_string_attr

    ; Правая граница коробки (колонка 76)
    mov ah, 0x02
    xor bh, bh
    mov dh, byte [current_row]
    mov dl, 76
    int 0x10
    mov al, 0xB3
    mov [current_attr], byte 0x07
    call print_char_attr_direct

    pop bx
    pop cx
    inc word [current_row]
    add bx, 2
    dec cx
    jz .loop_done
    jmp near .print_item_loop
.loop_done:

    ; --- Разделитель внутри коробки (строка 13) ---
    mov ah, 0x02
    mov dh, 13
    mov dl, 3
    int 0x10
    mov si, box_mid
    mov dl, 0x07
    call print_bios_string

    ; --- Рендеринг 9-го пункта (AUTO / MAX) на строке 14 ---
    mov ax, 8
    cmp ax, [selected_item]
    je .max_sel_col
    mov byte [row_attr], 0x07
    jmp .do_draw_max
.max_sel_col:
    mov byte [row_attr], 0x70

.do_draw_max:
    ; Левая граница
    mov ah, 0x02
    xor bh, bh
    mov dh, 14
    mov dl, 3
    int 0x10
    mov al, 0xB3
    mov [current_attr], byte 0x07
    call print_char_attr_direct

    ; Текст 9-го пункта
    mov ah, 0x02
    mov dh, 14
    mov dl, 6
    int 0x10

    mov al, [row_attr]
    mov [current_attr], al

    mov di, line_buffer
    mov byte [di], '['
    inc di
    mov byte [di], ' '
    inc di
    mov byte [di], '9'
    inc di
    mov byte [di], ' '
    inc di
    mov byte [di], ']'
    inc di
    mov byte [di], ' '
    inc di
    mov byte [di], ' '
    inc di

    mov si, str_auto_limit
    call strcpy

    mov ax, [max_width]
    call int_to_str
    mov byte [di], 'x'
    inc di
    mov ax, [max_height]
    call int_to_str

    mov si, str_max_tag
    call strcpy
    mov byte [di], 0

    mov si, line_buffer
    call print_bios_string_attr

    ; Правая граница
    mov ah, 0x02
    xor bh, bh
    mov dh, 14
    mov dl, 76
    int 0x10
    mov al, 0xB3
    mov [current_attr], byte 0x07
    call print_char_attr_direct

    ; Нижняя граница коробки (строка 15)
    mov ah, 0x02
    mov dh, 15
    mov dl, 3
    int 0x10
    mov si, box_bot
    mov dl, 0x07
    call print_bios_string

    ; Подсказка внизу
    mov ah, 0x02
    mov dh, 18
    mov dl, 17
    int 0x10
    mov si, menu_prompt
    mov dl, 0x0F
    call print_bios_string

    ; Прячем мигающий курсор в правый нижний угол
    mov ah, 0x02
    xor bh, bh
    mov dh, 24
    mov dl, 79
    int 0x10

    ; -------------------------------------------------------------------------
    ; 6. Опрос клавиатуры
    ; -------------------------------------------------------------------------
wait_input:
    mov ah, 0
    int 0x16

    cmp al, 0x0D
    jne .not_enter
    jmp near do_select
.not_enter:

    cmp ah, 0x48
    jne .not_up
    jmp near move_up
.not_up:

    cmp ah, 0x50
    jne .not_down
    jmp near move_down
.not_down:

    jmp wait_input

move_up:
    cmp word [selected_item], 0
    jne .no_wrap_up
    mov word [selected_item], 8
    jmp near redraw_menu
.no_wrap_up:
    dec word [selected_item]
    jmp near redraw_menu

move_down:
    cmp word [selected_item], 8
    jne .no_wrap_down
    mov word [selected_item], 0
    jmp near redraw_menu
.no_wrap_down:
    inc word [selected_item]
    jmp near redraw_menu

do_select:
    mov bx, [selected_item]
    cmp bx, 8
    je .select_max

    shl bx, 1
    mov cx, [res_m + bx]
    cmp cx, 0xFFFF
    je wait_input

    mov [saved_vbe_mode], cx
    jmp apply_and_boot

.select_max:
    mov cx, [mode_max]
    cmp cx, 0xFFFF
    je wait_input
    mov [saved_vbe_mode], cx
    jmp apply_and_boot

    ; -------------------------------------------------------------------------
    ; 7. Запуск выбранного режима
    ; -------------------------------------------------------------------------
apply_and_boot:
    mov ax, 0x0003
    int 0x10
    mov si, menu_booting
    call print
    mov cx, 0x0005
    mov dx, 0x0000
    call sleep_real

    xor ax, ax
    mov es, ax
    mov di, 0x8000
    mov cx, [saved_vbe_mode]
    mov ax, 0x4F01
    int 0x10

    mov bx, [saved_vbe_mode]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne vbe_fail

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm_start

; -----------------------------------------------------------------------------
; ХЕЛПЕРЫ ВЫВОДА
; -----------------------------------------------------------------------------

print_char_attr:
    push ax
    push bx
    push cx
    push dx

    mov ah, 0x09
    xor bh, bh
    mov bl, [current_attr]
    mov cx, 1
    int 0x10

    mov ah, 0x03
    xor bh, bh
    int 0x10
    inc dl
    mov ah, 0x02
    xor bh, bh
    int 0x10

    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_char_attr_direct:
    push ax
    push bx
    push cx
    push dx
    mov ah, 0x09
    xor bh, bh
    mov bl, [current_attr]
    mov cx, 1
    int 0x10
    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_bios_string_attr:
    push ax
    push si
.char_loop:
    lodsb
    test al, al
    jz .done
    call print_char_attr
    jmp .char_loop
.done:
    pop si
    pop ax
    ret

print_bios_string:
    push ax
    push bx
    push si
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .loop
.done:
    pop si
    pop bx
    pop ax
    ret

int_to_str:
    push ax
    push bx
    push cx
    push dx

    mov bx, 10
    xor cx, cx
.div_loop:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .div_loop

.write_loop:
    pop ax
    add al, '0'
    mov [di], al
    inc di
    loop .write_loop

    pop dx
    pop cx
    pop bx
    pop ax
    ret

strcpy:
    push ax
    push si
.copy:
    lodsb
    test al, al
    jz .end
    mov [di], al
    inc di
    jmp .copy
.end:
    pop si
    pop ax
    ret

disk_fail:
vbe_fail:
    cli
    hlt
    jmp $

sleep_real:
    push ax
    mov ah, 0x86
    int 0x15
    pop ax
    ret

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

print_dec:
    push ax
    push bx
    push cx
    push dx
    xor cx, cx
    mov bx, 10
.div_loop:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .div_loop
.print_loop:
    pop ax
    add al, '0'
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    loop .print_loop
    pop dx
    pop cx
    pop bx
    pop ax
    ret

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
VBE_PTR_OFF      dw 0
VBE_PTR_SEG      dw 0
tmp_mode:        dw 0

current_row:     dw 0
selected_item:   dw 2
current_attr:    db 0x07
row_attr:        db 0x07

max_width:       dw 0
max_height:      dw 0
mode_max:        dw 0xFFFF
saved_vbe_mode:  dw 0

res_w: dw 640, 800, 1024, 1280, 1366, 1600, 1920, 2560
res_h: dw 480, 600,  768,  720,  768,  900, 1080, 1440
res_m: dw 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF

align 4
dap:
    db 0x10
    db 0
    dw 16
    dw 0x7E00
    dw 0x0000
dap_lba:
    dq 33

line_buffer:     times 80 db 0

msg_banner:      db "[ overOS Stage 2 Initializing ]", 13, 10, 0
msg_init_sys:    db "[ asm ] Preparing system & loading kernel...", 13, 10, 0
msg_vbe_probe:   db "[ asm ] Scanning VBE profiles...", 13, 10, 0
msg_ok:          db " [ OK ]", 13, 10, 0

menu_title:      db "overOS Video Mode Selection", 0

box_top:         db 0xDA
                 times 72 db 0xC4
                 db 0xBF, 0
box_mid:         db 0xC3
                 times 72 db 0xC4
                 db 0xB4, 0
box_bot:         db 0xC0
                 times 72 db 0xC4
                 db 0xD9, 0

str_pad_mid:     db "                        ", 0
str_status_ok:   db "[ OK ]", 0
str_status_fail: db "[ ! ] resolution not supported", 0

str_auto_limit:  db "Auto / Hardware Limit : ", 0
str_max_tag:     db "  [ MAX ]", 0

menu_prompt:     db "Use UP/DOWN arrows to navigate, ENTER to select", 0
menu_booting:    db 13, 10, 13, 10, "Booting overOS...", 13, 10, 0