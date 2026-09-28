ENTRY(driver_entry)

SECTIONS
{
    . = 0;                      /* база 0: значения в образе = смещения */

    .header : { KEEP(*(.header)) }
    .text   : ALIGN(16) { *(.text*) }
    .rodata : ALIGN(16) { *(.rodata*) }
    .data   : ALIGN(16) { *(.data*) }
    .bss    : ALIGN(16) { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
