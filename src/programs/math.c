// math.c — Интерактивный граф-плоттер с правильным шрифтом Y и биндами F9-F12 для devOS (Ring 3).

#include <stdint.h>

#pragma GCC optimize ("no-tree-loop-distribute-patterns")

#define FB_W 320
#define FB_H 200

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                      : "memory");
    return ret;
}

static inline void sys_print(const char* str) {
    syscall3(1, (uint64_t)str, 0x00FFFFFF, 0);
}

static void run_plotter(void);

int main(void) {
    sys_print("[MATH.PRG] Plotter with F9-F12 bindings and fixed Y font started.\n");
    run_plotter();
    syscall3(0, 0, 0, 0);
    while (1) { }
    return 0;
}

static uint32_t* fb = 0;
static uint64_t  tsc_per_ms = 2000000;

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void sleep_ms(uint32_t ms) {
    uint64_t end = rdtsc64() + (uint64_t)ms * tsc_per_ms;
    while (rdtsc64() < end) __asm__ volatile ("pause");
}

static void present(void) { syscall3(21, (uint64_t)fb, 0, 0); }

static void clear_fb(uint32_t color) {
    for (int i = 0; i < FB_W * FB_H; i++) fb[i] = color;
}

static inline void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
    fb[y * FB_W + x] = color;
}

// Пиксельный шрифт 5x7 (с исправленной буквой Y и знаком =)
static const uint8_t font5x7[128][5] = {
    ['A'] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F'] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
    ['H'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I'] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J'] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K'] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L'] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N'] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P'] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q'] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R'] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x26, 0x49, 0x49, 0x49, 0x32},
    ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V'] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W'] = {0x3F, 0x40, 0x30, 0x40, 0x3F},
    ['X'] = {0x41, 0x22, 0x1C, 0x22, 0x41},
    ['Y'] = {0x03, 0x04, 0x78, 0x04, 0x03}, // Настоящая Y с аккуратной ножкой
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
    ['0'] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1'] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3'] = {0x21, 0x41, 0x45, 0x4B, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3E, 0x49, 0x49, 0x49, 0x30},
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9'] = {0x06, 0x49, 0x49, 0x49, 0x3E},
    ['+'] = {0x08, 0x08, 0x3E, 0x08, 0x08},
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['*'] = {0x2A, 0x1C, 0x7F, 0x1C, 0x2A},
    ['/'] = {0x40, 0x20, 0x10, 0x08, 0x04},
    ['('] = {0x00, 0x1C, 0x22, 0x41, 0x00},
    [')'] = {0x00, 0x41, 0x22, 0x1C, 0x00},
    ['='] = {0x14, 0x14, 0x14, 0x14, 0x14},
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
};

static void draw_char(int x, int y, char c, uint32_t color) {
    if (c >= 'a' && c <= 'z') c -= 32;
    const uint8_t* col = font5x7[(unsigned char)c];
    for (int dx = 0; dx < 5; dx++) {
        uint8_t line = col[dx];
        for (int dy = 0; dy < 7; dy++) {
            if (line & (1 << dy)) {
                put_pixel(x + dx, y + dy, color);
            }
        }
    }
}

static void draw_string(int x, int y, const char* str, uint32_t color) {
    while (*str) {
        draw_char(x, y, *str, color);
        x += 6;
        str++;
    }
}

// Расчет синуса и косинуса в Fixed-Point (SCALE = 1000)
#define SCALE 1000

static int math_sin(int angle_deg) {
    angle_deg = angle_deg % 360;
    if (angle_deg < 0) angle_deg += 360;
    int sign = 1;
    if (angle_deg >= 180) {
        sign = -1;
        angle_deg -= 180;
    }
    int numerator = 4 * angle_deg * (180 - angle_deg);
    int denominator = 40500 - angle_deg * (180 - angle_deg);
    if (denominator == 0) return 0;
    return ((numerator * SCALE) / denominator) * sign;
}

static int math_cos(int angle_deg) {
    return math_sin(angle_deg + 90);
}

// Парсер формул
static const char* global_formula = "SIN(X)";
static int parse_expr(const char** p, int x);

static int parse_factor(const char** p, int x) {
    while (**p == ' ') (*p)++;
    if (**p == 'x' || **p == 'X') {
        (*p)++;
        return x * SCALE;
    }
    if (**p == '(') {
        (*p)++;
        int val = parse_expr(p, x);
        if (**p == ')') (*p)++;
        return val;
    }
    if (((*p)[0] == 's' || (*p)[0] == 'S') && 
        ((*p)[1] == 'i' || (*p)[1] == 'I') && 
        ((*p)[2] == 'n' || (*p)[2] == 'N')) {
        *p += 3;
        while (**p == ' ') (*p)++;
        if (**p == '(') (*p)++;
        int val = parse_expr(p, x);
        if (**p == ')') (*p)++;
        return math_sin(val / SCALE);
    }
    if (((*p)[0] == 'c' || (*p)[0] == 'C') && 
        ((*p)[1] == 'o' || (*p)[1] == 'O') && 
        ((*p)[2] == 's' || (*p)[2] == 'S')) {
        *p += 3;
        while (**p == ' ') (*p)++;
        if (**p == '(') (*p)++;
        int val = parse_expr(p, x);
        if (**p == ')') (*p)++;
        return math_cos(val / SCALE);
    }
    int val = 0;
    int neg = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return (neg ? -val : val) * SCALE;
}

static int parse_term(const char** p, int x) {
    int val = parse_factor(p, x);
    while (1) {
        while (**p == ' ') (*p)++;
        if (**p == '*') {
            (*p)++;
            val = (val * parse_factor(p, x)) / SCALE;
        } else if (**p == '/') {
            (*p)++;
            int div = parse_factor(p, x);
            if (div != 0) val = (val * SCALE) / div;
        } else {
            break;
        }
    }
    return val;
}

static int parse_expr(const char** p, int x) {
    int val = parse_term(p, x);
    while (1) {
        while (**p == ' ') (*p)++;
        if (**p == '+') { (*p)++; val += parse_term(p, x); }
        else if (**p == '-') { (*p)++; val -= parse_term(p, x); }
        else { break; }
    }
    return val;
}

static int evaluate_formula(const char* formula, int x_val) {
    const char* p = formula;
    while (*p == ' ') p++;
    if (*p == 'y' || *p == 'Y') {
        const char* temp = p + 1;
        while (*temp == ' ') temp++;
        if (*temp == '=') {
            p = temp + 1;
        }
    }
    return parse_expr(&p, x_val);
}

#define PLOT_X_START  30
#define PLOT_X_END    290
#define PLOT_Y_CENTER 95
#define PLOT_WIDTH    (PLOT_X_END - PLOT_X_START)

static int calculate_y(int x, const char* formula) {
    if (x < PLOT_X_START || x > PLOT_X_END) return PLOT_Y_CENTER;
    int local_x = x - PLOT_X_START;
    int math_x = (local_x * 360) / PLOT_WIDTH - 180;
    
    int result_fixed = evaluate_formula(formula, math_x);
    int y_offset = result_fixed / 100;
    return PLOT_Y_CENTER - y_offset;
}

static void run_plotter(void) {
    uint64_t heap = syscall3(14, 0, 0, 0);
    if (heap == (uint64_t)-1) return;
    if (syscall3(14, heap + FB_W * FB_H * 4, 0, 0) == (uint64_t)-1) return;
    fb = (uint32_t*)heap;

    uint32_t k = (uint32_t)syscall3(22, 0, 0, 0);
    if (k) tsc_per_ms = k;

    while ((uint8_t)syscall3(6, 0, 0, 0) != 0) { }

    char formula_buf[64] = "Y = 5 * SIN(X)";
    int buf_len = 14;
    int mode = 0; // 0 = ввод, 1 = график
    int t = 0;

    while (1) {
        uint8_t sc;
        int esc_pressed = 0;

        while ((sc = (uint8_t)syscall3(6, 0, 0, 0)) != 0) {
            if (sc == 0x01) { // ESC
                if (mode == 1) { mode = 0; t = 0; }
                else { esc_pressed = 1; break; }
            }
            if (sc == 0x3F) { // F5 (старт)
                if (mode == 0) {
                    global_formula = formula_buf;
                    mode = 1;
                    t = 0;
                }
            }

            if (mode == 0) {
                if (sc == 0x0E && buf_len > 0) { // Backspace
                    buf_len--;
                    formula_buf[buf_len] = '\0';
                } else if (buf_len < 63) {
                    char c = 0;

                    if (sc >= 0x02 && sc <= 0x0A) c = '1' + (sc - 0x02);
                    if (sc == 0x0B) c = '0';

                    if (sc == 0x1E) c = 'A';
                    if (sc == 0x30) c = 'B';
                    if (sc == 0x2E) c = 'C';
                    if (sc == 0x20) c = 'D';
                    if (sc == 0x12) c = 'E';
                    if (sc == 0x21) c = 'F';
                    if (sc == 0x22) c = 'G';
                    if (sc == 0x23) c = 'H';
                    if (sc == 0x17) c = 'I';
                    if (sc == 0x24) c = 'J';
                    if (sc == 0x25) c = 'K';
                    if (sc == 0x26) c = 'L';
                    if (sc == 0x32) c = 'M';
                    if (sc == 0x31) c = 'N';
                    if (sc == 0x18) c = 'O';
                    if (sc == 0x19) c = 'P';
                    if (sc == 0x10) c = 'Q';
                    if (sc == 0x13) c = 'R';
                    if (sc == 0x1F) c = 'S';
                    if (sc == 0x14) c = 'T';
                    if (sc == 0x16) c = 'U';
                    if (sc == 0x2F) c = 'V';
                    if (sc == 0x11) c = 'W';
                    if (sc == 0x2D) c = 'X';
                    if (sc == 0x15) c = 'Y';
                    if (sc == 0x2C) c = 'Z';

                    if (sc == 0x0C) c = '-';
                    if (sc == 0x0D || sc == 0x27) c = '+';
                    if (sc == 0x35) c = '/';
                    if (sc == 0x39) c = ' ';

                    // Спецклавиши
                    if (sc == 0x43) c = '('; // F9 -> '('
                    if (sc == 0x44) c = ')'; // F10 -> ')'
                    if (sc == 0x57) c = '*'; // F11 -> '*'
                    if (sc == 0x58) c = '='; // F12 -> '='

                    if (c != 0) {
                        formula_buf[buf_len++] = c;
                        formula_buf[buf_len] = '\0';
                    }
                }
            }
        }
        if (esc_pressed) break;

        clear_fb(0x0F111A);

        if (mode == 0) {
            draw_string(40, 35, "ENTER FORMULA:", 0x00E5FF);
            
            for (int x = 38; x <= 282; x++) {
                put_pixel(x, 55, 0x3E445B);
                put_pixel(x, 77, 0x3E445B);
            }
            for (int y = 55; y <= 77; y++) {
                put_pixel(38, y, 0x3E445B);
                put_pixel(282, y, 0x3E445B);
            }
            draw_string(46, 63, formula_buf, 0xFFFFFF);

            // Подсказка по функциональным клавишам
            draw_string(40, 95, "( F9  ) F10  * F11  = F12", 0x5C6370);
            draw_string(40, 115, "PRESS F5 TO PLOT", 0xF6DC1A);
            draw_string(40, 135, "PRESS ESC TO EXIT", 0x5C6370);
            present();
            sleep_ms(50);
        } else {
            for (int x = PLOT_X_START; x <= PLOT_X_END; x += 32) {
                for (int y = 20; y < 170; y += 20) {
                    put_pixel(x, y, 0x222638);
                }
            }

            for (int x = PLOT_X_START - 10; x <= PLOT_X_END + 15; x++) put_pixel(x, PLOT_Y_CENTER, 0x5C6370);
            int y_axis_x = 160;
            for (int y = 15; y < 175; y++) put_pixel(y_axis_x, y, 0x5C6370);

            int current_draw_x = PLOT_X_START + t;
            if (current_draw_x > PLOT_X_END) current_draw_x = PLOT_X_END;

            for (int x = PLOT_X_START + 1; x <= current_draw_x; x++) {
                int y_prev = calculate_y(x - 1, global_formula);
                int y_curr = calculate_y(x, global_formula);
                int y_start = y_prev < y_curr ? y_prev : y_curr;
                int y_end   = y_prev > y_curr ? y_prev : y_curr;

                for (int y = y_start; y <= y_end; y++) {
                    put_pixel(x, y, 0x00E5FF);
                }
            }

            if (PLOT_X_START + t <= PLOT_X_END) {
                int cursor_x = PLOT_X_START + t;
                int cursor_y = calculate_y(cursor_x, global_formula);

                for (int dy = -2; dy <= 2; dy++) {
                    for (int dx = -2; dx <= 2; dx++) {
                        if (dx*dx + dy*dy <= 4) put_pixel(cursor_x + dx, cursor_y + dy, 0xFFFFFF);
                        else if (dx*dx + dy*dy <= 8) put_pixel(cursor_x + dx, cursor_y + dy, 0xFF3366);
                    }
                }

                present();
                sleep_ms(20);
                t++;
            } else {
                present();
                sleep_ms(2000);
                t = 0;
            }
        }
    }
}