#include <stdint.h>
#include <stddef.h>

static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(0), "b"(code) : "memory");
    while (1);
}

static inline int sys_open(const char* fn, int flags) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(10), "b"(fn), "c"(flags) : "memory");
    return ret;
}

static inline int sys_ioctl(int fd, uint64_t req, void* arg) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(15), "b"(fd), "c"(req), "d"(arg) : "memory");
    return ret;
}

static inline int sys_write(int fd, const void* buf, uint32_t sz) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(13), "b"(fd), "c"(buf), "d"(sz) : "memory");
    return ret;
}

static inline uint8_t sys_get_key(void) {
    uint64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(6) : "memory");
    return (uint8_t)ret;
}

static inline void sys_sleep(uint32_t ms) {
    __asm__ volatile ("int $0x80" : : "a"(4), "b"(ms) : "memory");
}

static void* sys_sbrk(int64_t inc) {
    uint64_t cur = 0;
    __asm__ volatile ("int $0x80" : "=a"(cur) : "a"(14), "b"(0) : "memory");
    if (inc == 0) return (void*)cur;
    uint64_t res = 0;
    __asm__ volatile ("int $0x80" : "=a"(res) : "a"(14), "b"(cur + inc) : "memory");
    return (void*)cur;
}

typedef struct {
    int x;
    int y;
    uint8_t buttons;
    int last_dx;
    int last_dy;
} __attribute__((packed)) user_mouse_t;

static inline int sys_get_mouse(user_mouse_t* ms) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(7), "b"(ms) : "memory");
    return ret;
}

typedef struct {
    uint32_t width, height, pitch, bpp;
    uint64_t paddr;
} fb_var_info_t;

static const uint8_t font8x8[128][8] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['+'] = {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    ['-'] = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    ['/'] = {0x02,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    [':'] = {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    ['['] = {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    [']'] = {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    ['0'] = {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},
    ['1'] = {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    ['2'] = {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},
    ['3'] = {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    ['4'] = {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},
    ['5'] = {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    ['6'] = {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00},
    ['7'] = {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},
    ['8'] = {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
    ['9'] = {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    ['A'] = {0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00},
    ['B'] = {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
    ['C'] = {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    ['D'] = {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
    ['E'] = {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
    ['F'] = {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00},
    ['I'] = {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['K'] = {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    ['L'] = {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},
    ['M'] = {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    ['N'] = {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},
    ['O'] = {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    ['P'] = {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},
    ['R'] = {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00},
    ['S'] = {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    ['T'] = {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    ['U'] = {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    ['V'] = {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},
    ['W'] = {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    ['X'] = {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},
    ['Y'] = {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}
};

static uint32_t* g_fb = NULL;
static uint32_t  g_sw = 0;
static uint32_t  g_sh = 0;

static void put_pixel(int x, int y, uint32_t c) {
    if (x >= 0 && x < (int)g_sw && y >= 0 && y < (int)g_sh) {
        g_fb[y * g_sw + x] = c;
    }
}

static void draw_fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++)
            put_pixel(x + j, y + i, c);
}

static void draw_char(int x, int y, char ch, uint32_t color) {
    if ((uint8_t)ch > 127) return;
    if (ch >= 'a' && ch <= 'z') ch -= ('a' - 'A');
    const uint8_t* glyph = font8x8[(uint8_t)ch];
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (glyph[r] & (1 << (7 - c))) {
                put_pixel(x + c, y + r, color);
            }
        }
    }
}

static void draw_string(int x, int y, const char* str, uint32_t color) {
    while (*str) {
        draw_char(x, y, *str, color);
        x += 8;
        str++;
    }
}

static void draw_dec(int x, int y, int val, uint32_t color) {
    char buf[12];
    int i = 11;
    buf[i--] = '\0';
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) buf[i--] = '0';
    while (val > 0 && i >= 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    if (neg && i >= 0) buf[i--] = '-';
    draw_string(x, y, &buf[i + 1], color);
}

int main(void) {
    int fb_fd = sys_open("/dev/fb0", 0);
    if (fb_fd < 0) sys_exit(1);

    fb_var_info_t info;
    sys_ioctl(fb_fd, 0x4600, &info);
    g_sw = info.width;
    g_sh = info.height;

    uint32_t total_bytes = g_sw * g_sh * 4;
    g_fb = (uint32_t*)sys_sbrk((int64_t)total_bytes);

    draw_fill_rect(0, 0, g_sw, g_sh, 0x10141A);

    int canvas_x = 340, canvas_y = 60, canvas_w = g_sw - 360, canvas_h = g_sh - 80;
    draw_fill_rect(canvas_x, canvas_y, canvas_w, canvas_h, 0x05070A);

    int prev_x = g_sw / 2;
    int prev_y = g_sh / 2;

    while (1) {
        uint8_t key = sys_get_key();
        if (key == 0x01) break; // ESC
        if (key == 0x2E) {      // 'C'
            draw_fill_rect(canvas_x, canvas_y, canvas_w, canvas_h, 0x05070A);
        }

        user_mouse_t ms;
        sys_get_mouse(&ms);

        // Панель статуса слева
        draw_fill_rect(10, 10, 320, g_sh - 20, 0x1A212B);

        draw_string(20, 24, "[ DEVOS MOUSE MONITOR ]", 0x3DAEE9);

        draw_string(20, 60, "POS X : ", 0xAAAAAA);
        draw_dec(100, 60, ms.x, 0x00FF88);

        draw_string(20, 80, "POS Y : ", 0xAAAAAA);
        draw_dec(100, 80, ms.y, 0x00FF88);

        draw_string(20, 110, "LAST DX : ", 0xAAAAAA);
        draw_dec(100, 110, ms.last_dx, (ms.last_dx != 0) ? 0x00FF88 : 0xAAAAAA);

        draw_string(20, 130, "LAST DY : ", 0xAAAAAA);
        draw_dec(100, 130, ms.last_dy, (ms.last_dy != 0) ? 0x00FF88 : 0xAAAAAA);

        draw_string(20, 170, "LMB : ", 0xAAAAAA);
        draw_string(70, 170, (ms.buttons & 1) ? "[DOWN]" : "[ UP ]", (ms.buttons & 1) ? 0x00FF88 : 0x666666);

        draw_string(20, 190, "RMB : ", 0xAAAAAA);
        draw_string(70, 190, (ms.buttons & 2) ? "[DOWN]" : "[ UP ]", (ms.buttons & 2) ? 0x00FF88 : 0x666666);

        draw_string(20, 240, "C   - CLEAR CANVAS", 0xAAAAAA);
        draw_string(20, 260, "ESC - EXIT TO SHELL", 0xAAAAAA);

        // Холст: если ЛКМ зажата — рисуем линию/точку
        if ((ms.buttons & 1) && ms.x >= canvas_x && ms.x < canvas_x + canvas_w &&
            ms.y >= canvas_y && ms.y < canvas_y + canvas_h) {
            draw_fill_rect(ms.x - 2, ms.y - 2, 5, 5, 0x3DAEE9);
        }

        // Стираем старый прицел на холсте и рисуем новый
        draw_fill_rect(prev_x - 4, prev_y, 9, 1, 0x05070A);
        draw_fill_rect(prev_x, prev_y - 4, 1, 9, 0x05070A);

        draw_fill_rect(ms.x - 4, ms.y, 9, 1, 0xFFFFFF);
        draw_fill_rect(ms.x, ms.y - 4, 1, 9, 0xFFFFFF);

        prev_x = ms.x;
        prev_y = ms.y;

        sys_write(fb_fd, g_fb, total_bytes);
        sys_sleep(16);
    }

    return 0;
}