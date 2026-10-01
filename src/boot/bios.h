/* bios.h - тонкие обёртки над BIOS (INT 10h / 15h / 16h) для кода, который работает в
 * 16-битном реальном режиме. Компилируется gcc -m16 (32-битные инструкции с префиксами 0x66/0x67).
 *
 * Правила для такого C-кода (их обеспечивает boot_stage2.asm перед вызовом):
 *   - DS = ES = SS = 0, весь ESP (не только SP) < 0x10000 -> любой указатель = линейный адрес < 64 КиБ
 *   - DF = 0, прерывания разрешены (INT 16h их ждёт)
 *
 * При -DHOST_TEST эти функции подменяются заглушками из tools/test_menu.c: меню можно
 * проверить обычным gcc на хосте, без QEMU.
 */
#ifndef BIOS_H
#define BIOS_H

#include <stdint.h>

#ifdef HOST_TEST

uint16_t bios_getkey(void);                         /* AH = скан-код, AL = ASCII */
void     bios_set_cursor(uint8_t row, uint8_t col);
void     bios_put_char_attr(char c, uint8_t attr);  /* символ + атрибут в позиции курсора, курсор не двигается */
void     bios_tty(char c);                          /* символ с продвижением курсора (учитывает \r \n) */
void     bios_clear_screen(void);                   /* текстовый режим 80x25 */
void     bios_hide_cursor(void);
void     bios_sleep_us(uint32_t us);
uint16_t bios_vbe(uint16_t ax, uint16_t bx, uint16_t cx, void *buf);   /* INT 10h, AX=4Fxx; ES:DI = buf */
uint16_t far_read16(uint16_t seg, uint16_t off);
void     halt_forever(void) __attribute__((noreturn));

#else  /* ----- настоящий BIOS ----- */

static inline uint16_t bios_getkey(void)
{
    uint16_t ax = 0x0000;
    __asm__ volatile ("int $0x16" : "+a"(ax) : : "memory", "cc");
    return ax;
}

static inline void bios_set_cursor(uint8_t row, uint8_t col)
{
    uint16_t ax = 0x0200;
    __asm__ volatile ("int $0x10" : "+a"(ax)
                      : "b"(0), "d"(((uint32_t)row << 8) | col) : "memory", "cc");
}

static inline void bios_put_char_attr(char c, uint8_t attr)
{
    uint16_t ax = (uint16_t)(0x0900 | (uint8_t)c);
    __asm__ volatile ("int $0x10" : "+a"(ax) : "b"((uint32_t)attr), "c"(1) : "memory", "cc");
}

static inline void bios_tty(char c)
{
    uint16_t ax = (uint16_t)(0x0E00 | (uint8_t)c);
    __asm__ volatile ("int $0x10" : "+a"(ax) : "b"(7) : "memory", "cc");
}

static inline void bios_clear_screen(void)
{
    uint16_t ax = 0x0003;
    __asm__ volatile ("int $0x10" : "+a"(ax) : : "memory", "cc");
}

static inline void bios_hide_cursor(void)
{
    uint16_t ax = 0x0100;
    __asm__ volatile ("int $0x10" : "+a"(ax) : "c"(0x2000) : "memory", "cc");
}

static inline void bios_sleep_us(uint32_t us)
{
    uint16_t ax = 0x8600;
    __asm__ volatile ("int $0x15" : "+a"(ax)
                      : "c"(us >> 16), "d"(us & 0xFFFF) : "memory", "cc");
}

/* VBE: некоторые BIOS портят регистры кроме AX, поэтому все они объявлены изменяемыми */
static inline uint16_t bios_vbe(uint16_t ax, uint16_t bx, uint16_t cx, void *buf)
{
    uint32_t b = bx, c = cx, d, s;
    uint32_t di = (uint32_t)(uintptr_t)buf;        /* ES = 0, поэтому ES:DI = линейный адрес */
    __asm__ volatile ("int $0x10"
                      : "+a"(ax), "+b"(b), "+c"(c), "=d"(d), "=S"(s), "+D"(di)
                      : : "memory", "cc");
    return ax;
}

/* чтение слова по дальнему указателю seg:off (список режимов VBE может лежать в ПЗУ видеокарты) */
static inline uint16_t far_read16(uint16_t seg, uint16_t off)
{
    uint16_t v;
    __asm__ volatile ("pushw %%es\n\t"
                      "movw %w1, %%es\n\t"
                      "movw %%es:(%2), %w0\n\t"
                      "popw %%es"
                      : "=&r"(v) : "r"(seg), "r"((uint32_t)off) : "memory");
    return v;
}

static inline void halt_forever(void)
{
    for (;;)
        __asm__ volatile ("cli\n\thlt");
}

#endif /* HOST_TEST */
#endif /* BIOS_H */
