[BITS 32]
section .text
global _start
extern kernel_main
extern __bss_start          ; границы .bss из linker.ld (виртуальные адреса Higher-Half)
extern __bss_end

; Адреса 4-уровневых таблиц страниц (размещаем в безопасной зоне 0x70000 - 0x73000)
PAGE_TABLE_PML4 equ 0x70000
PAGE_TABLE_PDPT equ 0x71000
PAGE_TABLE_PD   equ 0x72000

_start:
    cli
    cld                         ; DF мог остаться 1 после BIOS/загрузчика: rep stos/movs пошли бы назад
    clts                        ; CR0.TS мог остаться 1: первая FPU/SSE-инструкция дала бы #NM

    ; 1. Включаем сопроцессор FPU x87 в CR0
    mov eax, cr0
    and eax, ~(1 << 2)          ; Сброс бита EM (Emulation)
    or eax, (1 << 1)            ; Установка бита MP (Monitor Coprocessor)
    mov cr0, eax
    fninit

    ; 2. Включаем поддержку SSE/SSE2 и выставляем бит PAE (Physical Address Extension) в CR4
    mov eax, cr4
    or eax, (1 << 5)            ; Бит 5 = PAE (ОБЯЗАТЕЛЕН для Long Mode!)
    or eax, (1 << 9) | (1 << 10); Бит 9 = OSFXSR, Бит 10 = OSXMMEXCPT
    mov cr4, eax

    ; 3. Проверка процессора на поддержку 64-битного режима (CPUID.LongMode)
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)         ; Бит 29 = Long Mode
    jz no_long_mode

    ; 4. Построение 4-уровневых таблиц страниц (Identity Mapping + Higher-Half Kernel)
    mov edi, PAGE_TABLE_PML4
    mov ecx, 4096 * 6 / 4
    xor eax, eax
    rep stosd

    ; Запись в PML4 для младшей половины (Identity Mapping 0-4 ГБ) -> указывает на PDPT 
    mov dword [PAGE_TABLE_PML4], PAGE_TABLE_PDPT | 0x07

    ; Поддерживаем ОБА варианта адресации Higher-Half Kernel:
    mov dword [PAGE_TABLE_PML4 + 2048], PAGE_TABLE_PDPT | 0x07  ; Индекс 256 (0xFFFF8000...)
    mov dword [PAGE_TABLE_PML4 + 4088], PAGE_TABLE_PDPT | 0x07  ; Индекс 511 (0xFFFFFFFF80...)

    ; В PDPT создаем записи, покрывающие первые 4 ГБ (по 1 ГБ на каждую таблицу PD) 
    mov dword [PAGE_TABLE_PDPT],      PAGE_TABLE_PD         | 0x07  ; Индекс 0 
    mov dword [PAGE_TABLE_PDPT + 8],  PAGE_TABLE_PD + 4096  | 0x07  ; Индекс 1 
    mov dword [PAGE_TABLE_PDPT + 16], PAGE_TABLE_PD + 8192  | 0x07  ; Индекс 2 
    mov dword [PAGE_TABLE_PDPT + 24], PAGE_TABLE_PD + 12288 | 0x07  ; Индекс 3 
    mov dword [PAGE_TABLE_PDPT + 4080], PAGE_TABLE_PD       | 0x07  ; Индекс 510

    ; Заполняем PD записями по 2 МБ (Huge Pages) — всего 2048 записей (4 ГБ)
    mov edi, PAGE_TABLE_PD
    mov ebx, 0x00000087         ; Present | Writable | User | Page Size 2MB (0x80)
    mov ecx, 2048               ; 2048 * 2 МБ = 4096 МБ (4 ГБ)

.fill_pd:
    mov [edi], ebx
    mov dword [edi + 4], 0      ; Старшие 32 бита базы = 0
    add ebx, 0x200000           ; Шаг +2 МБ
    add edi, 8                  ; Шаг дескриптора в Long Mode = 8 байт
    loop .fill_pd

    ; 5. Загружаем адрес PML4 в регистр корня пейджинга CR3
    mov eax, PAGE_TABLE_PML4
    mov cr3, eax

    ; 6. Активируем Long Mode в модельно-зависимом регистре MSR EFER (0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)            ; Устанавливаем бит LME (Long Mode Enable)
    wrmsr

    ; 7. Включаем пейджинг в CR0
    mov eax, cr0
    or eax, (1 << 31)           ; Бит 31 = PG (Paging Enable)
    mov cr0, eax

    ; 8. Загружаем 64-битную таблицу дескрипторов GDT
    lgdt [gdt64_desc]

    ; 9. Совершаем дальний переход в 64-битный сегмент кода 0x08!
    jmp 0x08:entry64

no_long_mode:
    cli
.freeze:
    hlt
    jmp .freeze

; ==============================================================================
; ЧИСТЫЙ 64-БИТНЫЙ РЕЖИМ (LONG MODE x86_64) С ВИЗУАЛЬНЫМИ ЧЕКПОИНТАМИ
; ==============================================================================
[BITS 64]
; Убираем default rel, он здесь вредит!

entry64:
    ; Обнуляем регистры данных
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Сегмент стека настраиваем на селектор данных ядра (0x10)
    mov ax, 0x10
    mov ss, ax

    ; Загрузочный стек ядра: физически 0x1E00000-0x1F00000 (вершина 0x1F00000), растёт вниз.
    ; Виртуально — в Higher-Half: 0xFFFFFFFF80000000 + 0x01F00000 = 0xFFFFFFFF81F00000.
    ; Раньше стек был на 15 МБ и упирался в .bss (см. карту памяти в linker.ld).
    mov rsp, 0xFFFFFFFF81F00000
    mov rbp, rsp

    ; Чистые RFLAGS (стек уже валиден): IF=0, DF=0, NT=0, AC=0, IOPL=0
    push qword 2
    popfq

    ; --- "Смываем за BIOS": обнуляем .bss ---------------------------------------
    ; kernel.bin содержит только .text/.rodata/.data, а .bss в образе НЕТ — она
    ; занимает ОЗУ, где после BIOS лежит мусор (в QEMU там случайно нули, на железе — нет).
    ; Без этого любая статическая переменная "без значения" стартует со случайным числом.
    ; Стек (0x1E00000-0x1F00000) и таблицы страниц (0x70000) лежат вне [__bss_start, __bss_end).
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    shr rcx, 3                  ; размер кратен 4096 (см. linker.ld), делим на 8 для stosq
    xor eax, eax
    rep stosq

    ; [ФИНАЛЬНОЕ ИСПРАВЛЕНИЕ]: 
    ; Абсолютная загрузка полного 64-битного виртуального адреса функции.
    ; Мы используем косвенный прыжок через адрес в памяти (переменную).
    mov rax, qword [kernel_main_ptr]
    call rax

    cli
.halt64:
    hlt
    jmp .halt64

; Определяем локальную переменную, содержащую абсолютный 64-битный адрес kernel_main
align 8
kernel_main_ptr:
    dq kernel_main

; ------------------------------------------------------------------------------
; 64-БИТНАЯ ТАБЛИЦА GDT
; ------------------------------------------------------------------------------
align 16
gdt64_start:
    dq 0                        ; 0x00: Нулевой дескриптор
gdt64_code:
    dq 0x00209A0000000000       ; 0x08: Kernel Code (Ring 0)
gdt64_data:
    dq 0x0000920000000000       ; 0x10: Kernel Data (Ring 0)
gdt64_user_code:
    dq 0x0020FA0000000000       ; 0x18: User Code (Ring 3)
gdt64_user_data:
    dq 0x0000F20000000000       ; 0x20: User Data (Ring 3)
global gdt64_tss
gdt64_tss:
    dq 0                        ; 0x28: TSS Descriptor (часть 1)
    dq 0                        ; 0x30: TSS Descriptor (часть 2)

gdt64_desc:
    dw $ - gdt64_start - 1
    dq gdt64_start