/* C-часть Stage 2 (меню VBE): плоский бинарник, ORG 0x2000 (сразу за asm-частью Stage 2).
 * .data и .bss лежат в одном выходном разделе: .bss попадает в файл нулями (иначе загрузчик
 * не обнулил бы статические переменные). Всё должно кончаться до 0x5000, где стоят буферы FAT32. */
OUTPUT_FORMAT(binary)
ENTRY(menu_main)

SECTIONS
{
    . = 0x2000;
    .text   : { *(.text.entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }

    ASSERT(. <= 0x5000, "C-часть Stage 2 не влезла в 0x2000-0x4FFF (12 КиБ)")
}
