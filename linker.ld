OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    /* 1. Загрузочный стаб на физическом и виртуальном адресе 1 МБ */
    . = 0x100000;
    .startup : {
        build/entry_kernel.o(.text)
        build/entry_kernel.o(.rodata)
        build/entry_kernel.o(.data)
        build/entry_kernel.o(.bss)
    }

    /* Выравниваем границу перед ядром */
    . = ALIGN(4096);
    kernel_physical_start = .;

    /* 2. Основное ядро. Смещаем VMA ровно на базу Higher-Half (0xFFFFFFFF80000000)
       от текущего физического адреса, чтобы пагинация работала пиксель в пиксель! */
    . = kernel_physical_start + 0xFFFFFFFF80000000;

    .text ALIGN(4096) : AT(kernel_physical_start) {
        *(EXCLUDE_FILE(*entry_kernel.o) .text .text.*)
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - 0xFFFFFFFF80000000) {
        *(EXCLUDE_FILE(*entry_kernel.o) .rodata .rodata.*)
    }

    .data ALIGN(4096) : AT(ADDR(.data) - 0xFFFFFFFF80000000) {
        *(EXCLUDE_FILE(*entry_kernel.o) .data .data.*)
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - 0xFFFFFFFF80000000) {
        __bss_start = .;
        *(EXCLUDE_FILE(*entry_kernel.o) COMMON)
        *(EXCLUDE_FILE(*entry_kernel.o) .bss .bss.*)
        . = ALIGN(4096);
        __bss_end = .;
    }

    /* Карта физической памяти ядра:
         0x000000 - 0x0FFFFF   BIOS, IVT, VBE Info, таблицы страниц (0x70000)
         0x100000 - 0x1DFFFFF  образ ядра (.text/.rodata/.data) и .bss
         0x1E00000 - 0x1EFFFFF  загрузочный стек ядра (1 МБ, вершина 0x1F00000, растёт вниз)
         0x1F00000 - 0x1FFFFFF  битмап PMM (pmm.c)
         0x2000000 - 0x23FFFFF  зарезервировано (раньше SYS_LOAD_BASE)
       .bss не должна доходить до стека — иначе стек затирает статические буферы. */
    ASSERT(__bss_end - 0xFFFFFFFF80000000 <= 0x1E00000,
           "BSS слишком большая: перекрывается с загрузочным стеком (0x1E00000)")

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
