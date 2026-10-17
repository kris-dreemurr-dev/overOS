OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    . = 0x4000000000;

    .text : ALIGN(16) {
        *(.text._start)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    /* Конструкторы глобальных объектов C++: их вызывает ovcxx_init() */
    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        _end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.fini_array*)
    }
}
