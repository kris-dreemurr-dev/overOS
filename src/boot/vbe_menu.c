/* vbe_menu.c - Stage 2: опрос видеорежимов VBE, меню выбора разрешения, установка режима.
 *
 * Вызывается из boot_stage2.asm (call dword MENU_ENTRY) уже ПОСЛЕ загрузки kernel.bin.
 * Результат для ядра (как и раньше):
 *   0x7000  VbeInfoBlock  (INT 10h AX=4F00)
 *   0x8000  ModeInfoBlock выбранного режима (INT 10h AX=4F01) - kernel.c читает width/pitch/lfb_ptr
 * и экран уже переключён в выбранный режим с линейным framebuffer.
 *
 * Сборка:  gcc -m16 -Os ...  (см. Makefile)
 */
#include <stdint.h>
#include <stddef.h>
#include "bios.h"

/* ---- структуры VBE (стандарт VESA) ---- */
struct vbe_info {                       /* AX=4F00 */
    char     signature[4];              /* "VESA" */
    uint16_t version;
    uint16_t oem_off, oem_seg;
    uint32_t capabilities;
    uint16_t modes_off, modes_seg;      /* far-указатель на список режимов, конец списка = 0xFFFF */
    uint16_t total_memory;
    uint8_t  rest[512 - 20];
} __attribute__((packed));

struct mode_info {                      /* AX=4F01 */
    uint16_t attributes;                /* бит 4 = графика, бит 7 = линейный framebuffer */
    uint8_t  win_a, win_b;
    uint16_t granularity, win_size, seg_a, seg_b;
    uint32_t win_func;
    uint16_t pitch;
    uint16_t width, height;
    uint8_t  x_char, y_char, planes, bpp, banks, memory_model, bank_size, image_pages, reserved0;
    uint8_t  red_size, red_pos, green_size, green_pos, blue_size, blue_pos, rsv_size, rsv_pos, direct_color;
    uint32_t phys_base;                 /* физический адрес framebuffer */
    uint8_t  rest[256 - 44];
} __attribute__((packed));

_Static_assert(offsetof(struct vbe_info, modes_off) == 0x0E, "VideoModePtr");
_Static_assert(offsetof(struct mode_info, pitch) == 0x10, "BytesPerScanLine");
_Static_assert(offsetof(struct mode_info, width) == 0x12, "XResolution");
_Static_assert(offsetof(struct mode_info, bpp) == 0x19, "BitsPerPixel");
_Static_assert(offsetof(struct mode_info, phys_base) == 0x28, "PhysBasePtr");
_Static_assert(sizeof(struct vbe_info) == 512, "VbeInfoBlock");
_Static_assert(sizeof(struct mode_info) == 256, "ModeInfoBlock");

#ifndef HOST_TEST
#define VBE_INFO_BUF   ((struct vbe_info *)0x7000)
#define MODE_INFO_BUF  ((struct mode_info *)0x8000)
#else
extern struct vbe_info  host_vbe_info;
extern struct mode_info host_mode_info;
#define VBE_INFO_BUF   (&host_vbe_info)
#define MODE_INFO_BUF  (&host_mode_info)
#endif

#define VBE_OK          0x004F
#define NO_MODE         0xFFFF
#define ITEMS           8           /* пунктов со стандартными разрешениями; 9-й = "Auto / MAX" */
#define ITEM_MAX        ITEMS

/* стандартные разрешения меню и найденные для них номера режимов VBE (NO_MODE = не поддерживается) */
static const uint16_t res_w[ITEMS] = { 640, 800, 1024, 1280, 1366, 1600, 1920, 2560 };
static const uint16_t res_h[ITEMS] = { 480, 600,  768,  720,  768,  900, 1080, 1440 };
static uint16_t res_mode[ITEMS];

static uint16_t mode_max = NO_MODE;     /* самый большой 32-битный режим */
static uint16_t max_w, max_h;
static uint16_t selected = 2;           /* по умолчанию 1024x768 */

/* ---------------------------------------------------------------- вывод */

static void halt_msg(const char *s)
{
    while (*s)
        bios_tty(*s++);
    halt_forever();
}

static void tty_str(const char *s)
{
    while (*s)
        bios_tty(*s++);
}

static void tty_repeat(char c, int n)
{
    while (n--)
        bios_tty(c);
}

/* строка с атрибутом начиная с (row, col): символ + атрибут, курсор двигаем сами */
static void put_str_attr(uint8_t row, uint8_t col, const char *s, uint8_t attr)
{
    while (*s) {
        bios_set_cursor(row, col++);
        bios_put_char_attr(*s++, attr);
    }
}

/* ---- сборка строки меню в буфере ---- */
static char line[80];
static uint8_t line_len;

static void lc(char c)              { line[line_len++] = c; }
static void ls(const char *s)       { while (*s) lc(*s++); }
static void ldec(uint16_t v)
{
    char t[6];
    uint8_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n)
        lc(t[--n]);
}

/* ---------------------------------------------------------------- опрос VBE */

static void probe_modes(void)
{
    struct vbe_info  *vi = VBE_INFO_BUF;
    struct mode_info *mi = MODE_INFO_BUF;

    for (uint8_t i = 0; i < ITEMS; i++)
        res_mode[i] = NO_MODE;

    vi->signature[0] = 'V'; vi->signature[1] = 'B'; vi->signature[2] = 'E'; vi->signature[3] = '2';
    if (bios_vbe(0x4F00, 0, 0, vi) != VBE_OK)
        halt_msg("\r\n[ c ] ERROR: VBE is not supported by this BIOS");

    uint16_t seg = vi->modes_seg, off = vi->modes_off;      /* список могут затереть вызовы ниже */

    for (;; off += 2) {
        uint16_t mode = far_read16(seg, off);
        if (mode == 0xFFFF)
            break;
        if (bios_vbe(0x4F01, 0, mode, mi) != VBE_OK)
            continue;
        if (!(mi->attributes & 0x0090))     /* нужна графика или линейный framebuffer */
            continue;
        if (mi->phys_base == 0)
            continue;
        if (mi->bpp != 32)                  /* только 32 бита на пиксель */
            continue;

        /* самый большой режим: по ширине, при равенстве по высоте */
        if (mi->width > max_w || (mi->width == max_w && mi->height > max_h)) {
            max_w = mi->width;
            max_h = mi->height;
            mode_max = mode;
        }
        for (uint8_t i = 0; i < ITEMS; i++)
            if (mi->width == res_w[i] && mi->height == res_h[i])
                res_mode[i] = mode;
    }
}

/* ---------------------------------------------------------------- меню */

#define BOX_COL   3
#define BOX_RIGHT 76
#define ATTR_NORM 0x07
#define ATTR_SEL  0x70                      /* инверсия, как в GRUB */

static void draw_border_char(uint8_t row, uint8_t col)
{
    bios_set_cursor(row, col);
    bios_put_char_attr((char)0xB3, ATTR_NORM);          /* '|' */
}

static void draw_hline(uint8_t row, char left, char right)
{
    bios_set_cursor(row, BOX_COL);
    bios_tty(left);
    tty_repeat((char)0xC4, 72);
    bios_tty(right);
}

static void draw_item(uint8_t row, uint8_t item)
{
    uint8_t attr = (item == selected) ? ATTR_SEL : ATTR_NORM;

    draw_border_char(row, BOX_COL);

    line_len = 0;
    ls("[ "); lc((char)('1' + item)); ls(" ]  ");
    if (item == ITEM_MAX) {
        ls("Auto / Hardware Limit : ");
        ldec(max_w); lc('x'); ldec(max_h);
        ls("  [ MAX ]");
    } else {
        ldec(res_w[item]); lc('x'); ldec(res_h[item]);
        while (line_len < 21)                           /* столбец статуса */
            lc(' ');
        ls(res_mode[item] != NO_MODE ? "[ OK ]" : "[ ! ] resolution not supported");
    }
    line[line_len] = 0;
    put_str_attr(row, 6, line, attr);

    draw_border_char(row, BOX_RIGHT);
}

static void draw_menu(void)
{
    bios_clear_screen();

    bios_set_cursor(2, 26);
    tty_str("overOS Video Mode Selection");

    draw_hline(4, (char)0xDA, (char)0xBF);
    for (uint8_t i = 0; i < ITEMS; i++)
        draw_item((uint8_t)(5 + i), i);
    draw_hline(13, (char)0xC3, (char)0xB4);
    draw_item(14, ITEM_MAX);
    draw_hline(15, (char)0xC0, (char)0xD9);

    bios_set_cursor(18, 17);
    tty_str("Use UP/DOWN arrows to navigate, ENTER to select");

    bios_set_cursor(24, 79);                            /* курсор в угол */
}

/* возвращает номер режима VBE, выбранный пользователем */
static uint16_t run_menu(void)
{
    for (;;) {
        draw_menu();
        for (;;) {
            uint16_t key = bios_getkey();
            uint8_t ascii = (uint8_t)key, scan = (uint8_t)(key >> 8);

            if (ascii == 0x0D) {
                uint16_t mode = (selected == ITEM_MAX) ? mode_max : res_mode[selected];
                if (mode != NO_MODE)
                    return mode;                        /* на неподдерживаемом пункте Enter игнорируется */
            } else if (scan == 0x48) {                  /* вверх */
                selected = selected ? selected - 1 : ITEM_MAX;
                break;
            } else if (scan == 0x50) {                  /* вниз */
                selected = (selected == ITEM_MAX) ? 0 : selected + 1;
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------- точка входа */

void menu_main(void) __attribute__((section(".text.entry")));

void menu_main(void)
{
    tty_str("[ c ] Scanning VBE profiles...\r\n");
    probe_modes();
    if (mode_max == NO_MODE)
        halt_msg("\r\n[ c ] ERROR: no 32-bit linear-framebuffer video mode");

    uint16_t mode = run_menu();

    bios_clear_screen();
    tty_str("\r\n\r\nBooting overOS...\r\n");
    bios_sleep_us(5UL << 16);

    /* ядро ждёт ModeInfoBlock выбранного режима по адресу 0x8000 */
    if (bios_vbe(0x4F01, 0, mode, MODE_INFO_BUF) != VBE_OK)
        halt_msg("\r\n[ c ] ERROR: VBE mode info failed");
    if (bios_vbe(0x4F02, mode | 0x4000, 0, NULL) != VBE_OK)     /* бит 14 = линейный framebuffer */
        halt_msg("\r\n[ c ] ERROR: cannot set the video mode");
}
