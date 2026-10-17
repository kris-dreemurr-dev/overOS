#include <stdint.h>
#include <stddef.h>
#include "../font/font.h"
#include "../drivers/hda.h"
#include "../drivers/usb/hid.h"
#include "cpu.h"
#include "contheme.h"
#include "../drivers/pci.h"
#include "../fs/fs.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "user_mode.h"
#include "sched.h"
#include "smp.h"
#include "tty.h"
#include "config.h"
#include "loader_kernel.h"
#include "../fs/mount.h"
#include "login.h"

#include "../files/image/sprite.h"
#include "../files/image/image.h"

// Цвет фона консоли — поле TTY (tty_console()->bg_color); здесь только дефолт для ранней загрузки
int mc_mode = 0;
void init_mc_monitor(void) {}

typedef struct {
    uint16_t attributes;
    uint8_t  winA, winB;
    uint16_t granularity;
    uint16_t winsize;
    uint16_t segmentA, segmentB;
    uint32_t winFuncPtr;
    uint16_t pitch;
    uint16_t width;
    uint16_t height;
    uint8_t  w_char, y_char, planes;
    uint8_t  bpp;
    uint8_t  banks, memory_model, bank_size, image_pages;
    uint8_t  reserved0;
    uint8_t  red_mask, red_position;
    uint8_t  green_mask, green_position;
    uint8_t  blue_mask, blue_position;
    uint8_t  rsv_mask, rsv_position;
    uint8_t  directcolor_attributes;
    uint32_t lfb_ptr;
} __attribute__((packed)) vbe_info_t;

volatile uint32_t* lfb;
int g_lfb_wc_entries = 0;            // записей таблиц страниц LFB с типом WC (0 -- видеопамять некэшируемая)
uint16_t screen_width;
uint16_t screen_height;
uint16_t screen_pitch;

// Прототипы графики (определены ниже в этом файле)
void flush_buffer(void);
void flush_dirty(void);
void gfx_mark_all_dirty(void);
void fill_rect(int x, int y, int w, int h, uint32_t color);

// Счётчик "кто-то кроме /dev/fb0 записал в LFB" (devfs.c по нему понимает, что его копия экрана устарела)
volatile uint32_t g_lfb_gen = 0;
// Когда и с какого TTY последний раз писали в /dev/fb0 (пока пишут — курсор консоли не рисуем поверх)
volatile uint32_t g_fb_last_write_tick = 0;
volatile int      g_fb_write_tty = -2;

#define MAX_SCREEN_WIDTH  1920
#define MAX_SCREEN_HEIGHT 1080
uint32_t back_buffer[MAX_SCREEN_WIDTH * MAX_SCREEN_HEIGHT];
// Кольцо прокрутки TTY1: как и его экранный буфер, статическое (остальные TTY
// получают своё при первом Alt+F2..F6). Шаг строки — screen_width, под него же
// выделяется память в tty_spawn: с разными шагами прокрутка писала за буфер и
// рушила чужую память (ядро падало на первой же прокрутке в TTY2).
uint32_t kernel_screen_history[TTY_HIST_HEIGHT * MAX_SCREEN_WIDTH];


// Динамический хук потока вывода терминала (для модулей типа DE.SYS)
static void (*g_term_hook)(const char* text) = 0;

void set_term_hook(void (*hook)(const char* text)) {
    g_term_hook = hook;
}

void clear_screen(uint32_t color);
uint32_t con_tint(const tty_t* con, uint32_t c);   // палитра консоли (определена ниже)

// Снимок консоли на время работы модуля в графике — у каждого TTY свой
static int console_is_saved[TTY_SLOTS];
static int saved_cursor_x[TTY_SLOTS], saved_cursor_y[TTY_SLOTS];
static int saved_total_scrolled[TTY_SLOTS], saved_scroll_offset[TTY_SLOTS];

// TTY, которому принадлежит вызывающая задача (для модулей — TTY, где их запустили)
static tty_t* console_task_tty(void) {
    return tty_console();
}

void console_save_state(void) {
    tty_t* con = console_task_tty();
    if (!con) return;
    con->gfx_mode = 1;   // экран занят модулем: мигание курсора консоли выключено
    if (console_is_saved[con->id]) return;   // не перезаписываем снимок во время подмодулей
    saved_cursor_x[con->id]      = con->cursor_x;
    saved_cursor_y[con->id]      = con->cursor_y;
    saved_total_scrolled[con->id] = con->total_scrolled_px;
    saved_scroll_offset[con->id]  = con->scroll_offset_px;
    console_is_saved[con->id] = 1;
}

void console_restore_state(void) {
    tty_t* con = console_task_tty();
    if (!con) return;
    con->gfx_mode = 0;
    // Модуль рисовал прямо в буфер TTY: после выхода чистим экран, иначе
    // приглашение печаталось поверх остатков его картинки
    clear_screen(CON_BG);
    con->cursor_x         = 0;
    con->cursor_y         = 0;
    con->total_scrolled_px = 0;
    con->scroll_offset_px  = 0;
    con->bg_color = con_tint(con, CON_BG);
    (void)saved_cursor_x; (void)saved_cursor_y;
    (void)saved_total_scrolled; (void)saved_scroll_offset;

    flush_buffer();
    console_is_saved[con->id] = 0;
}

extern void usb_controller_init(void);
extern void ehci_init(void);
extern void keyboard_poll_handler(void);
extern void init_ps2_mouse(void);
extern void update_mouse_state(void);   // читает мышь — нужна case 23 (get_mouse_delta), не трогаю
extern void ps2_hw_service(void);       // клавиатура для .prg/.sys/оболочки + Ctrl+C (keyboard.c)
extern void draw_cursor_shape(int x, int y);
extern int get_mouse_x(void);
extern int get_mouse_y(void);
extern int get_mouse_btn(void);
extern volatile int g_user_mode_active;
extern void return_from_user_mode(int exit_code);
extern uint64_t* vmm_get_kernel_pml4_phys(void);
extern void timer_isr_asm(void);
extern int prog_load_module(const char* filename, const char* args);

extern uint64_t* kernel_pml4;

uint32_t timer_ticks = 0;

uint64_t g_tsc_per_ms = 0;   // заполняется в tsc_calibrate()

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Вызывать один раз после sti, когда таймер уже тикает
void tsc_calibrate(void) {
    uint32_t t0 = timer_ticks;
    while (timer_ticks == t0) __asm__ volatile ("pause");   // выравниваемся на границу тика
    uint64_t a = rdtsc64();
    uint32_t st = timer_ticks;
    while ((timer_ticks - st) < 50) __asm__ volatile ("pause");
    uint64_t b = rdtsc64();
    g_tsc_per_ms = (b - a) / 50;
}

int strcasecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        int c1 = *s1++;
        int c2 = *s2++;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
    }
    return (*s1 - *s2);
}

void itoa(int n, char* str) {
    int i = 0;
    int is_negative = 0;

    if (n == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return;
    }

    if (n < 0) {
        is_negative = 1;
        n = -n;
    }

    while (n != 0) {
        int rem = n % 10;
        str[i++] = rem + '0';
        n = n / 10;
    }

    if (is_negative) {
        str[i++] = '-';
    }

    str[i] = '\0';

    int start = 0;
    int end = i - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %w0, %1" : : "a"(val), "Nd"(port));
}

uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Если outl / inl еще не объявлены в заголовочных файлах ядра:
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) return;

    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));

    // Если прерывания выключены (сисколл из Ring 3 или ранняя загрузка ядра)
    if (!(rflags & 0x200)) {
        // После калибровки ждём по TSC: точно и не зависит от IF
        if (g_tsc_per_ms) {
            uint64_t end = rdtsc64() + (uint64_t)ms * g_tsc_per_ms;
            while (rdtsc64() < end) {
                __asm__ volatile ("pause");
            }
            return;
        }
        for (uint32_t m = 0; m < ms; m++) {
            for (volatile int i = 0; i < 30000; i++) {
                __asm__ volatile ("pause");
            }
        }
        return;
    }

    // Если прерывания включены — спим строго по тикам таймера
    uint32_t start = timer_ticks;
    while ((timer_ticks - start) < ms) {
        // Уступаем процессор другим задачам, а если ждать больше некого — спим до
        // следующего прерывания таймера (hlt): раньше ядро крутило pause и грело CPU на 100%
        sched_yield();
        __asm__ volatile ("hlt");
    }
}

void wait(uint32_t seconds) {
    sleep_ms(seconds * 1000);
}

void wait_vsync(void) {
    while ((inb(0x3DA) & 0x08) == 0x08);
    while ((inb(0x3DA) & 0x08) == 0);
}

// ---------- быстрые примитивы копирования/заливки (счётчик в ПИКСЕЛЯХ) ----------
static inline void copy32(void* d, const void* s, size_t n) {
    size_t q = n >> 1;
    __asm__ volatile ("rep movsq" : "+D"(d), "+S"(s), "+c"(q) :: "memory");
    if (n & 1) *(uint32_t*)d = *(const uint32_t*)s;
}

static inline void fill32(void* d, uint32_t v, size_t n) {
    uint64_t v2 = ((uint64_t)v << 32) | v;
    size_t q = n >> 1;
    __asm__ volatile ("rep stosq" : "+D"(d), "+c"(q) : "a"(v2) : "memory");
    if (n & 1) *(uint32_t*)d = v;
}

// ---------- dirty rectangle: что изменилось с прошлого сброса в LFB (пусто, если x0 >= x1) ----------
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;

static inline void dirty_reset(void) { dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0; }

static inline void mark_dirty(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > screen_width)  x1 = screen_width;
    if (y1 > screen_height) y1 = screen_height;
    if (x0 >= x1 || y0 >= y1) return;
    if (dirty_x0 >= dirty_x1) {
        dirty_x0 = x0; dirty_y0 = y0; dirty_x1 = x1; dirty_y1 = y1;
        return;
    }
    if (x0 < dirty_x0) dirty_x0 = x0;
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (x1 > dirty_x1) dirty_x1 = x1;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

void gfx_mark_all_dirty(void) { mark_dirty(0, 0, screen_width, screen_height); }

// Полный сброс буфера в LFB. Рисует ВСЁ — безопасен для кода, который пишет
// в буфер TTY напрямую (put_pixel, модули, desktop).
void flush_buffer(void) {
    tty_t* con = tty_console();
    if (!con || !con->buffer) return;
    // На экран попадает только активный TTY: фоновая оболочка рисует в свой буфер
    if (con->id != tty_get_active_id()) return;

    uint8_t* lfb_bytes = (uint8_t*)lfb;
    const size_t w = screen_width;

    if (con->scroll_offset_px == 0) {
        if (screen_pitch == w * sizeof(uint32_t)) {
            copy32(lfb_bytes, con->buffer, w * screen_height);
        } else {
            for (int y = 0; y < screen_height; y++)
                copy32(lfb_bytes + (size_t)y * screen_pitch, &con->buffer[y * w], w);
        }
    } else {
        // Просмотр истории: копируем целыми строками, а не по пикселю
        int virtual_y = con->total_scrolled_px - con->scroll_offset_px;
        for (int y = 0; y < screen_height; y++) {
            int src_y = virtual_y + y;
            void* dst = lfb_bytes + (size_t)y * screen_pitch;
            if (src_y >= 0 && src_y < con->total_scrolled_px) {
                copy32(dst, &con->history[(size_t)(src_y % TTY_HIST_HEIGHT) * w], w);
            } else if (src_y >= con->total_scrolled_px && (src_y - con->total_scrolled_px) < screen_height) {
                copy32(dst, &con->buffer[(src_y - con->total_scrolled_px) * w], w);
            } else {
                fill32(dst, 0, w);
            }
        }
    }
    __asm__ volatile ("sfence" ::: "memory");
    dirty_reset();
    g_lfb_gen++;
}

// Сброс только изменённого прямоугольника (консоль: символ = 8x16, курсор = 8x2)
void flush_dirty(void) {
    tty_t* con = tty_console();
    if (!con || !con->buffer) return;
    if (con->id != tty_get_active_id()) { dirty_reset(); return; }
    if (dirty_x0 >= dirty_x1) return;
    if (con->scroll_offset_px != 0) { flush_buffer(); return; }

    int x0 = dirty_x0, y0 = dirty_y0, x1 = dirty_x1, y1 = dirty_y1;
    dirty_reset();

    size_t n = (size_t)(x1 - x0);
    uint8_t* dst = (uint8_t*)lfb + (size_t)y0 * screen_pitch + (size_t)x0 * 4;
    const uint32_t* src = con->buffer + (size_t)y0 * con->w_px + x0;
    for (int y = y0; y < y1; y++) {
        copy32(dst, src, n);
        dst += screen_pitch;
        src += con->w_px;
    }
    __asm__ volatile ("sfence" ::: "memory");
    g_lfb_gen++;
}

void put_pixel(int x, int y, uint32_t color) {
    tty_t* con = tty_console();
    if (!con || !con->buffer) return;
    if (x < 0 || x >= con->w_px || y < 0 || y >= con->h_px) return;
    con->buffer[y * con->w_px + x] = color;
}

void fill_rect(int x, int y, int w, int h, uint32_t color) {
    tty_t* con = tty_console();
    color = con_tint(con, color);
    if (!con || !con->buffer) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > con->w_px) x1 = con->w_px;
    if (y1 > con->h_px) y1 = con->h_px;
    if (x >= x1 || y >= y1) return;
    uint32_t* row = con->buffer + (size_t)y * con->w_px + x;
    for (int yy = y; yy < y1; yy++, row += con->w_px)
        fill32(row, color, (size_t)(x1 - x));
    mark_dirty(x, y, x1, y1);
}

// Перевод цвета из палитры терминала в классическую палитру текстовой консоли.
//
// Пишущие в консоль (ядро, оболочка, fastfetch) знают один набор имён CON_*.
// Это цвета ОКНА терминала; на tty1..tty6 должна быть классика — чёрный фон и
// яркие цвета VGA, как в консоли Linux. Перевод делается здесь, в самом низу,
// поэтому ни одному вызывающему не нужно знать, в какую консоль он пишет.
uint32_t con_tint(const tty_t* con, uint32_t c) {
    if (con && con->is_pty) return c;            // окно терминала — своя тема
    switch (c) {
        case CON_BG:      return CONT_BG;
        case CON_BG_ALT:  return CONT_BG_ALT;
        case CON_FG:      return CONT_FG;
        case CON_DIM:     return CONT_DIM;
        case CON_RED:     return CONT_RED;
        case CON_GREEN:   return CONT_GREEN;
        case CON_YELLOW:  return CONT_YELLOW;
        case CON_BLUE:    return CONT_BLUE;
        case CON_MAGENTA: return CONT_MAGENTA;
        case CON_CYAN:    return CONT_CYAN;
        default:          return c;              // произвольный цвет (логотип, модуль) не трогаем
    }
}

void draw_cursor(uint32_t color) {
    tty_t* con = tty_console();
    if (!con) return;
    fill_rect(con->cursor_x, con->cursor_y + 14, 8, 2, con_tint(con, color));
}

void clear_screen(uint32_t color) {
    tty_t* con = tty_console();
    if (!con || !con->buffer) return;
    color = con_tint(con, color);
    con->bg_color = color;
    fill32(con->buffer, color, (size_t)con->w_px * con->h_px);
    con->cursor_x = 0;
    con->cursor_y = 0;
    con->total_scrolled_px = 0;
    con->scroll_offset_px = 0;
    gfx_mark_all_dirty();
}

void scroll_screen(void) {
    tty_t* con = tty_console();
    if (!con || !con->buffer || !con->history) return;
    const size_t w = con->w_px;
    const int    h = con->h_px;
    for (int y = 0; y < 16; y++) {
        int hist_y = (con->total_scrolled_px + y) % TTY_HIST_HEIGHT;
        copy32(&con->history[(size_t)hist_y * w], &con->buffer[y * w], w);
    }
    con->total_scrolled_px += 16;

    // один непрерывный сдвиг вверх (dst < src, прямой rep movs корректен)
    copy32(con->buffer, con->buffer + 16 * w, (size_t)(h - 16) * w);
    fill32(con->buffer + (size_t)(h - 16) * w, con->bg_color, 16 * w);
    con->cursor_y -= 16;
    gfx_mark_all_dirty();
}

// Юникод -> CP866. Кириллица ложится в таблицу один в один, а типографику
// (длинное тире, кавычки, многоточие) CP866 просто не знает: её заменяем
// похожими знаками, иначе в консоли вместо «—» появлялось "тАФ" — это были
// сырые байты UTF-8, нарисованные как символы кодовой страницы.
static uint8_t uni_to_cp866(uint32_t cp) {
    if (cp < 0x80) return (uint8_t)cp;
    if (cp >= 0x410 && cp <= 0x43F) return (uint8_t)(0x80 + (cp - 0x410));
    if (cp >= 0x440 && cp <= 0x44F) return (uint8_t)(0xE0 + (cp - 0x440));
    if (cp == 0x401) return 0x85;              // Ё -> Е
    if (cp == 0x451) return 0xA5;              // ё -> е
    switch (cp) {
        case 0x00A0: return ' ';               // неразрывный пробел
        case 0x00AB: case 0x00BB:              // « »
        case 0x201C: case 0x201D: case 0x201E: return '"';
        case 0x2018: case 0x2019: case 0x02BC: return '\'';
        case 0x2010: case 0x2011: case 0x2012:
        case 0x2013: case 0x2014: case 0x2015: return '-';   // тире всех видов
        case 0x2026: return 0xFA;              // многоточие -> точка-середина
        case 0x00B7: case 0x2022: case 0x2219: return 0xFA;
        case 0x00B0: return 0xF8;              // градус
        case 0x2116: return 0xFC;              // №
        case 0x00A9: return 'c';
        case 0x2192: return '>';
        case 0x2190: return '<';
    }
    return '?';
}

// Разбор UTF-8 по одному байту. Возвращает 1, когда символ собран целиком,
// и кладёт его код CP866 в *out. Состояние живёт у вызывающего, поэтому одна
// и та же функция обслуживает и посимвольный kputc, и строковый kputs_at.
typedef struct { uint32_t cp; int need; } utf8_state_t;

static int utf8_feed(utf8_state_t* st, uint8_t b, uint8_t* out) {
    if (st->need) {
        if ((b & 0xC0) != 0x80) { st->need = 0; *out = '?'; return 1; }   // битая последовательность
        st->cp = (st->cp << 6) | (b & 0x3F);
        if (--st->need) return 0;
        *out = uni_to_cp866(st->cp);
        return 1;
    }
    if (b < 0x80)        { *out = b; return 1; }
    if ((b & 0xE0) == 0xC0) { st->cp = b & 0x1F; st->need = 1; return 0; }
    if ((b & 0xF0) == 0xE0) { st->cp = b & 0x0F; st->need = 2; return 0; }
    if ((b & 0xF8) == 0xF0) { st->cp = b & 0x07; st->need = 3; return 0; }
    *out = '?';                                 // одиночный байт продолжения
    return 1;
}

// Отрисовка символа с возможностью прозрачного фона (быстрый путь без проверок на каждый пиксель)
void draw_char_ex(int start_x, int start_y, char c, uint32_t color, int opaque) {
    unsigned char uc = (unsigned char)c;
    const uint8_t* glyph = &fontdata_ru_8x16[uc * 16];
    tty_t* con = tty_console();
    if (!con || !con->buffer) return;
    color = con_tint(con, color);

    if (start_x < 0 || start_y < 0 ||
        start_x + 8 > con->w_px || start_y + 16 > con->h_px) {
        // редкий случай: символ на краю экрана — медленный путь с клиппингом
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    put_pixel(start_x + col, start_y + row, color);
                } else if (opaque) {
                    put_pixel(start_x + col, start_y + row, con->bg_color);
                }
            }
        }
        mark_dirty(start_x, start_y, start_x + 8, start_y + 16);
        return;
    }

    uint32_t* d = con->buffer + (size_t)start_y * con->w_px + start_x;
    const uint32_t bg = con->bg_color;
    for (int row = 0; row < 16; row++, d += con->w_px) {
        unsigned bits = glyph[row];
        if (opaque) {
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++) d[i] = (bits & (0x80u >> i)) ? color : bg;
        } else {
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++) if (bits & (0x80u >> i)) d[i] = color;
        }
    }
    mark_dirty(start_x, start_y, start_x + 8, start_y + 16);
}

// Стандартный draw_char (непрозрачный для консоли)
void draw_char(int start_x, int start_y, char c, uint32_t color) {
    draw_char_ex(start_x, start_y, c, color, 1);
}

void kputs_at(int x, int y, const char* str, uint32_t color) {
    int cx = x;
    utf8_state_t st = { 0, 0 };

    while (*str) {
        uint8_t uc;
        if (!utf8_feed(&st, (uint8_t)*str++, &uc)) continue;
        // [ФИКС]: opaque = 0, чтобы фон окна не затирался цветом обоев
        draw_char_ex(cx, y, (char)uc, color, 0);
        cx += 8;
    }
}

static char de_stream_line[128];
static int de_stream_idx = 0;

void kputc(char c, uint32_t color) {
    if (g_term_hook) {
        if (c == '\n') {
            de_stream_line[de_stream_idx] = '\0';
            g_term_hook(de_stream_line);
            de_stream_idx = 0;
        } else if (c == '\b') {
            if (de_stream_idx > 0) de_stream_idx--;
        } else if (c != '\r') {
            if (de_stream_idx < 120) {
                de_stream_line[de_stream_idx++] = c;
            }
        }
        return;
    }

    static utf8_state_t st = { 0, 0 };
    uint8_t uc;
    if (!utf8_feed(&st, (uint8_t)c, &uc)) return;    // ждём остальные байты символа

    tty_t* con = tty_console();
    if (!con || !con->buffer) return;

    draw_cursor(con->bg_color);

    if (uc == '\b') {
        // Возврат через край строки: длинная команда занимает несколько строк
        // экрана, и без перехода наверх её нельзя было стереть (это видно при
        // листании истории стрелками).
        if (con->cursor_x >= 8) {
            con->cursor_x -= 8;
        } else if (con->cursor_y >= 16) {
            con->cursor_y -= 16;
            con->cursor_x = con->w_px - 8;
        } else {
            return;
        }
        fill_rect(con->cursor_x, con->cursor_y, 8, 16, con->bg_color);
        draw_cursor(CON_CURSOR);
        return;
    }

    if (uc == '\n') {
        con->cursor_x = 0;
        con->cursor_y += 16;
    } else if (uc == '\r') {
        con->cursor_x = 0;
    } else {
        draw_char(con->cursor_x, con->cursor_y, (char)uc, color);
        con->cursor_x += 8;
        if (con->cursor_x + 8 > con->w_px) {
            con->cursor_x = 0;
            con->cursor_y += 16;
        }
    }

    if (con->cursor_y + 16 > con->h_px) {
        scroll_screen();
    }
}

void kputs(const char* str, uint32_t color) {
    while (*str) {
        kputc(*str, color);
        str++;
    }
    //flush_buffer();
    //sleep_ms(5);
}

void kdebug(const char* str, uint32_t color) {
    if (DEBUG_MODE) {
        kputs(str, color);
    }
}

uint32_t get_back_pixel(int x, int y) {
    tty_t* con = tty_console();
    if (!con || !con->buffer) return 0;
    if (x < 0 || x >= con->w_px || y < 0 || y >= con->h_px) return 0;
    return con->buffer[y * con->w_px + x];
}

void print_prompt(void) {
    login_print_prompt();   // "login:" / "Password:" / "user@overos:/path$ " — по состоянию TTY
    flush_dirty();
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

void sys_reboot(void) {
    while (inb(0x64) & 2);
    outb(0x64, 0xFE);
    __asm__ __volatile__("cli; hlt");
}

static void render_frame(int x_pos, int y_pos) {
    clear_screen(CON_BG);

    for (int y = 0; y < IMAGE_HEIGHT; y++) {
        for (int x = 0; x < IMAGE_WIDTH; x++) {
            put_pixel(x_pos + x, y_pos + y, image_data[y * IMAGE_WIDTH + x]);
        }
    }

    const char* hint = "WASD - Move | ESC - Exit";
    kputs_at(16, 16, hint, 0xAAAAAA);
    flush_buffer();
}

void enter_render_mode(void) {
    console_save_state();

    int img_x = (screen_width - IMAGE_WIDTH) / 2;
    int img_y = (screen_height - IMAGE_HEIGHT) / 2;
    int step = 16;

    render_frame(img_x, img_y);

    while (1) {
        if (inb(0x64) & 1) {
            uint8_t scancode = inb(0x60);
            if (scancode == 0x01) break;

            int moved = 0;
            if (scancode == 0x11) { img_y -= step; moved = 1; }
            if (scancode == 0x1F) { img_y += step; moved = 1; }
            if (scancode == 0x1E) { img_x -= step; moved = 1; }
            if (scancode == 0x20) { img_x += step; moved = 1; }

            if (moved) {
                if (img_x < 0) img_x = 0;
                if (img_y < 0) img_y = 0;
                if (img_x + IMAGE_WIDTH > screen_width) img_x = screen_width - IMAGE_WIDTH;
                if (img_y + IMAGE_HEIGHT > screen_height) img_y = screen_height - IMAGE_HEIGHT;

                render_frame(img_x, img_y);
            }
        }
    }

    console_restore_state();
}

// Чтение модели процессора через CPUID
static void get_cpu_brand(char* out_brand) {
    uint32_t brand[12];
    for (int i = 0; i < 3; i++) {
        __asm__ volatile ("cpuid"
            : "=a"(brand[i * 4 + 0]), "=b"(brand[i * 4 + 1]), 
              "=c"(brand[i * 4 + 2]), "=d"(brand[i * 4 + 3])
            : "a"(0x80000002 + i), "c"(0));
    }
    char* src = (char*)brand;
    while (*src == ' ') src++; // Пропуск начальных пробелов
    int idx = 0;
    while (*src && idx < 47) out_brand[idx++] = *src++;
    out_brand[idx] = '\0';
}

// Получение модели ПК/Платы через SMBIOS Type 1
static void get_smbios_host(char* out_host, int max_len) {
    const uint8_t* bios_mem = (const uint8_t*)0xF0000;
    out_host[0] = '\0';

    for (uint32_t off = 0; off < 0x10000; off += 16) {
        if (bios_mem[off] == '_' && bios_mem[off+1] == 'S' && 
            bios_mem[off+2] == 'M' && bios_mem[off+3] == '_') {
            
            uint32_t table_addr = *(const uint32_t*)&bios_mem[off + 0x18];
            uint16_t num_structs = *(const uint16_t*)&bios_mem[off + 0x1C];
            const uint8_t* ptr = (const uint8_t*)(uintptr_t)table_addr;

            for (uint16_t i = 0; i < num_structs; i++) {
                uint8_t type = ptr[0];
                uint8_t len = ptr[1];
                if (type == 1) { // System Information
                    uint8_t prod_idx = ptr[5];
                    const char* str = (const char*)ptr + len;
                    while (prod_idx > 1 && *str) {
                        while (*str) str++;
                        str++;
                        prod_idx--;
                    }
                    int k = 0;
                    while (*str && k < max_len - 1) out_host[k++] = *str++;
                    out_host[k] = '\0';
                    return;
                }
                if (type == 127) break;
                ptr += len;
                while (*(const uint16_t*)ptr != 0) ptr++;
                ptr += 2;
            }
            break;
        }
    }
    if (out_host[0] == '\0') {
        const char* def = "Generic x86_64 PC";
        for (int i = 0; def[i]; i++) out_host[i] = def[i];
        out_host[17] = '\0';
    }
}

// Поиск GPU на шине PCI
static void get_pci_gpu(char* out_gpu) {
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t addr = (1U << 31) | (bus << 16) | (slot << 11);
            outl(0xCF8, addr);
            uint32_t ven_dev = inl(0xCFC);
            uint16_t ven = ven_dev & 0xFFFF;
            uint16_t dev = ven_dev >> 16;

            if (ven == 0x10DE) { // NVIDIA
                const char* n = "NVIDIA GeForce (PCIe)";
                int i = 0; while (n[i]) { out_gpu[i] = n[i]; i++; }
                out_gpu[i] = '\0';
                return;
            } else if (ven == 0x8086 && (slot == 2)) { // Intel Integrated
                const char* n = "Intel HD Graphics";
                int i = 0; while (n[i]) { out_gpu[i] = n[i]; i++; }
                out_gpu[i] = '\0';
                return;
            } else if (ven == 0x1234 && dev == 0x1111) { // QEMU Standard VGA
                const char* n = "QEMU Standard VBE Display";
                int i = 0; while (n[i]) { out_gpu[i] = n[i]; i++; }
                out_gpu[i] = '\0';
                return;
            }
        }
    }
    const char* n = "VGA Compatible Controller";
    int i = 0; while (n[i]) { out_gpu[i] = n[i]; i++; }
    out_gpu[i] = '\0';
}

void cmd_fastfetch(void) {
    char buf[64];
    uint32_t total_sec = timer_ticks / 1000;
    uint32_t days = total_sec / 86400;
    uint32_t hours = (total_sec % 86400) / 3600;
    uint32_t mins = (total_sec % 3600) / 60;
    uint32_t secs = total_sec % 60;

#if FUN_EDITION
    const int scale = 2;
    int scaled_w = FASTFETCH_WIDTH * scale;
    int scaled_h = FASTFETCH_HEIGHT * scale;
#else
    int scaled_w = LOGO_ASCII_COLS * 8;   // Ширина в пикселях (символ шрифта = 8px)
    int scaled_h = LOGO_ASCII_ROWS * 16;  // Высота в пикселях (строка шрифта = 16px)
#endif

    // Текстовый блок fastfetch -- 16 строк по 16 точек, плюс полоска цветов
    int total_h = scaled_h > 320 ? scaled_h : 320;

    tty_t* con = tty_console();
    // Высоту берём у самой консоли, а не у экрана: у окна терминала она своя,
    // и по screen_height прокрутка не срабатывала -- вывод уезжал за нижний
    // край и просто пропадал.
    int guard = 256;
    while (con->cursor_y > 0 && con->cursor_y + total_h > con->h_px && guard-- > 0) {
        scroll_screen();
    }
    if (con->cursor_y < 0) con->cursor_y = 0;

    int start_x = con->cursor_x;
    int start_y = con->cursor_y;

    int sprite_x = start_x + 10;
    int sprite_y = start_y + (total_h - scaled_h) / 2;

#if FUN_EDITION
    // Отрисовка попиксельного спрайта Криса
    for (int y = 0; y < FASTFETCH_HEIGHT; y++) {
        for (int x = 0; x < FASTFETCH_WIDTH; x++) {
            uint32_t color = fastfetch_sprite_data[y * FASTFETCH_WIDTH + x];
            if (color != 0x000000) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        put_pixel(sprite_x + x * scale + dx, sprite_y + y * scale + dy, color);
                    }
                }
            }
        }
    }
#else
    // Отрисовка нового ASCII-секундомера фирменным бирюзовым цветом
    for (int r = 0; r < LOGO_ASCII_ROWS; r++) {
        kputs_at(sprite_x, sprite_y + (r * 16), logo_ascii[r], CON_CYAN);
    }
#endif

    // Сдвигаем текстовый блок с учетом ширины логотипа
    int text_x = sprite_x + scaled_w + 24;
    int text_y = start_y;

    // Имя хоста
    // Шапка fastfetch: кто на самом деле работает в этом TTY, а не всегда root
    {
        char who[64];
        int wp = 0;
        tty_t* tcon = tty_console();
        const char* uname_s = (tcon && tcon->login_name[0]) ? tcon->login_name : "root";
        while (*uname_s && wp < 32) who[wp++] = *uname_s++;
        who[wp++] = '@';
        const char* hn = OS_LOWER_NAME;
        while (*hn && wp < 60) who[wp++] = *hn++;
        who[wp] = '\0';
        kputs_at(text_x, text_y, who, CON_GREEN); text_y += 16;
    }
    kputs_at(text_x, text_y, "----------------------------", CON_DIM); text_y += 16;

    // OS & Kernel
    kputs_at(text_x, text_y, "OS:          ", CON_CYAN);
    kputs_at(text_x + 13 * 8, text_y, OS_NAME " x86_64", CON_FG); text_y += 16;

    char host_str[64];
    get_smbios_host(host_str, sizeof(host_str));
    kputs_at(text_x, text_y, "Host:        ", CON_CYAN);
    kputs_at(text_x + 13 * 8, text_y, host_str, CON_FG); text_y += 16;

    kputs_at(text_x, text_y, "Kernel:      ", CON_CYAN);
    kputs_at(text_x + 13 * 8, text_y, OS_NAME " " OS_VERSION " (" OS_ARCH ")", CON_FG);
    text_y += 16;

    // Uptime
    kputs_at(text_x, text_y, "Uptime:      ", CON_CYAN);
    char up_msg[64];
    int up_p = 0;
    if (days > 0) {
        itoa(days, buf); for (int i = 0; buf[i]; i++) up_msg[up_p++] = buf[i];
        up_msg[up_p++] = 'd'; up_msg[up_p++] = ' ';
    }
    itoa(hours, buf); for (int i = 0; buf[i]; i++) up_msg[up_p++] = buf[i];
    up_msg[up_p++] = 'h'; up_msg[up_p++] = ' ';
    itoa(mins, buf); for (int i = 0; buf[i]; i++) up_msg[up_p++] = buf[i];
    up_msg[up_p++] = 'm'; up_msg[up_p++] = ' ';
    itoa(secs, buf); for (int i = 0; buf[i]; i++) up_msg[up_p++] = buf[i];
    up_msg[up_p++] = 's'; up_msg[up_p] = '\0';
    kputs_at(text_x + 13 * 8, text_y, up_msg, CON_FG); text_y += 16;

    // Display
    kputs_at(text_x, text_y, "Display:     ", CON_CYAN);
    char res_buf[48];
    char w_s[8], h_s[8];
    itoa(screen_width, w_s); 
    itoa(screen_height, h_s);
    int rp = 0;
    for (int i = 0; w_s[i]; i++) res_buf[rp++] = w_s[i];
    res_buf[rp++] = 'x';
    for (int i = 0; h_s[i]; i++) res_buf[rp++] = h_s[i];
    const char* bpp_str = " @ 60 Hz [VBE LFB]";
    for (int i = 0; bpp_str[i]; i++) res_buf[rp++] = bpp_str[i];
    res_buf[rp] = '\0';
    kputs_at(text_x + 13 * 8, text_y, res_buf, CON_FG); 
    text_y += 16;

    // Shell
    kputs_at(text_x, text_y, "Shell:       ", CON_CYAN);
    kputs_at(text_x + 13 * 8, text_y, "bash-lite (overOS)", CON_FG); 
    text_y += 16;

    // Terminal
    kputs_at(text_x, text_y, "Terminal:    ", CON_CYAN);
    {
        tty_t* tcon = tty_console();
        kputs_at(text_x + 13 * 8, text_y,
                 (tcon && tcon->is_pty) ? "overTerm (pty)" : "kernel-console", CON_FG);
    } 
    text_y += 16;

    // Font
    kputs_at(text_x, text_y, "Font:        ", CON_CYAN);
    kputs_at(text_x + 13 * 8, text_y, "8x16-tty (CP866)", CON_FG); 
    text_y += 16;

    // CPU: модель, сколько ядер и потоков мы ВИДИМ, и настоящая частота
    char cpu_str[48];
    get_cpu_brand(cpu_str);
    kputs_at(text_x, text_y, "CPU:         ", CON_CYAN);
    kputs_at(text_x + 13 * 8, text_y, cpu_str, CON_FG); text_y += 16;
    {
        const cpu_info_t* ci = cpu_get();
        char line[96];
        int p = 0;
        char num[16];
        #define CPU_PUT(str) do { const char* _s = (str); \
            while (*_s && p < (int)sizeof(line) - 1) line[p++] = *_s++; } while (0)
        CPU_PUT("ядер ");    itoa(ci->cores, num);   CPU_PUT(num);
        CPU_PUT(", потоков "); itoa(ci->threads, num); CPU_PUT(num);
        CPU_PUT(" (занято 1), ");
        // Текущая частота -- измеренная по APERF/MPERF. Под гипервизором этих
        // счётчиков нет, и вместо нуля честнее показать паспортную.
        int mhz = ci->cur_mhz ? ci->cur_mhz : ci->base_mhz;
        if (mhz) {
            itoa(mhz, num); CPU_PUT(num); CPU_PUT(" МГц");
            if (!ci->cur_mhz) CPU_PUT(" (паспорт)");
            if (ci->max_mhz > mhz) { CPU_PUT(" из "); itoa(ci->max_mhz, num); CPU_PUT(num); }
        } else {
            CPU_PUT("частота неизвестна");
        }
        line[p] = '\0';
        #undef CPU_PUT
        kputs_at(text_x, text_y, "Частота:     ", CON_CYAN);
        kputs_at(text_x + 13 * 8, text_y, line, ci->turbo_on ? CON_GREEN : CON_FG);
        text_y += 16;

        const char* gov = (ci->governor == CPU_GOV_HWP)    ? "HWP, максимум" :
                          (ci->governor == CPU_GOV_PSTATE) ? "P-state, максимум" :
                                                             "управляет прошивка";
        int gw = 0;
        for (const char* g = gov; *g; g++) if (((uint8_t)*g & 0xC0) != 0x80) gw++;
        kputs_at(text_x, text_y, "Питание:     ", CON_CYAN);
        kputs_at(text_x + 13 * 8, text_y, gov, CON_FG);
        kputs_at(text_x + 13 * 8 + gw * 8, text_y,
                 ci->turbo_on ? "  турбо вкл." : "  турбо выкл.",
                 ci->turbo_on ? CON_GREEN : CON_DIM);
        text_y += 16;
    }

    // GPU
    char gpu_str[36];
    get_pci_gpu(gpu_str);
    kputs_at(text_x, text_y, "GPU:         ", CON_CYAN);
    kputs_at(text_x + 13 * 8, text_y, gpu_str, CON_FG); text_y += 16;

    // Memory
    kputs_at(text_x, text_y, "Memory:      ", CON_CYAN);

    uint64_t used_bytes = pmm_get_used_blocks() * PAGE_SIZE;
    uint64_t total_bytes = pmm_get_total_blocks() * PAGE_SIZE;

    uint32_t total_mb = (uint32_t)(total_bytes / (1024 * 1024));
    uint32_t mem_percent = (total_bytes > 0) ? (uint32_t)((used_bytes * 100) / total_bytes) : 0;

    char mem_buf[64];
    char tot_s[12], pct_s[8];
    itoa(total_mb, tot_s);
    itoa(mem_percent, pct_s);

    int mp = 0;

    if (used_bytes < 1024 * 1024) {
        uint32_t used_kb = (uint32_t)(used_bytes / 1024);
        char kb_s[12];
        itoa(used_kb, kb_s);

        for (int i = 0; kb_s[i]; i++) mem_buf[mp++] = kb_s[i];
        const char* k_div = " KiB / ";
        for (int i = 0; k_div[i]; i++) mem_buf[mp++] = k_div[i];
    } else {
        uint32_t used_mb_int = (uint32_t)(used_bytes / (1024 * 1024));
        uint32_t used_mb_frac = (uint32_t)(((used_bytes % (1024 * 1024)) * 10) / (1024 * 1024));
        char int_s[12], frac_s[8];
        itoa(used_mb_int, int_s);
        itoa(used_mb_frac, frac_s);

        for (int i = 0; int_s[i]; i++) mem_buf[mp++] = int_s[i];
        mem_buf[mp++] = '.';
        mem_buf[mp++] = frac_s[0] ? frac_s[0] : '0';
        const char* m_div = " MiB / ";
        for (int i = 0; m_div[i]; i++) mem_buf[mp++] = m_div[i];
    }

    for (int i = 0; tot_s[i]; i++) mem_buf[mp++] = tot_s[i];
    const char* m_p1 = " MiB (";
    for (int i = 0; m_p1[i]; i++) mem_buf[mp++] = m_p1[i];
    for (int i = 0; pct_s[i]; i++) mem_buf[mp++] = pct_s[i];
    const char* m_p2 = "%)";
    for (int i = 0; m_p2[i]; i++) mem_buf[mp++] = m_p2[i];
    mem_buf[mp] = '\0';

    uint32_t mem_col = (mem_percent > 80) ? CON_RED : (mem_percent > 50) ? CON_YELLOW : CON_GREEN;
    kputs_at(text_x + 13 * 8, text_y, mem_buf, mem_col);
    text_y += 16;

    // Disk
    kputs_at(text_x, text_y, "Disk (/):    ", CON_CYAN);

    uint32_t disk_used_mb = 0, disk_total_mb = 0;
    fs_get_stats(&disk_used_mb, &disk_total_mb);

    uint32_t disk_percent = 0;
    if (disk_total_mb > 0) {
        disk_percent = (disk_used_mb * 100) / disk_total_mb;
    }

    char disk_buf[64];
    char du_s[12], dt_s[12], dp_s[8];
    itoa(disk_used_mb, du_s);
    itoa(disk_total_mb, dt_s);
    itoa(disk_percent, dp_s);

    int dp = 0;
    for (int i = 0; du_s[i]; i++) disk_buf[dp++] = du_s[i];
    const char* d_div = " MiB / ";
    for (int i = 0; d_div[i]; i++) disk_buf[dp++] = d_div[i];
    for (int i = 0; dt_s[i]; i++) disk_buf[dp++] = dt_s[i];
    const char* d_p1 = " MiB (";
    for (int i = 0; d_p1[i]; i++) disk_buf[dp++] = d_p1[i];
    for (int i = 0; dp_s[i]; i++) disk_buf[dp++] = dp_s[i];
    const char* d_p2 = "%) - fat32";
    for (int i = 0; d_p2[i]; i++) disk_buf[dp++] = d_p2[i];
    disk_buf[dp] = '\0';

    uint32_t disk_col = (disk_percent > 80) ? CON_RED : (disk_percent > 50) ? CON_YELLOW : CON_GREEN;
    kputs_at(text_x + 13 * 8, text_y, disk_buf, disk_col);
    text_y += 16;

    // Audio
    kputs_at(text_x, text_y, "Audio:       ", CON_CYAN);
    kputs_at(text_x + 13 * 8, text_y, pci_get_audio_controller_name(), CON_YELLOW); text_y += 24;

    // Цветовая палитра
    static const uint32_t palette_colors[8] = {
        0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
        0x0000AA, 0xAA00AA, 0x00AAAA, CON_DIM
    };
    static const uint32_t bright_colors[8] = {
        0x555555, CON_RED, CON_GREEN, CON_YELLOW,
        0x5555FF, 0xFF55FF, CON_CYAN, CON_FG
    };

    int box_w = 12;
    int box_h = 10;
    for (int i = 0; i < 8; i++) {
        for (int dy = 0; dy < box_h; dy++) {
            for (int dx = 0; dx < box_w; dx++) {
                put_pixel(text_x + i * box_w + dx, text_y + dy, palette_colors[i]);
                put_pixel(text_x + i * box_w + dx, text_y + dy + box_h, bright_colors[i]);
            }
        }
    }
    text_y += (box_h * 2 + 12);

    con->cursor_x = 0;
    con->cursor_y = start_y + total_h + 48;
    int guard2 = 256;
    while (con->cursor_y + 48 > con->h_px && guard2-- > 0) {
        scroll_screen();
    }
    if (con->cursor_y < 0) con->cursor_y = 0;

    flush_buffer();
}


void force_bios_setup(void) {
    outb(0x70, 0x2E);
    uint8_t csum = inb(0x71);
    outb(0x70, 0x2E);
    outb(0x71, csum ^ 0xFF);

    outb(0x70, 0x0E);
    outb(0x71, 0xC0);

    while (inb(0x64) & 2);
    outb(0x64, 0xFE);

    __asm__ __volatile__("cli; push $0; push $0; lidt (%esp); int $3");
}

void hex_str(uint32_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        out[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    out[10] = '\0';
}

extern void show_bsod(const char* reason, uint32_t error_code, int exc_no, uint64_t fault_eip, uint64_t fault_esp);
extern void handle_cpu_exception(int exc_no, uint64_t fault_eip, uint64_t fault_esp, uint64_t fault_cs,
                                 uint64_t* rip_slot);

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[33];
static struct idt_ptr   idtp;

// Массив оберток для каждого из 32 исключений CPU с передачей их номера
#define DECLARE_ISR(vec) \
    __attribute__((naked)) static void isr_wrapper_##vec(void) { \
        __asm__ __volatile__ ( \
            "pushq $0\n\t"        /* Денди-заглушка кода ошибки, если процессор его не шлет */ \
            "pushq $" #vec "\n\t"  /* Передаем вектор прерывания */ \
            "jmp isr_common_stub\n\t" \
        ); \
    }

// Общая точка входа для исключений CPU (глобальная, без static!)
__attribute__((naked)) void isr_common_stub(void) {
    __asm__ __volatile__ (
        "push %rax\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        
        // Точные смещения с учетом сохраненных регистров (9 * 8 = 72 байта):
        // 72(%rsp) — номер вектора (который мы запушили в макросе DEF_STUB)
        // 88(%rsp) — RIP, сохраненный процессором при прерывании
        // 112(%rsp) — RSP, сохраненный процессором
        "movq 88(%rsp), %rsi\n\t" // RIP -> передаем как fault_eip (в rsi)
        "movq 112(%rsp), %rdx\n\t" // RSP -> передаем как fault_esp (в rdx)
        "movq 72(%rsp), %rdi\n\t" // Номер вектора -> передаем как exc_no (в rdi)
        "movq 96(%rsp), %rcx\n\t" // CS -> fault_cs (кольцо, где случился сбой)
        "leaq 88(%rsp), %r8\n\t"  // адрес ячейки с RIP: обработчик может его сдвинуть

        "call handle_cpu_exception\n\t"

        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rax\n\t"
        "addq $16, %rsp\n\t"      // Очищаем вектор и код ошибки со стека
        "iretq\n\t"
    );
}

// Упрощенные отдельные макросы-точки входа для таблицы IDT
// У части исключений процессор САМ кладёт на стек код ошибки (#DF, #TS, #NP,
// #SS, #GP, #PF, #AC, #CP). Раньше заглушка клала фиктивный код для всех
// подряд, и у этих восьми весь кадр уезжал на 8 байт: вместо RIP читался код
// ошибки, вместо CS -- настоящий RIP. А проверка "сбой был в Ring 3" смотрит
// именно на CS, то есть падение программы по #PF или #GP -- самые частые --
// разбиралось по младшим битам её же RIP и часто уходило в BSOD вместо того,
// чтобы просто убить процесс. Поэтому заглушек теперь две.
#define DEF_STUB(n) \
    __attribute__((naked)) static void isr_stub_##n(void) { \
        __asm__ __volatile__("pushq $0\n\tpushq $" #n "\n\tjmp isr_common_stub"); \
    }
#define DEF_STUB_EC(n) \
    __attribute__((naked)) static void isr_stub_##n(void) { \
        __asm__ __volatile__("pushq $" #n "\n\tjmp isr_common_stub"); \
    }

DEF_STUB(0)  DEF_STUB(1)  DEF_STUB(2)  DEF_STUB(3)
DEF_STUB(4)  DEF_STUB(5)  DEF_STUB(6)  DEF_STUB(7)
DEF_STUB_EC(8)  DEF_STUB(9)  DEF_STUB_EC(10) DEF_STUB_EC(11)
DEF_STUB_EC(12) DEF_STUB_EC(13) DEF_STUB_EC(14) DEF_STUB(15)
DEF_STUB(16) DEF_STUB_EC(17) DEF_STUB(18) DEF_STUB(19)
DEF_STUB(20) DEF_STUB_EC(21) DEF_STUB(22) DEF_STUB(23)
DEF_STUB(24) DEF_STUB(25) DEF_STUB(26) DEF_STUB(27)
DEF_STUB(28) DEF_STUB(29) DEF_STUB(30) DEF_STUB(31)
static int sched_preempt_counter = 0;


void timer_isr_handler(void) {
    timer_ticks++;

    // Тик достаётся той задаче, которая была на процессоре: из этих счётчиков
    // диспетчер задач считает загрузку (простой — это тики pid 0, он же hlt).
    {
        task_t* run = sched_get_current_task();
        if (run) run->cpu_ticks++;
    }

    outb(0x20, 0x20);

    // Звуковую очередь обслуживает ядро, а не программа: иначе стоит плееру
    // замолчать (пауза, переключение TTY), и DMA пойдёт по второму кругу.
    hda_tick();

    // USB-клавиатуры и мыши: отчёты из второго кольца событий xHCI (drivers/usb/hid.c)
    usb_hid_tick();

    // Вытесняем всегда, в том числе задачи в Ring 3 (у каждой свой стек входа в ядро)
    sched_preempt_counter++;
    if (sched_preempt_counter >= 20) {
        sched_preempt_counter = 0;
        sched_yield();
    }
}

void pit_init(uint32_t freq) {
    uint32_t divisor = 1193182 / freq;
    outb(0x43, 0x34); // Канал 0, LSB/MSB, Режим 2 (Rate Generator)
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

void pic_remap(void) {
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    
    outb(PIC1_DATA, 0x20); 
    outb(PIC2_DATA, 0x28); 
    
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

void init_crash_guard_idt(void) {
    uint64_t handlers[32] = {
        (uint64_t)isr_stub_0,  (uint64_t)isr_stub_1,  (uint64_t)isr_stub_2,  (uint64_t)isr_stub_3,
        (uint64_t)isr_stub_4,  (uint64_t)isr_stub_5,  (uint64_t)isr_stub_6,  (uint64_t)isr_stub_7,
        (uint64_t)isr_stub_8,  (uint64_t)isr_stub_9,  (uint64_t)isr_stub_10, (uint64_t)isr_stub_11,
        (uint64_t)isr_stub_12, (uint64_t)isr_stub_13, (uint64_t)isr_stub_14, (uint64_t)isr_stub_15,
        (uint64_t)isr_stub_16, (uint64_t)isr_stub_17, (uint64_t)isr_stub_18, (uint64_t)isr_stub_19,
        (uint64_t)isr_stub_20, (uint64_t)isr_stub_21, (uint64_t)isr_stub_22, (uint64_t)isr_stub_23,
        (uint64_t)isr_stub_24, (uint64_t)isr_stub_25, (uint64_t)isr_stub_26, (uint64_t)isr_stub_27,
        (uint64_t)isr_stub_28, (uint64_t)isr_stub_29, (uint64_t)isr_stub_30, (uint64_t)isr_stub_31
    };

    for (int i = 0; i < 32; i++) {
        idt[i].base_low   = handlers[i] & 0xFFFF;
        idt[i].sel        = 0x08;
        idt[i].always0    = 0;
        idt[i].flags      = 0x8E;
        idt[i].base_high  = (handlers[i] >> 16) & 0xFFFF;
        idt[i].base_upper = (handlers[i] >> 32) & 0xFFFFFFFF;
        idt[i].reserved   = 0;
    }

    // Настройка таймера (IRQ0 / вектор 32)
    uint64_t timer_handler = (uint64_t)timer_isr_asm;
    idt[32].base_low   = timer_handler & 0xFFFF;
    idt[32].sel        = 0x08;
    idt[32].always0    = 0;
    idt[32].flags      = 0x8E;
    idt[32].base_high  = (timer_handler >> 16) & 0xFFFF;
    idt[32].base_upper = (timer_handler >> 32) & 0xFFFFFFFF;
    idt[32].reserved   = 0;

    idtp.limit = (sizeof(struct idt_entry) * 33) - 1;
    idtp.base  = (uint64_t)&idt;

    __asm__ __volatile__("lidt (%0)" : : "r"(&idtp));
}

// Статический буфер для загрузки логотипа (512 КБ с запасом)
static uint8_t bmp_load_buffer[512 * 1024];

void render_boot_logo(void) {
    // Полный путь: положение драйвера теперь выставляет сама ФС по каталогу
    // вызывающего, поэтому "перейти в sys и прочитать logo.bmp" больше не работает
    int bytes = fs_read_file("/sys/logo.bmp", bmp_load_buffer, sizeof(bmp_load_buffer));

    clear_screen(CON_BG); // Чёрный экран по умолчанию 

    // Если файл не найден или меньше минимального размера заголовка BMP (54 байта)
    if (bytes <= 54) {
        int text_x = (screen_width - (13 * 8)) / 2; 
        int text_y = (screen_height / 2) - 8; 
        kputs_at(text_x, text_y, "LOGO NOT FOUND", CON_RED); 
        flush_buffer(); 
        sleep_ms(3500); 
        clear_screen(CON_BG); 
        flush_buffer(); 
        return;
    }

    // Проверяем сигнатуру 'BM' (0x4D42)
    uint16_t* signature = (uint16_t*)bmp_load_buffer;
    if (*signature != 0x4D42) {
        kputs_at(100, 100, "INVALID BMP FORMAT", CON_RED); 
        flush_buffer(); 
        sleep_ms(3500); 
        clear_screen(CON_BG); 
        flush_buffer(); 
        return;
    }

    // Читаем параметры из заголовка BMP
    uint32_t data_offset = *(uint32_t*)(bmp_load_buffer + 10);
    int32_t  bmp_width   = *(int32_t*)(bmp_load_buffer + 18);
    int32_t  bmp_height  = *(int32_t*)(bmp_load_buffer + 22);
    uint16_t bpp         = *(uint16_t*)(bmp_load_buffer + 28);

    if (bpp != 24 && bpp != 32) {
        kputs_at(100, 100, "UNSUPPORTED BMP BPP", CON_RED); 
        flush_buffer(); 
        sleep_ms(3500); 
        clear_screen(CON_BG); 
        flush_buffer(); 
        return;
    }

    int start_x = (screen_width - bmp_width) / 2; 
    int start_y = (screen_height - (bmp_height < 0 ? -bmp_height : bmp_height)) / 2; 

    uint8_t* pixel_data = bmp_load_buffer + data_offset;
    int row_stride = ((bmp_width * (bpp / 8) + 3) & ~3);

    int is_top_down = (bmp_height < 0);
    int abs_height = is_top_down ? -bmp_height : bmp_height;

    for (int y = 0; y < abs_height; y++) {
        int src_y = is_top_down ? y : (abs_height - 1 - y);
        int screen_y = start_y + y;

        if (screen_y < 0 || screen_y >= screen_height) continue; 

        uint8_t* row_ptr = pixel_data + (src_y * row_stride);

        for (int x = 0; x < bmp_width; x++) {
            int screen_x = start_x + x;
            if (screen_x < 0 || screen_x >= screen_width) continue; 

            uint8_t b = row_ptr[x * (bpp / 8) + 0];
            uint8_t g = row_ptr[x * (bpp / 8) + 1];
            uint8_t r = row_ptr[x * (bpp / 8) + 2];

            uint32_t color = (r << 16) | (g << 8) | b;

            if (color != 0x000000) {
                put_pixel(screen_x, screen_y, color); 
            }
        }
    }

    flush_buffer(); 
    sleep_ms(1500); 
    clear_screen(CON_BG); 
    flush_buffer(); 
}

void print_tty_banner(int tty_id) {
    kputs(OS_NAME " " OS_ARCH " " OS_VERSION " / tty", CON_GREEN);
    char num_buf[16];
    itoa(tty_id + 1, num_buf);
    kputs(num_buf, CON_CYAN);
    kputs("\n\n", CON_FG);

    kputs("Защита от ДОЛБАЁБОВ!!! не вытаскивайте флешку с ОС а если и вытащили не втыкайте, не поможет.\n\n", CON_DIM);
    // Листинг корня теперь показывается после входа (login.c), как motd
}

// Наш первый настоящий float-таймер высокого разрешения!
float get_uptime_seconds(void) {
    if (g_tsc_per_ms > 0) {
        // rdtsc64() возвращает количество тактов со старта процессора.
        // Делим на (такты_в_мс * 1000), чтобы получить секунды с микросекундной точностью.
        return (float)rdtsc64() / ((float)g_tsc_per_ms * 1000.0f);
    }
    // Запасной вариант через прерывания PIT, если калибровка еще не прошла
    return (float)timer_ticks / 1000.0f;
}

void check_and_run_autostart(void) {
    fs_file_info_t auto_files[16];
    fs_lock();
    int count = fs_get_dir_files("/autostart", auto_files, 16);
    fs_unlock();

    if (count <= 0) return;

    for (int i = 0; i < count; i++) {
        // Пропускаем директории
        if (auto_files[i].attr & 0x10) continue;

        const char* fn = auto_files[i].name;
        int len = 0;
        while (fn[len]) len++;

        // Проверяем расширение .prg / .PRG
        if (len > 4 && (fn[len-4] == '.' &&
           (fn[len-3] == 'p' || fn[len-3] == 'P') &&
           (fn[len-2] == 'r' || fn[len-2] == 'R') &&
           (fn[len-1] == 'g' || fn[len-1] == 'G'))) {

            char path[64] = "/autostart/";
            int p = 11;
            for (int k = 0; fn[k] && p < 63; k++) {
                path[p++] = fn[k];
            }
            path[p] = '\0';

            // Запускаем программу в графическом/пользовательском режиме.
            // Если не вышло -- говорим об этом: молчащий автозапуск однажды
            // стоил мне часа поисков (рабочий стол просто не появлялся).
            int rc = prog_load_module(path, 0);
            if (rc != 0) {
                char num[16]; itoa(rc, num);
                kputs("[автозапуск] ", 0xFF5555);
                kputs(path, 0xFFFFFF);
                kputs(" завершилась с кодом ", 0xFF5555);
                kputs(num, 0xFFFFFF);
                kputs("\n", 0xFF5555);
            }
        }
    }
}

void kernel_main(void) {
    vbe_info_t* vbe = (vbe_info_t*)(uintptr_t)0x8000;

    screen_width = vbe->width;
    screen_height = vbe->height;
    screen_pitch = vbe->pitch;

    // --- Физическая память: карту отдал BIOS (INT 15h E820) в stage2 ---------
    // Раньше объём был прошит константой 512 МБ, и на машине с 8 или 16 ГБ
    // система использовала шестнадцатую часть ОЗУ.
    uint32_t  e820_count = *(volatile uint32_t*)(uintptr_t)E820_COUNT_ADDR;
    e820_entry_t* e820   = (e820_entry_t*)(uintptr_t)E820_LIST_ADDR;

    uint64_t ram_top = 0;
    if (e820_count > 0 && e820_count <= 96) {
        for (uint32_t i = 0; i < e820_count; i++) {
            if (e820[i].type != E820_USABLE) continue;
            uint64_t end = e820[i].base + e820[i].length;
            if (end > ram_top) ram_top = end;
        }
    }
    // Битмап PMM лежит по 0x1F00000 и занимает не больше мегабайта, то есть
    // описывает до 32 ГБ; прямое окно vmm ограничено 64 ГБ. Берём меньшее.
    if (ram_top > 32ULL * 1024 * 1024 * 1024) ram_top = 32ULL * 1024 * 1024 * 1024;
    if (ram_top < 64ULL * 1024 * 1024) ram_top = 512ULL * 1024 * 1024;   // BIOS промолчал
    g_ram_bytes = ram_top;

    pmm_init(ram_top);
    vmm_init(ram_top);
    enable_write_combining();

    // Свободно только то, что BIOS назвал пригодным: ACPI, NVS и дыры
    // оставляем занятыми, иначе ядро рано или поздно затрёт таблицы прошивки.
    if (e820_count > 0 && e820_count <= 96) {
        for (uint32_t i = 0; i < e820_count; i++) {
            if (e820[i].type != E820_USABLE) continue;
            uint64_t base = e820[i].base, len = e820[i].length;
            if (base >= ram_top) continue;
            if (base + len > ram_top) len = ram_top - base;
            pmm_mark_region_free(base, len);
        }
    } else {
        pmm_mark_region_free(0x000000, ram_top);
    }

    // Защищаем первые 32 МБ физической памяти:
    // 0-1 МБ   — BIOS, IVT, VBE Info
    // 1-16 МБ  — Код ядра, Data, BSS (back_buffer 8 МБ, temp_buf 4 МБ, IDT, TSS)
    // 16-17 МБ — pmm_bitmap (по адресу 0x1000000)
    // 17-32 МБ — Резерв структур ядра
    // 32-36 МБ — область загрузки .SYS-модулей (SYS_LOAD_BASE = 0x2000000)
    pmm_mark_region_used(0x000000, 36 * 1024 * 1024);

    uintptr_t lfb_phys = (uintptr_t)vbe->lfb_ptr;
    // (g_lfb_wc_entries — сколько записей таблиц переведено в WC, для загрузочного лога)
    size_t fb_size = screen_pitch * screen_height;

    for (size_t offset = 0; offset < fb_size; offset += 0x1000) {
        vmm_map_page(vmm_get_kernel_pml4_phys(), 
                     lfb_phys + offset, 
                     lfb_phys + offset, 
                     VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_WC);
    }

    // Цикл выше покрывает только адреса без готового отображения; LFB почти
    // всегда внутри identity-области из 2 МБ-страниц, поэтому тип WC ставим
    // прямо в существующих записях — и в identity, и в прямом окне DMAP
    // (два отображения одной памяти с разными типами процессор не любит)
    g_lfb_wc_entries = vmm_set_wc(lfb_phys, fb_size);
    vmm_set_wc(DMAP_BASE + lfb_phys, fb_size);

    lfb = (uint32_t*)lfb_phys;

    // Консоль живёт в tty_t, поэтому TTY1 надо поднять ДО первого kputs: иначе
    // загрузочные сообщения и логотип уходили в никуда (буфера ещё нет)
    tty_init_core();

    g_term_hook = 0;
    de_stream_idx = 0;
    de_stream_line[0] = '\0';

    init_user_mode();

    pic_remap();                              // Переносим IRQ 0-7 на векторы 32-39
    pit_init(1000);                     // Запускаем PIT на частоту 1000 Гц
    
    uint8_t pic_mask = inb(0x21);
    outb(0x21, pic_mask & ~0x01);  // Разрешаем IRQ0 (таймер) на контроллере PIC

    __asm__ volatile ("sti");
    tsc_calibrate();

    // Процессор: паспорт, снятие запрета турбо и запрос максимальной частоты.
    // Строго ПОСЛЕ init_user_mode(): там ставится настоящая таблица прерываний,
    // а без неё #GP на отсутствующем MSR некому было бы перехватить.
    cpu_init();

    //init_crash_guard_idt();

    init_mc_monitor();

    kernel_system_bootstrap();

    render_boot_logo();

    sched_init();
    // init_ps2_mouse() теперь поднимает загрузчик (kernel_system_bootstrap),
    // вместе с остальным железом и с отчётом о результате

    task_create_kernel(tty1_task_entry, "tty1_shell")->tty_id = 0;

    // storaged: горячее подключение USB, тома в /media и сторож корневой
    // флешки (см. fs/mount.c). Создаётся после sched_init: это поток ядра.
    mount_start();

    uint8_t cursor_visible = 1;
    uint32_t last_blink = 0;

    while (1) {
        //keyboard_poll_handler();
        ps2_hw_service();   // клавиатура: единый читатель, раскладывает по очередям TTY + ловит Ctrl+C
        update_mouse_state();

        sleep_ms(2);   // чаще опрашиваем PS/2: мышь не должна ждать 10 мс на каждый пакет

        if ((timer_ticks - last_blink) >= 500) {
            cursor_visible = !cursor_visible;
            tty_t* at = tty_get(tty_get_active_id());
            // экраном владеет модуль (gfx_mode) или программа пишет кадры в /dev/fb0 — курсор консоли не трогаем
            int fb_busy = (g_fb_write_tty == tty_get_active_id()) && ((timer_ticks - g_fb_last_write_tick) < 2000);
            if (!(at && at->gfx_mode) && !fb_busy) {
                draw_cursor(cursor_visible ? CON_CURSOR : at->bg_color);
                flush_dirty();
            }
            last_blink = timer_ticks;

            // init: добираем сирот (родитель умер раньше ребёнка)
            sched_reap_orphans();
            smp_poll();                 // упавший на рабочем ядре поток снимает свой процесс

            // Частота меряется по приросту APERF/MPERF, то есть нужен
            // предыдущий замер. Снимаем его тут, раз в полсекунды: кто бы
            // потом ни спросил, значение будет свежим, а не паспортным.
            cpu_current_mhz();
        }

        sched_yield();
    }
}