[BITS 16]
section .trampoline
global smp_trampoline_start
global smp_trampoline_end
global ap_ready_counter
global ap_work_flag

; Этот блок ядро скопирует по физическому адресу 0x8000
smp_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; 1. Загружаем временный GDT
    lgdt [gdt32_desc - smp_trampoline_start + 0x8000]

    ; 2. Включаем 32-битный Protected Mode
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:(0x8000 + (ap_pm_start - smp_trampoline_start))

[BITS 32]
ap_pm_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; 3. Включаем PAE в CR4
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9)   ; PAE + OSFXSR
    mov cr4, eax

    ; 4. Подключаем таблицы страниц ядра devOS (PML4 = 0x70000)
    mov eax, 0x70000
    mov cr3, eax

    ; 5. Включаем Long Mode в EFER (0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)              ; LME
    wrmsr

    ; 6. Включаем пейджинг (CR0.PG)
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; 7. Загружаем 64-битную GDT (ту же, что в entry_kernel.asm)
    lgdt [gdt64_desc_ap - smp_trampoline_start + 0x8000]

    ; 8. Прыжок в Long Mode!
    jmp 0x08:(0x8000 + (ap_long_mode - smp_trampoline_start))

[BITS 64]
ap_long_mode:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Атомарно сообщаем ядру, что ядро успешно проснулось в 64-битном режиме!
    lock inc dword [0x8000 + (ap_ready_counter - smp_trampoline_start)]

.ap_wait_loop:
    ; Ядро ждет команды: если ap_work_flag == 1, начинаем греть АЛУ на 100%!
    cmp dword [0x8000 + (ap_work_flag - smp_trampoline_start)], 1
    jne .check_halt

    ; СУПЕРСКАЛЯРНЫЙ РАЗОГРЕВ ДЛЯ ДОПОЛНИТЕЛЬНЫХ ЯДЕР:
    add rax, 1
    xor rbx, rax
    imul rdx, rbx, 31
    jmp .ap_wait_loop

.check_halt:
    cmp dword [0x8000 + (ap_work_flag - smp_trampoline_start)], 2
    je .ap_park

    pause
    jmp .ap_wait_loop

.ap_park:
    hlt
    jmp .ap_park

; --- Вспомогательные дескрипторы прямо внутри страницы 0x8000 ---
align 4
gdt32_start:
    dq 0
    dq 0x00CF9A000000FFFF         ; 0x08: Code 32
    dq 0x00CF92000000FFFF         ; 0x10: Data 32
gdt32_desc:
    dw $ - gdt32_start - 1
    dd 0x8000 + (gdt32_start - smp_trampoline_start)

align 8
gdt64_desc_ap:
    dw 0x28
    dq 0x8000 + (gdt64_ap_table - smp_trampoline_start)

gdt64_ap_table:
    dq 0
    dq 0x00209A0000000000         ; 0x08: 64-bit Code Ring 0
    dq 0x0000920000000000         ; 0x10: 64-bit Data Ring 0

align 4
ap_ready_counter: dd 0
ap_work_flag:      dd 0

smp_trampoline_end: