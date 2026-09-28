// psx.prg — заставка PS1 как обычная программа Ring 3 (замена psx_module.c / psx.sys).
// Рисует кадр 320x200 в свой буфер и отдаёт его ядру через sys_blit_frame (сисколл 21).
// Ядро само масштабирует кадр, рисует его в буфер ТВОЕГО TTY и не рисует, пока этот TTY скрыт.
#include <stdint.h>

// Не даём GCC превращать циклы заливки в вызовы memset (libc в программе нет)
#pragma GCC optimize ("no-tree-loop-distribute-patterns")

#define FB_W 320
#define FB_H 200

#define COLOR_ORANGE 0xE23A23
#define COLOR_YELLOW 0xF6DC1A
#define COLOR_RED    0xD6221C

#define TRIANGLE_MARGIN 14
#define ANIM_STEPS      25
#define FRAME_DELAY     15
#define TRIANGLE_ALPHA  180

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                      : "memory");
    return ret;
}

static void run_psx(void);

// ТОЧКА ВХОДА ДОЛЖНА БЫТЬ ПЕРВОЙ В ФАЙЛЕ (как в test.c)
int main(void) {
    run_psx();
    syscall3(0, 0, 0, 0);   // exit(0)
    while (1) { }
    return 0;
}

// ---------------------------------------------------------------- система

static uint32_t* fb = 0;                 // буфер кадра 320x200 (выделяем через sys_brk)
static uint64_t  tsc_per_ms = 2000000;

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Ждём в Ring 3, а не в сисколле: таймер вытесняет нас, другие TTY не подвисают
static void sleep_ms(uint32_t ms) {
    uint64_t end = rdtsc64() + (uint64_t)ms * tsc_per_ms;
    while (rdtsc64() < end) __asm__ volatile ("pause");
}

// Ввод: сканкоды Set 1. Возвращает 1, если нажат ESC (make-код 0x01)
static int check_esc(void) {
    uint8_t sc;
    while ((sc = (uint8_t)syscall3(6, 0, 0, 0)) != 0) {
        if (sc == 0x01) return 1;
    }
    return 0;
}

static void present(void) { syscall3(21, (uint64_t)fb, 0, 0); }

static void clear_fb(uint32_t color) {
    for (int i = 0; i < FB_W * FB_H; i++) fb[i] = color;
}

static inline void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
    fb[y * FB_W + x] = color;
}

// ---------------------------------------------------------------- графика (как в psx_module.c)

static inline uint8_t lerp_u8(uint8_t a, uint8_t b, int t, int max_t) {
    if (max_t <= 0) return a;
    return a + ((b - a) * t) / max_t;
}

static inline uint32_t lerp_color(uint32_t c1, uint32_t c2, int t, int max_t) {
    if (max_t <= 0) return c1;
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    return (lerp_u8(r1, r2, t, max_t) << 16) |
           (lerp_u8(g1, g2, t, max_t) << 8)  |
            lerp_u8(b1, b2, t, max_t);
}

static inline uint32_t blend_colors(uint32_t bg, uint32_t fg, uint8_t alpha) {
    uint8_t r_bg = (bg >> 16) & 0xFF, g_bg = (bg >> 8) & 0xFF, b_bg = bg & 0xFF;
    uint8_t r_fg = (fg >> 16) & 0xFF, g_fg = (fg >> 8) & 0xFF, b_fg = fg & 0xFF;
    uint8_t r = (r_fg * alpha + r_bg * (255 - alpha)) / 255;
    uint8_t g = (g_fg * alpha + g_bg * (255 - alpha)) / 255;
    uint8_t b = (b_fg * alpha + b_bg * (255 - alpha)) / 255;
    return (r << 16) | (g << 8) | b;
}

static uint32_t get_ps1_gradient_color_horizontal(int local_x, int total_width) {
    int half = total_width / 2;
    if (half <= 0) return COLOR_ORANGE;
    uint8_t r, g, b;
    if (local_x < half) {
        r = lerp_u8(0xE2, 0xF6, local_x, half);
        g = lerp_u8(0x3A, 0xDC, local_x, half);
        b = lerp_u8(0x23, 0x1A, local_x, half);
    } else {
        r = lerp_u8(0xF6, 0xE2, local_x - half, half);
        g = lerp_u8(0xDC, 0x3A, local_x - half, half);
        b = lerp_u8(0x1A, 0x23, local_x - half, half);
    }
    return (r << 16) | (g << 8) | b;
}

static int get_triangle_color(int x, int y,
                              int x1, int y1, int x2, int y2, int x3, int y3,
                              uint32_t base_color, uint32_t apex_color,
                              uint32_t* out_color) {
    int det = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3);
    if (det == 0) return 0;

    int w1_num = (y2 - y3) * (x - x3) + (x3 - x2) * (y - y3);
    int w2_num = (y3 - y1) * (x - x3) + (x1 - x3) * (y - y3);

    int inside;
    if (det > 0) inside = (w1_num >= 0 && w2_num >= 0 && (w1_num + w2_num) <= det);
    else         inside = (w1_num <= 0 && w2_num <= 0 && (w1_num + w2_num) >= det);

    if (inside) {
        int t = (det - w1_num - w2_num) * 255 / det;
        if (t < 0) t = -t;
        *out_color = lerp_color(base_color, apex_color, t, 255);
        return 1;
    }
    return 0;
}

static void draw_base_diamond(int cx, int cy, int size) {
    int half = size / 2;
    for (int dy = -half; dy <= half; dy++) {
        int y = cy + dy;
        int abs_dy = (dy < 0) ? -dy : dy;
        int current_half_w = half * (half - abs_dy) / half;
        for (int x = cx - current_half_w; x <= cx + current_half_w; x++) {
            int local_x = x - (cx - half);
            put_pixel(x, y, get_ps1_gradient_color_horizontal(local_x, size));
        }
    }
}

static void draw_moving_triangle_blended(int x1, int y1, int x2, int y2, int x3, int y3,
                                         uint32_t base_color, uint32_t apex_color,
                                         int cx, int size, uint8_t current_alpha) {
    int min_x = x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3);
    int max_x = x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3);
    int min_y = y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3);
    int max_y = y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3);

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            uint32_t tri_color;
            if (get_triangle_color(x, y, x1, y1, x2, y2, x3, y3, base_color, apex_color, &tri_color)) {
                int local_x = x - (cx - size / 2);
                uint32_t bg_color = get_ps1_gradient_color_horizontal(local_x, size);
                put_pixel(x, y, blend_colors(bg_color, tri_color, current_alpha));
            }
        }
    }
}

// ---------------------------------------------------------------- сама анимация

static void run_psx(void) {
    // Кадровый буфер берём из кучи процесса (sys_brk), чтобы не зависеть от .bss
    uint64_t heap = syscall3(14, 0, 0, 0);
    if (heap == (uint64_t)-1) return;
    if (syscall3(14, heap + FB_W * FB_H * 4, 0, 0) == (uint64_t)-1) return;
    fb = (uint32_t*)heap;

    // Частоту TSC измерило ядро при загрузке
    uint32_t k = (uint32_t)syscall3(22, 0, 0, 0);
    if (k) tsc_per_ms = k;

    // Сбрасываем накопленные нажатия (Enter от запуска команды)
    while ((uint8_t)syscall3(6, 0, 0, 0) != 0) { }

    int cx = FB_W / 2;
    int cy = FB_H / 2 - 8;
    int size = 100;
    int half = size / 2;
    int inner_half = half - TRIANGLE_MARGIN;
    if (inner_half < 10) inner_half = 10;

    // 1. Плавное появление серого фона
    for (int factor = 0; factor <= 255; factor += 10) {
        if (check_esc()) return;
        clear_fb((factor << 16) | (factor << 8) | factor);
        present();
        sleep_ms(10);
    }

    int tl_x1_end = cx,                  tl_y1_end = cy - inner_half;
    int tl_x2_end = cx,                  tl_y2_end = cy;
    int tl_x3_end = cx - inner_half / 2, tl_y3_end = cy - inner_half / 2;

    int br_x1_end = cx,                  br_y1_end = cy;
    int br_x2_end = cx,                  br_y2_end = cy + inner_half;
    int br_x3_end = cx + inner_half / 2, br_y3_end = cy + inner_half / 2;

    int dx_tl = (cx - half) - tl_x3_end;
    int dy_tl = cy - tl_y3_end;
    int dx_br = (cx + half) - br_x3_end;
    int dy_br = cy - br_y3_end;

    // 2. Анимация движения треугольников
    for (int step = ANIM_STEPS; step >= 0; step--) {
        if (check_esc()) return;

        clear_fb(0xFFFFFF);
        draw_base_diamond(cx, cy, size);

        int off_tl_x = (dx_tl * step) / ANIM_STEPS;
        int off_tl_y = (dy_tl * step) / ANIM_STEPS;
        int off_br_x = (dx_br * step) / ANIM_STEPS;
        int off_br_y = (dy_br * step) / ANIM_STEPS;

        uint8_t alpha = (TRIANGLE_ALPHA * (ANIM_STEPS - step)) / ANIM_STEPS;

        draw_moving_triangle_blended(tl_x1_end + off_tl_x, tl_y1_end + off_tl_y,
                                     tl_x2_end + off_tl_x, tl_y2_end + off_tl_y,
                                     tl_x3_end + off_tl_x, tl_y3_end + off_tl_y,
                                     COLOR_YELLOW, COLOR_RED, cx, size, alpha);
        draw_moving_triangle_blended(br_x1_end + off_br_x, br_y1_end + off_br_y,
                                     br_x2_end + off_br_x, br_y2_end + off_br_y,
                                     br_x3_end + off_br_x, br_y3_end + off_br_y,
                                     COLOR_YELLOW, COLOR_RED, cx, size, alpha);
        present();
        sleep_ms(FRAME_DELAY);
    }

    // 3. Финальный кадр, ждём ESC (перерисовываем, чтобы кадр не пропадал при смене TTY)
    clear_fb(0xFFFFFF);
    draw_base_diamond(cx, cy, size);
    draw_moving_triangle_blended(tl_x1_end, tl_y1_end, tl_x2_end, tl_y2_end, tl_x3_end, tl_y3_end,
                                 COLOR_YELLOW, COLOR_RED, cx, size, TRIANGLE_ALPHA);
    draw_moving_triangle_blended(br_x1_end, br_y1_end, br_x2_end, br_y2_end, br_x3_end, br_y3_end,
                                 COLOR_YELLOW, COLOR_RED, cx, size, TRIANGLE_ALPHA);
    while (!check_esc()) {
        present();
        sleep_ms(50);
    }
}