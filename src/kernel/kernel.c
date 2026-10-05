#include <stdint.h>
#include <stddef.h>
#include "../font/font.h"
#include "../drivers/display.h"
#include "../drivers/pci.h"
#include "../fs/fs.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "user_mode.h"
#include "sched.h"
#include "tty.h"
#include "config.h"
#include "loader_kernel.h"

#include "../files/image/sprite.h"
#include "../files/image/image.h"

uint32_t current_bg_color = 0x000000;
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
    if (DEBUG_MODE) {
        kputs(str, color);
    }
}

uint32_t get_back_pixel(int x, int y) {
    if (x < 0 || x >= screen_width || y < 0 || y >= screen_height) return 0;
    return current_draw_buffer[y * screen_width + x];
}

void print_prompt(void) {
    kputs("root@" OS_LOWER_NAME ":", 0x55FF55);
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

    int total_h = scaled_h > 230 ? scaled_h : 230;

    while (cursor_y + total_h > screen_height) {
        scroll_screen();
    }

    int start_x = cursor_x;
    int start_y = cursor_y;

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
        kputs_at(sprite_x, sprite_y + (r * 16), logo_ascii[r], 0x55FFFF);
    }
#endif

    // Сдвигаем текстовый блок с учетом ширины логотипа
    int text_x = sprite_x + scaled_w + 24;
    int text_y = start_y;

    // Имя хоста
    kputs_at(text_x, text_y, "root@" OS_LOWER_NAME, 0x55FF55); text_y += 16;
    kputs_at(text_x, text_y, "----------------------------", 0x666666); text_y += 16;

    // OS & Kernel
    kputs_at(text_x, text_y, "OS:          ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, OS_NAME " x86_64", 0xFFFFFF); text_y += 16;

    char host_str[64];
    get_smbios_host(host_str, sizeof(host_str));
    kputs_at(text_x, text_y, "Host:        ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, host_str, 0xFFFFFF); text_y += 16;

    kputs_at(text_x, text_y, "Kernel:      ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, OS_NAME " " OS_VERSION " (" OS_ARCH ")", 0xFFFFFF);
    text_y += 16;

    // Uptime
    kputs_at(text_x, text_y, "Uptime:      ", 0x55FFFF);
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
    kputs_at(text_x + 13 * 8, text_y, up_msg, 0xFFFFFF); text_y += 16;

    // Display
    kputs_at(text_x, text_y, "Display:     ", 0x55FFFF);
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
    kputs_at(text_x + 13 * 8, text_y, res_buf, 0xFFFFFF); 
    text_y += 16;

    // Shell
    kputs_at(text_x, text_y, "Shell:       ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, "bash-lite (overOS)", 0xFFFFFF); 
    text_y += 16;

    // Terminal
    kputs_at(text_x, text_y, "Terminal:    ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, "kernel-console", 0xFFFFFF); 
    text_y += 16;

    // Font
    kputs_at(text_x, text_y, "Font:        ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, "8x16-tty (CP866)", 0xFFFFFF); 
    text_y += 16;

    // CPU
    char cpu_str[48];
    get_cpu_brand(cpu_str);
    kputs_at(text_x, text_y, "CPU:         ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, cpu_str, 0xFFFFFF); text_y += 16;

    // GPU
    char gpu_str[36];
    get_pci_gpu(gpu_str);
    kputs_at(text_x, text_y, "GPU:         ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, gpu_str, 0xFFFFFF); text_y += 16;

    // Memory
    kputs_at(text_x, text_y, "Memory:      ", 0x55FFFF);

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

    uint32_t mem_col = (mem_percent > 80) ? 0xFF5555 : (mem_percent > 50) ? 0xFFFF55 : 0x55FF55;
    kputs_at(text_x + 13 * 8, text_y, mem_buf, mem_col);
    text_y += 16;

    // Disk
    kputs_at(text_x, text_y, "Disk (/):    ", 0x55FFFF);

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

    uint32_t disk_col = (disk_percent > 80) ? 0xFF5555 : (disk_percent > 50) ? 0xFFFF55 : 0x55FF55;
    kputs_at(text_x + 13 * 8, text_y, disk_buf, disk_col);
    text_y += 16;

    // Audio
    kputs_at(text_x, text_y, "Audio:       ", 0x55FFFF);
    kputs_at(text_x + 13 * 8, text_y, pci_get_audio_controller_name(), 0xFFFF55); text_y += 24;

    // Цветовая палитра
    static const uint32_t palette_colors[8] = {
        0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
        0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA
    };
    static const uint32_t bright_colors[8] = {
        0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
        0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
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

    cursor_x = 0;
    cursor_y = start_y + total_h + 48;
    while (cursor_y + 48 > screen_height) {
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

extern void show_bsod(const char* reason, uint32_t error_code, int exc_no, uint64_t fault_eip, uint64_t fault_esp);
extern void handle_cpu_exception(int exc_no, uint64_t fault_eip, uint64_t fault_esp);

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
#define DEF_STUB(n) \
    __attribute__((naked)) static void isr_stub_##n(void) { \
        __asm__ __volatile__("pushq $0\n\tpushq $" #n "\n\tjmp isr_common_stub"); \
    }

DEF_STUB(0)  DEF_STUB(1)  DEF_STUB(2)  DEF_STUB(3)
DEF_STUB(4)  DEF_STUB(5)  DEF_STUB(6)  DEF_STUB(7)
DEF_STUB(8)  DEF_STUB(9)  DEF_STUB(10) DEF_STUB(11)
DEF_STUB(12) DEF_STUB(13) DEF_STUB(14) DEF_STUB(15)
DEF_STUB(16) DEF_STUB(17) DEF_STUB(18) DEF_STUB(19)
DEF_STUB(20) DEF_STUB(21) DEF_STUB(22) DEF_STUB(23)
DEF_STUB(24) DEF_STUB(25) DEF_STUB(26) DEF_STUB(27)
DEF_STUB(28) DEF_STUB(29) DEF_STUB(30) DEF_STUB(31)
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
    // Переходим в системную папку обычной строкой
    fs_change_dir("sys"); 

    // Читаем файл в оригинальном регистре — наш FAT32 драйвер с LFN и strcasecmp легко его найдет
    int bytes = fs_read_file("logo.bmp", bmp_load_buffer, sizeof(bmp_load_buffer));

    fs_go_root(); // Возвращаемся в корень файловой системы

    clear_screen(0x000000); // Чёрный экран по умолчанию 

    // Если файл не найден или меньше минимального размера заголовка BMP (54 байта)
    if (bytes <= 54) {
        int text_x = (screen_width - (13 * 8)) / 2; 
        int text_y = (screen_height / 2) - 8; 
        kputs_at(text_x, text_y, "LOGO NOT FOUND", 0xFF5555); 
        flush_buffer(); 
        sleep_ms(3500); 
        clear_screen(0x000000); 
        flush_buffer(); 
        return;
    }

    // Проверяем сигнатуру 'BM' (0x4D42)
    uint16_t* signature = (uint16_t*)bmp_load_buffer;
    if (*signature != 0x4D42) {
        kputs_at(100, 100, "INVALID BMP FORMAT", 0xFF5555); 
        flush_buffer(); 
        sleep_ms(3500); 
        clear_screen(0x000000); 
        flush_buffer(); 
        return;
    }

    // Читаем параметры из заголовка BMP
    uint32_t data_offset = *(uint32_t*)(bmp_load_buffer + 10);
    int32_t  bmp_width   = *(int32_t*)(bmp_load_buffer + 18);
    int32_t  bmp_height  = *(int32_t*)(bmp_load_buffer + 22);
    uint16_t bpp         = *(uint16_t*)(bmp_load_buffer + 28);

    if (bpp != 24 && bpp != 32) {
        kputs_at(100, 100, "UNSUPPORTED BMP BPP", 0xFF5555); 
        flush_buffer(); 
        sleep_ms(3500); 
        clear_screen(0x000000); 
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
    clear_screen(0x000000); 
    flush_buffer(); 
}

void print_tty_banner(int tty_id) {
    kputs(OS_NAME " " OS_ARCH " " OS_VERSION " / tty", 0x55FF55);
    char num_buf[16];
    itoa(tty_id + 1, num_buf);
    kputs(num_buf, 0x55FFFF);
    kputs("\n\n", 0xFFFFFF);

    kputs("Защита от ДОЛБАЁБОВ!!! не вытаскивайте флешку с ОС а если и вытащили не втыкайте, не поможет.\n\n", 0x00AA00);
    fs_dir();
    kputs("\n", 0xFFFFFF);
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

    init_user_mode();

    pic_remap();                              // Переносим IRQ 0-7 на векторы 32-39
    pit_init(1000);                     // Запускаем PIT на частоту 1000 Гц
    
    uint8_t pic_mask = inb(0x21);
    outb(0x21, pic_mask & ~0x01);  // Разрешаем IRQ0 (таймер) на контроллере PIC

    __asm__ volatile ("sti");
    tsc_calibrate();

    //init_crash_guard_idt();

    init_mc_monitor();
    init_ps2_mouse();
    clear_screen(0x000000);

    kernel_system_bootstrap();

    render_boot_logo();

    input_len = 0;
    for (int i = 0; i < MAX_INPUT; i++) {
        input_buffer[i] = '\0';
    }

    tty_init_core();
    sched_init();

    task_create_kernel(tty1_task_entry, "tty1_shell")->tty_id = 0;
    kbd_layout = 0;

    uint8_t cursor_visible = 1;
    uint32_t last_blink = 0;

    while (1) {
        //keyboard_poll_handler();
        ps2_hw_service();   // клавиатура: единый читатель, раскладывает по очередям TTY + ловит Ctrl+C
        update_mouse_state();

        sleep_ms(10);

        if ((timer_ticks - last_blink) >= 500) {
            cursor_visible = !cursor_visible;
            tty_t* at = tty_get(tty_get_active_id());
            if (!(at && at->gfx_mode)) {   // если экраном владеет модуль - курсор консоли не трогаем
                draw_cursor(cursor_visible ? 0xFFFFFF : current_bg_color);
                flush_buffer();
            }
            last_blink = timer_ticks;
        }

        sched_yield();
    }
}