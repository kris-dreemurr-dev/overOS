#include <stdint.h>
#include <stddef.h>
#include "../font/font.h"
#include "../files/image/image.h"
#include "../drivers/display.h"
#include "../drivers/pci.h"
#include "../files/image/sprite.h"
#include "../fs/fat16.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "user_mode.h"
#include "sched.h"
#include "tty.h"

#ifndef IMAGE_WIDTH
#define IMAGE_WIDTH  320
#endif
#ifndef IMAGE_HEIGHT
#define IMAGE_HEIGHT 240
#endif

// ==================== [ OS System Config ] ====================
#define  OS_NAME            "overOS"
#define  OS_LOWER_NAME      "overos"
#define  OS_ARCH            "x86_64"
#define  OS_VERSION         "0.0.1"
int      kernel_debug =     1;
uint32_t current_bg_color = 0x000000;
// =================================================================

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
uint16_t screen_width;
uint16_t screen_height;
uint16_t screen_pitch;

#define MAX_SCREEN_WIDTH  1920
#define MAX_SCREEN_HEIGHT 1080
uint32_t back_buffer[MAX_SCREEN_WIDTH * MAX_SCREEN_HEIGHT];
uint32_t* current_draw_buffer = back_buffer;

#define HIST_HEIGHT 64
static uint32_t screen_history[HIST_HEIGHT * MAX_SCREEN_WIDTH];
int total_scrolled_px = 0;
int scroll_offset_px = 0;

int cursor_x = 0;
int cursor_y = 0;
int prompt_min_x = 0;

//static uint32_t saved_console_buffer[MAX_SCREEN_WIDTH * MAX_SCREEN_HEIGHT];
static int saved_cursor_x = 0;
static int saved_cursor_y = 0;
static int saved_total_scrolled_px = 0;
static int saved_scroll_offset_px = 0;

char current_path[128] = "/";

// Динамический хук потока вывода терминала (для модулей типа DE.SYS)
static void (*g_term_hook)(const char* text) = 0;

void set_term_hook(void (*hook)(const char* text)) {
    g_term_hook = hook;
}

static int console_is_saved = 0;

// TTY, которому принадлежит вызывающая задача (для модулей — TTY, где их запустили)
static tty_t* console_task_tty(void) {
    task_t* me = sched_get_current_task();
    int id = (me && me->tty_id >= 0) ? me->tty_id : tty_get_active_id();
    return tty_get(id);
}

void console_save_state(void) {
    tty_t* owner = console_task_tty();
    if (owner) owner->gfx_mode = 1;   // экран занят модулем: мигание курсора консоли выключено
    if (console_is_saved) return; // Не перезаписываем снимок повторно во время работы подмодулей!
    uint32_t total_pixels = screen_width * screen_height;
    for (uint32_t i = 0; i < total_pixels; i++) {
        //saved_console_buffer[i] = back_buffer[i];
    }
    saved_cursor_x = cursor_x;
    saved_cursor_y = cursor_y;
    saved_total_scrolled_px = total_scrolled_px;
    saved_scroll_offset_px = scroll_offset_px;
    console_is_saved = 1;
}

void console_restore_state(void) {
    tty_t* owner = console_task_tty();
    if (owner) owner->gfx_mode = 0;
    uint32_t stride = screen_pitch / 4;
    uint32_t total_pixels = screen_width * screen_height;

    for (uint32_t i = 0; i < total_pixels; i++) {
        //back_buffer[i] = saved_console_buffer[i];
    }
    cursor_x = saved_cursor_x;
    cursor_y = saved_cursor_y;
    total_scrolled_px = saved_total_scrolled_px;
    scroll_offset_px = saved_scroll_offset_px;

    current_bg_color = 0x000000;

    for (int y = 0; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            lfb[y * stride + x] = back_buffer[y * screen_width + x];
        }
    }
    console_is_saved = 0;
}

extern void usb_controller_init(void);
extern void ehci_init(void);
extern void keyboard_poll_handler(void);
extern void init_ps2_mouse(void);
extern void update_mouse_state(void);
extern void draw_cursor_shape(int x, int y);
extern int get_mouse_x(void);
extern int get_mouse_y(void);
extern int get_mouse_btn(void);
extern volatile int g_user_mode_active;
extern void return_from_user_mode(int exit_code);
extern uint64_t* vmm_get_kernel_pml4_phys(void);
extern void timer_isr_asm(void);

#define MAX_INPUT 256
extern char input_buffer[MAX_INPUT];
extern int input_len;
extern int kbd_layout;
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
        // Уступаем процессор другим задачам, пока ждём тика
        sched_yield();
        __asm__ volatile ("pause");
    }
}

void wait(uint32_t seconds) {
    sleep_ms(seconds * 1000);
}

void wait_vsync(void) {
    while ((inb(0x3DA) & 0x08) == 0x08);
    while ((inb(0x3DA) & 0x08) == 0);
}

void flush_buffer(void) {
    uint8_t* lfb_bytes = (uint8_t*)lfb;

    if (scroll_offset_px == 0) {
        if (screen_pitch == screen_width * sizeof(uint32_t)) {
            uint64_t* dst = (uint64_t*)lfb_bytes;
            const uint64_t* src = (const uint64_t*)current_draw_buffer;
            size_t qwords = ((size_t)screen_width * screen_height) / 2;

            __asm__ volatile (
                "rep movsq"
                : "+D"(dst), "+S"(src), "+c"(qwords)
                :
                : "memory"
            );
            return;
        }

        size_t qwords_per_line = ((size_t)screen_width * sizeof(uint32_t)) / 8;
        for (int y = 0; y < screen_height; y++) {
            uint64_t* dst = (uint64_t*)(lfb_bytes + (y * (uint32_t)screen_pitch));
            const uint64_t* src = (const uint64_t*)(&current_draw_buffer[y * screen_width]);

            __asm__ volatile (
                "rep movsq"
                : "+D"(dst), "+S"(src), "+c"(qwords_per_line)
                :
                : "memory"
            );
        }
    } else {
        int virtual_y = total_scrolled_px - scroll_offset_px;
        for (int y = 0; y < screen_height; y++) {
            int src_y = virtual_y + y;
            volatile uint32_t* dst = (volatile uint32_t*)(lfb_bytes + (y * (uint32_t)screen_pitch));
            for (int x = 0; x < screen_width; x++) {
                uint32_t color = 0x000000;
                if (src_y >= 0 && src_y < total_scrolled_px) {
                    int hist_y = src_y % HIST_HEIGHT;
                    color = screen_history[hist_y * MAX_SCREEN_WIDTH + x];
                } else if (src_y >= total_scrolled_px) {
                    int back_y = src_y - total_scrolled_px;
                    if (back_y < screen_height) {
                        color = current_draw_buffer[back_y * screen_width + x];
                    }
                }
                dst[x] = color;
            }
        }
    }
}

void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= screen_width || y < 0 || y >= screen_height) return;
    current_draw_buffer[y * screen_width + x] = color;
}

void draw_cursor(uint32_t color) {
    for (int row = 14; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            put_pixel(cursor_x + col, cursor_y + row, color);
        }
    }
}

void clear_screen(uint32_t color) {
    current_bg_color = color;
    uint32_t total_pixels = (uint32_t)screen_width * screen_height;
    for (uint32_t i = 0; i < total_pixels; i++) {
        current_draw_buffer[i] = color;
    }
    cursor_x = 0;
    cursor_y = 0;
    total_scrolled_px = 0;
    scroll_offset_px = 0;
}

void scroll_screen(void) {
    for (int y = 0; y < 16; y++) {
        int hist_y = (total_scrolled_px + y) % HIST_HEIGHT;
        for (int x = 0; x < screen_width; x++) {
            screen_history[hist_y * MAX_SCREEN_WIDTH + x] = current_draw_buffer[y * screen_width + x];
        }
    }
    total_scrolled_px += 16;

    for (int y = 0; y < screen_height - 16; y++) {
        for (int x = 0; x < screen_width; x++) {
            current_draw_buffer[y * screen_width + x] = current_draw_buffer[(y + 16) * screen_width + x];
        }
    }
    for (int y = screen_height - 16; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            current_draw_buffer[y * screen_width + x] = current_bg_color;
        }
    }
    cursor_y -= 16;
}

static uint8_t utf8_to_cp866(uint8_t b1, uint8_t b2) {
    if (b1 == 0xD0) {
        if (b2 >= 0x90 && b2 <= 0xBF) return (uint8_t)(b2 - 0x90 + 0x80);
        if (b2 == 0x81) return 0x85;
    }
    if (b1 == 0xD1) {
        if (b2 >= 0x80 && b2 <= 0x8F) return (uint8_t)(b2 - 0x80 + 0xE0);
        if (b2 == 0x91) return 0xA5;
    }
    return '?';
}

// Отрисовка символа с возможностью прозрачного фона
void draw_char_ex(int start_x, int start_y, char c, uint32_t color, int opaque) {
    unsigned char uc = (unsigned char)c;
    const uint8_t* glyph = &fontdata_ru_8x16[uc * 16];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                put_pixel(start_x + col, start_y + row, color);
            } else if (opaque) {
                put_pixel(start_x + col, start_y + row, current_bg_color);
            }
        }
    }
}

// Стандартный draw_char (непрозрачный для консоли)
void draw_char(int start_x, int start_y, char c, uint32_t color) {
    draw_char_ex(start_x, start_y, c, color, 1);
}

void kputs_at(int x, int y, const char* str, uint32_t color) {
    int cx = x;
    uint8_t utf8_prefix = 0;

    while (*str) {
        uint8_t uc = (uint8_t)*str;
        if (uc == 0xD0 || uc == 0xD1) {
            utf8_prefix = uc;
            str++;
            continue;
        }
        if (utf8_prefix) {
            uc = utf8_to_cp866(utf8_prefix, uc);
            utf8_prefix = 0;
        }
        // [ФИКС]: opaque = 0, чтобы фон окна не затирался цветом обоев
        draw_char_ex(cx, y, (char)uc, color, 0);
        cx += 8;
        str++;
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

    static uint8_t utf8_prefix = 0;
    uint8_t uc = (uint8_t)c;

    if (uc == 0xD0 || uc == 0xD1) {
        utf8_prefix = uc;
        return;
    }
    if (utf8_prefix) {
        uc = utf8_to_cp866(utf8_prefix, uc);
        utf8_prefix = 0;
    }

    draw_cursor(current_bg_color);

    if (uc == '\b') {
        if (cursor_x >= 8) {
            cursor_x -= 8;
            for (int dy = 0; dy < 16; dy++) {
                for (int dx = 0; dx < 8; dx++) {
                    put_pixel(cursor_x + dx, cursor_y + dy, current_bg_color);
                }
            }
            draw_cursor(0xFFFFFF);
        }
        return;
    }

    if (uc == '\n') {
        cursor_x = 0;
        cursor_y += 16;
    } else if (uc == '\r') {
        cursor_x = 0;
    } else {
        draw_char(cursor_x, cursor_y, (char)uc, color);
        cursor_x += 8;
        if (cursor_x + 8 > screen_width) {
            cursor_x = 0;
            cursor_y += 16;
        }
    }

    if (cursor_y + 16 > screen_height) {
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
    if (kernel_debug) {
        kputs(str, color);
    }
}

uint32_t get_back_pixel(int x, int y) {
    if (x < 0 || x >= screen_width || y < 0 || y >= screen_height) return 0;
    return current_draw_buffer[y * screen_width + x];
}

void print_prompt(void) {
    kputs("root@devos:", 0x55FF55);
    kputs(current_path, 0x55FFFF);
    kputs("$ ", 0xFFFFFF);
    flush_buffer();
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
    clear_screen(0x000000);

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

void cmd_fastfetch(void) {
    char buf[32];
    uint32_t uptime_sec = timer_ticks / 1000;

    int total_h = FASTFETCH_HEIGHT > 140 ? FASTFETCH_HEIGHT : 140;

    while (cursor_y + total_h > screen_height) {
        scroll_screen();
    }

    int start_x = cursor_x;
    int start_y = cursor_y;

    int sprite_x = start_x + 10;
    int sprite_y = start_y + (total_h - FASTFETCH_HEIGHT) / 2;

    for (int y = 0; y < FASTFETCH_HEIGHT; y++) {
        for (int x = 0; x < FASTFETCH_WIDTH; x++) {
            uint32_t color = fastfetch_sprite_data[y * FASTFETCH_WIDTH + x];
            if (color != 0x000000) {
                put_pixel(sprite_x + x, sprite_y + y, color);
            }
        }
    }

    int text_x = sprite_x + FASTFETCH_WIDTH + 20;
    int text_y = start_y;

    kputs_at(text_x, text_y, "root@devos", 0xFFFFFF); text_y += 18;
    kputs_at(text_x, text_y, "----------", 0xAAAAAA); text_y += 18;

    kputs_at(text_x, text_y, "OS:         ", 0x55FFFF);
    kputs_at(text_x + 12 * 8, text_y, "devOS (x86_64)", 0xFFFFFF); text_y += 18;

    kputs_at(text_x, text_y, "Kernel:     ", 0x55FFFF);
    kputs_at(text_x + 12 * 8, text_y, "devOS Custom VBE", 0xFFFFFF); text_y += 18;

    kputs_at(text_x, text_y, "Uptime:     ", 0x55FFFF);
    char uptime_msg[64] = "";
    int u_idx = 0;
    itoa(uptime_sec, buf);
    for (int i = 0; buf[i] != '\0'; i++) uptime_msg[u_idx++] = buf[i];
    const char* sec_str = " seconds";
    for (int i = 0; sec_str[i] != '\0'; i++) uptime_msg[u_idx++] = sec_str[i];
    uptime_msg[u_idx] = '\0';
    kputs_at(text_x + 12 * 8, text_y, uptime_msg, 0xFFFFFF); text_y += 18;

    kputs_at(text_x, text_y, "Resolution: ", 0x55FFFF);
    char res_msg[64] = "";
    int r_idx = 0;
    char w_buf[16], h_buf[16];
    itoa(screen_width, w_buf);
    itoa(screen_height, h_buf);
    for (int i = 0; w_buf[i] != '\0'; i++) res_msg[r_idx++] = w_buf[i];
    res_msg[r_idx++] = 'x';
    for (int i = 0; h_buf[i] != '\0'; i++) res_msg[r_idx++] = h_buf[i];
    res_msg[r_idx] = '\0';
    kputs_at(text_x + 12 * 8, text_y, res_msg, 0xFFFFFF); text_y += 18;

    kputs_at(text_x, text_y, "Audio:      ", 0x55FFFF);
    char audio_msg[128] = "";
    int a_idx = 0;
    const char* audio_name = pci_get_audio_controller_name();
    for (int i = 0; audio_name[i] != '\0' && a_idx < 120; i++) audio_msg[a_idx++] = audio_name[i];
    audio_msg[a_idx] = '\0';
    kputs_at(text_x + 12 * 8, text_y, audio_msg, 0xFFFF55); text_y += 24;

    cursor_x = 0;
    cursor_y = start_y + total_h + 16;

    while (cursor_y + 16 > screen_height) {
        scroll_screen();
    }

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

extern void show_bsod(const char* reason, uint32_t error_code, int exc_no, uint32_t fault_eip, uint32_t fault_esp);

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
    uint32_t base_upper;
    uint32_t reserved;   // <--- Обязательные 4 байта для x86_64 (размер структуры станет 16 байт)
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;       // 64-битный базовый адрес IDT
} __attribute__((packed));

static struct idt_entry idt[33];
static struct idt_ptr   idtp;

extern void handle_cpu_exception(int exc_no, uint64_t fault_eip, uint64_t fault_esp);

void default_exception_handler(uint64_t* frame) {
    g_term_hook = 0;
    // см. пункт 2 — frame нужно формировать правильно
    int      exc_no    = (int)frame[15];   // вектор
    uint64_t fault_eip = frame[17];        // RIP из аппаратного фрейма
    uint64_t fault_esp = frame[20];        // RSP (см. нюанс ниже про ring3)

    handle_cpu_exception(exc_no, fault_eip, fault_esp);
}

__attribute__((naked)) static void isr_stub(void) {
    __asm__ __volatile__(
        "cli\n\t"
        "call default_exception_handler\n\t"
        "iretq\n\t"
    );
}

static int sched_preempt_counter = 0;


void timer_isr_handler(void) {
    timer_ticks++;

    outb(0x20, 0x20);

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
    uint64_t handler = (uint64_t)isr_stub;

    for (int i = 0; i < 32; i++) {
        idt[i].base_low   = handler & 0xFFFF;
        idt[i].sel        = 0x08;
        idt[i].always0    = 0;
        idt[i].flags      = 0x8E;
        idt[i].base_high  = (handler >> 16) & 0xFFFF;
        idt[i].base_upper = (handler >> 32) & 0xFFFFFFFF;
        idt[i].reserved   = 0;
    }

    // Настраиваем вектор 32 (аппаратный таймер PIT / IRQ0) через чистый ассемблер
    uint64_t timer_handler = (uint64_t)timer_isr_asm;
    idt[32].base_low   = timer_handler & 0xFFFF;
    idt[32].sel        = 0x08;        // Селектор сегмента кода ядра
    idt[32].always0    = 0;
    idt[32].flags      = 0x8E;        // Атрибуты (Present, DPL=0, Interrupt Gate)
    idt[32].base_high  = (timer_handler >> 16) & 0xFFFF;
    idt[32].base_upper = (timer_handler >> 32) & 0xFFFFFFFF;
    idt[32].reserved   = 0;

    idtp.limit = (sizeof(struct idt_entry) * 33) - 1;
    idtp.base  = (uint64_t)&idt;

    __asm__ __volatile__("lidt (%0)" : : "r"(&idtp));
}

void print_tty_banner(int tty_id) {
    kputs(OS_NAME " " OS_ARCH " " OS_VERSION " / tty", 0x55FF55);
    char num_buf[16];
    itoa(tty_id + 1, num_buf);
    kputs(num_buf, 0x55FFFF);
    kputs("\n\n", 0xFFFFFF);
}

void kernel_main(void) {
    vbe_info_t* vbe = (vbe_info_t*)(uintptr_t)0x8000; 

    screen_width = vbe->width;
    screen_height = vbe->height;
    screen_pitch = vbe->pitch;

    pmm_init(512 * 1024 * 1024); 
    vmm_init();
    pmm_mark_region_free(0x000000, 512 * 1024 * 1024);

    // Защищаем первые 32 МБ физической памяти:
    // 0-1 МБ   — BIOS, IVT, VBE Info
    // 1-16 МБ  — Код ядра, Data, BSS (back_buffer 8 МБ, temp_buf 4 МБ, IDT, TSS)
    // 16-17 МБ — pmm_bitmap (по адресу 0x1000000)
    // 17-32 МБ — Резерв структур ядра
    // 32-36 МБ — область загрузки .SYS-модулей (SYS_LOAD_BASE = 0x2000000)
    pmm_mark_region_used(0x000000, 36 * 1024 * 1024);

    uintptr_t lfb_phys = (uintptr_t)vbe->lfb_ptr;
    size_t fb_size = screen_pitch * screen_height;

    for (size_t offset = 0; offset < fb_size; offset += 0x1000) {
        vmm_map_page(vmm_get_kernel_pml4_phys(), lfb_phys + offset, lfb_phys + offset, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    }

    lfb = (uint32_t*)lfb_phys;


    g_term_hook = 0;
    de_stream_idx = 0;
    de_stream_line[0] = '\0';

    init_crash_guard_idt();
    init_user_mode();

    pic_remap();                              // Переносим IRQ 0-7 на векторы 32-39
    pit_init(1000);                     // Запускаем PIT на частоту 1000 Гц
    
    uint8_t pic_mask = inb(0x21);
    outb(0x21, pic_mask & ~0x01);  // Разрешаем IRQ0 (таймер) на контроллере PIC

    init_mc_monitor();
    init_ps2_mouse();
    clear_screen(0x000000);

    ehci_init();
    clear_screen(0x000000);
    flush_buffer();
    kputs("[", 0xFFFFFF); kputs(" OK ", 0x55FF55); kputs("] EHCI init\n", 0xFFFFFF);
    flush_buffer();
    sleep_ms(100);
    kputs("[", 0xFFFFFF); kputs(" OK ", 0x55FF55); kputs("] Display driver init\n", 0xFFFFFF);
    flush_buffer();
    intel_set_backlight(100, 1);
    sleep_ms(100);
    kputs("[", 0xFFFFFF); kputs(" OK ", 0x55FF55); kputs("] File System mounted\n", 0xFFFFFF);
    flush_buffer();
    //fat16_dir();
    kputs("\n", 0xFFFFFF);
    flush_buffer();
    sleep_ms(100);
    kbd_layout = 0;

    kputs("DevOS (Дев-Билд) теперь на ", 0x55FF55);
    kputs("ру", 0xFFFFFF);
    kputs("сск", 0x2277FF);
    kputs("ом\n\n", 0xFF2222);

    kputs("Защита от ДОЛБАЁБОВ!!! не вытаскивайте флешку с ОС а если и вытащили не втыкайте, не поможет.\n\n", 0x55FF55);

    input_len = 0;
    for (int i = 0; i < MAX_INPUT; i++) {
        input_buffer[i] = '\0';
    }

    tty_init_core();
    sched_init();

    task_create_kernel(tty1_task_entry, "tty1_shell")->tty_id = 0;

    __asm__ volatile ("sti");
    tsc_calibrate();

    uint8_t cursor_visible = 1;
    uint32_t last_blink = 0;

    while (1) {
        //keyboard_poll_handler();
        update_mouse_state();

        sleep_ms(10);

        if ((timer_ticks - last_blink) >= 500) {
            cursor_visible = !cursor_visible;
            tty_t* at = tty_get(tty_get_active_id());
            if (!(at && at->gfx_mode)) {   // если экраном владеет модуль — курсор консоли не трогаем
                draw_cursor(cursor_visible ? 0xFFFFFF : current_bg_color);
                flush_buffer();
            }
            last_blink = timer_ticks;
        }

        sched_yield();
    }
}