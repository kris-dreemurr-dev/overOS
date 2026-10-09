#include <stdint.h>
#include <stddef.h>
#include "icons.h"
#include "font_8x8.h" // Файл со шрифтом 8x16 (массив font_ru_8x16)

// -----------------------------------------------------------------------------
// Системные вызовы ядра devOS
// -----------------------------------------------------------------------------
static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(0), "b"(code) : "memory");
    while (1);
}

static inline int sys_open(const char* fn, int flags) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(10), "b"(fn), "c"(flags) : "memory");
    return ret;
}

static inline int sys_read(int fd, void* buf, int sz) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(11), "b"(fd), "c"(buf), "d"(sz) : "memory");
    return ret;
}

static inline void sys_close(int fd) {
    __asm__ volatile ("int $0x80" : : "a"(12), "b"(fd) : "memory");
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

static inline int sys_ioctl(int fd, uint64_t req, void* arg) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(15), "b"(fd), "c"(req), "d"(arg) : "memory");
    return ret;
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
    int x, y;
    uint8_t buttons;
    int last_dx, last_dy;
} __attribute__((packed)) user_mouse_t;

static inline int sys_get_mouse(user_mouse_t* ms) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(7), "b"(ms) : "memory");
    return ret;
}

typedef struct {
    uint8_t sec, min, hour;
    uint8_t day, month, year;
} __attribute__((packed)) rtc_time_t;

static inline int sys_get_time(rtc_time_t* t) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(8), "b"(t) : "memory");
    return ret;
}

typedef struct {
    uint32_t width, height, pitch, bpp;
    uint64_t paddr;
} fb_var_info_t;

// -----------------------------------------------------------------------------
// Цвета KDE Plasma (Breeze Dark)
// -----------------------------------------------------------------------------
#define COLOR_DESKTOP_BG    0x1B1E20
#define COLOR_PANEL_BG      0x232629
#define COLOR_PANEL_BORDER  0x31363B
#define COLOR_DOCK_ITEM_BG  0x2A2E32
#define COLOR_DOCK_HOVER    0x353B42
#define COLOR_WIN_BG        0x232629
#define COLOR_WIN_TITLE_ACT 0x3DAEE9
#define COLOR_WIN_TITLE_IN  0x31363B
#define COLOR_WIN_BORDER    0x141618
#define COLOR_CLOSE_BTN     0xDA4453
#define COLOR_TEXT_MAIN     0xFCFCFC
#define COLOR_TEXT_MUTED    0x8F98A0
#define COLOR_ACCENT_BLUE   0x3DAEE9
#define COLOR_WARN_RED      0xED1515

#define TITLE_BAR_HEIGHT 26
#define TASKBAR_HEIGHT   44

typedef struct {
    int id;
    int x, y, w, h;
    const char* title;
    int is_dragging;
    int drag_off_x, drag_off_y;
    int is_minimized;
} window_t;

enum PopupType {
    POPUP_NONE = 0,
    POPUP_SOUND,
    POPUP_NET,
    POPUP_BRIGHTNESS,
    POPUP_CLOCK
};

static uint32_t* g_backbuffer = NULL;
static uint32_t* g_outbuffer  = NULL;
static uint32_t  g_screen_w = 0, g_screen_h = 0;
static int       g_fb_fd = -1;
static uint32_t  g_buf_bytes = 0;

static int g_brightness = 100;
static int g_brightness_dragging = 0;
static int g_layout_ru = 1;
static int g_tz_offset = 3; // 0: UTC, 3: MSK, 9: YAKT
static int g_active_popup = POPUP_NONE;
static uint32_t g_popup_timer = 0;
static int g_menu_open = 0;

static int s_alt_down = 0;
static int s_shift_down = 0;

// -----------------------------------------------------------------------------
// Конфигурационный файл (/programs/desktop.cfg)
// -----------------------------------------------------------------------------
static void load_config(void) {
    int fd = sys_open("/programs/desktop.cfg", 0);
    if (fd < 0) return;

    char buf[128];
    int rd = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (rd <= 0) return;
    buf[rd] = '\0';

    for (int i = 0; i < rd; i++) {
        if (buf[i] == 't' && buf[i+1] == 'z' && buf[i+2] == '=') {
            g_tz_offset = buf[i+3] - '0';
        }
        if (buf[i] == 'r' && buf[i+1] == 'u' && buf[i+2] == '=') {
            g_layout_ru = buf[i+3] - '0';
        }
    }
}

static void save_config(void) {
    int fd = sys_open("/programs/desktop.cfg", 1);
    if (fd < 0) return;

    char buf[32];
    buf[0] = 't'; buf[1] = 'z'; buf[2] = '='; buf[3] = '0' + (g_tz_offset % 10); buf[4] = '\n';
    buf[5] = 'r'; buf[6] = 'u'; buf[7] = '='; buf[8] = '0' + (g_layout_ru ? 1 : 0); buf[9] = '\n';
    buf[10] = '\0';

    sys_write(fd, buf, 10);
    sys_close(fd);
}

// -----------------------------------------------------------------------------
// Отрисовка текста 8x16 с поддержкой CP866
// -----------------------------------------------------------------------------
static void draw_rect(int rx, int ry, int rw, int rh, uint32_t color) {
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { ry += ry; ry = 0; }
    if (rx + rw > (int)g_screen_w) rw = g_screen_w - rx;
    if (ry + rh > (int)g_screen_h) rh = g_screen_h - ry;
    if (rw <= 0 || rh <= 0) return;

    for (int y = 0; y < rh; y++) {
        uint32_t* row = &g_backbuffer[(ry + y) * g_screen_w + rx];
        for (int x = 0; x < rw; x++) row[x] = color;
    }
}

// Символ 8x16 (16 строк)
static void draw_char(int x, int y, uint8_t ch, uint32_t color) {
    const uint8_t* glyph = &font_ru_8x16[(uint32_t)ch * 16];

    for (int r = 0; r < 16; r++) {
        uint8_t row = glyph[r];
        for (int c = 0; c < 8; c++) {
            if (row & (1 << (7 - c))) {
                int px = x + c;
                int py = y + r;
                if (px >= 0 && px < (int)g_screen_w && py >= 0 && py < (int)g_screen_h) {
                    g_backbuffer[py * g_screen_w + px] = color;
                }
            }
        }
    }
}

// Декодер UTF-8 -> CP866
static void draw_string(int x, int y, const char* str, uint32_t color) {
    const uint8_t* s = (const uint8_t*)str;
    while (*s) {
        if (*s == 0xD0 && *(s + 1)) {
            uint8_t next = *(s + 1);
            if (next >= 0x90 && next <= 0xBF) {
                draw_char(x, y, (uint8_t)(next - 0x90 + 0x80), color); // А..п
            } else if (next == 0x81) {
                draw_char(x, y, 0xF0, color); // Ё
            }
            s += 2;
            x += 8;
        } else if (*s == 0xD1 && *(s + 1)) {
            uint8_t next = *(s + 1);
            if (next >= 0x80 && next <= 0x8F) {
                draw_char(x, y, (uint8_t)(next - 0x80 + 0xE0), color); // р..я
            } else if (next == 0x91) {
                draw_char(x, y, 0xF1, color); // ё
            }
            s += 2;
            x += 8;
        } else {
            draw_char(x, y, *s, color);
            s++;
            x += 8;
        }
    }
}

static void draw_dec(int x, int y, int val, uint32_t color) {
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    if (val == 0) buf[i--] = '0';
    while (val > 0 && i >= 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    draw_string(x, y, &buf[i + 1], color);
}

static void draw_dec2(int x, int y, int val, uint32_t color) {
    draw_char(x, y, (uint8_t)('0' + ((val / 10) % 10)), color);
    draw_char(x + 8, y, (uint8_t)('0' + (val % 10)), color);
}

static void draw_mono_icon(int x, int y, const uint16_t* mask, uint32_t fg, uint32_t alt_col, int has_alt) {
    for (int r = 0; r < 16; r++) {
        for (int c = 0; c < 16; c++) {
            if (mask[r] & (1 << (15 - c))) {
                uint32_t col = (has_alt && c >= 7) ? alt_col : fg;
                int px = x + c, py = y + r;
                if (px >= 0 && px < (int)g_screen_w && py >= 0 && py < (int)g_screen_h) {
                    g_backbuffer[py * g_screen_w + px] = col;
                }
            }
        }
    }
}

static void draw_dolphin_folder(int x, int y) {
    draw_rect(x + 2, y + 3, 6, 3, 0x1D99F3);
    draw_rect(x + 2, y + 5, 14, 9, 0x3DAEE9);
    draw_rect(x + 3, y + 7, 12, 6, 0x56BEF5);
}

static void draw_cursor(int mx, int my) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 11; x++) {
            if (cursor_mask[y] & (1 << (15 - x))) {
                int px = mx + x, py = my + y;
                if (px >= 0 && px < (int)g_screen_w && py >= 0 && py < (int)g_screen_h) {
                    uint32_t col = (x == 0 || y == 0 || (cursor_mask[y] & (1 << (14 - x))) == 0) ? 0x000000 : 0xFFFFFF;
                    g_backbuffer[py * g_screen_w + px] = col;
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Окно Dolphin
// -----------------------------------------------------------------------------
static void render_window(window_t* win, int is_active) {
    if (win->is_minimized) return;

    draw_rect(win->x - 1, win->y - 1, win->w + 2, win->h + TITLE_BAR_HEIGHT + 2, COLOR_WIN_BORDER);
    uint32_t t_color = is_active ? COLOR_WIN_TITLE_ACT : COLOR_WIN_TITLE_IN;
    draw_rect(win->x, win->y, win->w, TITLE_BAR_HEIGHT, t_color);
    
    // Заголовок центрирован по высоте 26 px
    draw_string(win->x + 10, win->y + 5, win->title, COLOR_TEXT_MAIN);

    // Крестик [X]
    draw_rect(win->x + win->w - 22, win->y + 5, 16, 16, COLOR_CLOSE_BTN);
    draw_string(win->x + win->w - 18, win->y + 5, "X", COLOR_TEXT_MAIN);

    draw_rect(win->x, win->y + TITLE_BAR_HEIGHT, win->w, win->h, COLOR_WIN_BG);

    // Список файлов с шагом 24 px
    draw_string(win->x + 20, win->y + TITLE_BAR_HEIGHT + 14, "Путь: /programs", COLOR_ACCENT_BLUE);
    draw_rect(win->x + 20, win->y + TITLE_BAR_HEIGHT + 34, win->w - 40, 1, COLOR_PANEL_BORDER);
    draw_string(win->x + 20, win->y + TITLE_BAR_HEIGHT + 44, "[DIR]  ..", COLOR_TEXT_MAIN);
    draw_string(win->x + 20, win->y + TITLE_BAR_HEIGHT + 68, "[PRG]  DOOM.PRG          471 KB", COLOR_TEXT_MAIN);
    draw_string(win->x + 20, win->y + TITLE_BAR_HEIGHT + 92, "[PRG]  QUAKE.PRG        1250 KB", COLOR_TEXT_MAIN);
    draw_string(win->x + 20, win->y + TITLE_BAR_HEIGHT + 116,"[PRG]  FASTFETCH.PRG      48 KB", COLOR_TEXT_MAIN);
}

// -----------------------------------------------------------------------------
// Панель задач KDE Plasma
// -----------------------------------------------------------------------------
static void render_plasma_taskbar(window_t* win, int mouse_x, int mouse_y, const rtc_time_t* t) {
    int bar_y = g_screen_h - TASKBAR_HEIGHT;

    draw_rect(0, bar_y, g_screen_w, 1, COLOR_PANEL_BORDER);
    draw_rect(0, bar_y + 1, g_screen_w, TASKBAR_HEIGHT - 1, COLOR_PANEL_BG);

    // 1. Кнопка [overOS]
    int app_btn_w = 84;
    int app_hover = (mouse_x >= 6 && mouse_x <= 6 + app_btn_w && mouse_y >= bar_y + 4);
    uint32_t btn_bg = g_menu_open ? COLOR_ACCENT_BLUE : (app_hover ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
    draw_rect(6, bar_y + 5, app_btn_w, TASKBAR_HEIGHT - 10, btn_bg);

    draw_mono_icon(12, bar_y + 14, icon_plasma_gear, g_menu_open ? 0x000000 : COLOR_TEXT_MAIN, 0, 0);
    draw_string(32, bar_y + 14, "overOS", g_menu_open ? 0x000000 : COLOR_TEXT_MAIN);

    // 2. Вкладка Dolphin слева
    int task_x = 6 + app_btn_w + 8;
    int task_w = 140;
    int task_hover = (mouse_x >= task_x && mouse_x < task_x + task_w && mouse_y >= bar_y + 5);

    draw_rect(task_x, bar_y + 5, task_w, TASKBAR_HEIGHT - 10,
              task_hover ? COLOR_DOCK_HOVER : (!win->is_minimized ? 0x2E3338 : COLOR_DOCK_ITEM_BG));

    draw_dolphin_folder(task_x + 8, bar_y + 14);
    draw_string(task_x + 30, bar_y + 14, "Dolphin", COLOR_TEXT_MAIN);

    if (!win->is_minimized) {
        draw_rect(task_x + 6, bar_y + TASKBAR_HEIGHT - 3, task_w - 12, 2, COLOR_ACCENT_BLUE);
    }

    // 3. Трей справа
    int tray_x = g_screen_w - 270;
    draw_mono_icon(tray_x, bar_y + 14, icon_sound_mute, COLOR_TEXT_MUTED, COLOR_WARN_RED, 1);
    draw_mono_icon(tray_x + 28, bar_y + 14, icon_network_none, COLOR_TEXT_MUTED, COLOR_WARN_RED, 1);
    draw_mono_icon(tray_x + 56, bar_y + 14, icon_brightness, (g_active_popup == POPUP_BRIGHTNESS) ? COLOR_ACCENT_BLUE : COLOR_TEXT_MAIN, 0, 0);

    // Раскладка [RU] / [EN]
    draw_rect(tray_x + 84, bar_y + 10, 36, 24, COLOR_DOCK_ITEM_BG);
    draw_string(tray_x + 94, bar_y + 14, g_layout_ru ? "RU" : "EN", COLOR_TEXT_MAIN);

    // Часы и дата RTC в 2 строки
    uint8_t local_hour = (t->hour + g_tz_offset) % 24;

    draw_rect(tray_x + 130, bar_y + 4, 130, 36, (g_active_popup == POPUP_CLOCK) ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
    draw_dec2(tray_x + 142, bar_y + 4, local_hour, COLOR_TEXT_MAIN);
    draw_string(tray_x + 158, bar_y + 4, ":", COLOR_TEXT_MAIN);
    draw_dec2(tray_x + 166, bar_y + 4, t->min, COLOR_TEXT_MAIN);
    draw_string(tray_x + 182, bar_y + 4, ":", COLOR_TEXT_MAIN);
    draw_dec2(tray_x + 190, bar_y + 4, t->sec, COLOR_ACCENT_BLUE);

    draw_dec2(tray_x + 142, bar_y + 22, t->day, COLOR_TEXT_MUTED);
    draw_string(tray_x + 158, bar_y + 22, ".", COLOR_TEXT_MUTED);
    draw_dec2(tray_x + 166, bar_y + 22, t->month, COLOR_TEXT_MUTED);
    draw_string(tray_x + 182, bar_y + 22, ".20", COLOR_TEXT_MUTED);
    draw_dec2(tray_x + 206, bar_y + 22, t->year, COLOR_TEXT_MUTED);

    // 4. Меню overOS
    if (g_menu_open) {
        int m_w = 210, m_h = 160;
        int m_x = 6, m_y = bar_y - m_h - 4;

        draw_rect(m_x - 1, m_y - 1, m_w + 2, m_h + 2, COLOR_PANEL_BORDER);
        draw_rect(m_x, m_y, m_w, m_h, COLOR_PANEL_BG);

        draw_rect(m_x, m_y, m_w, 28, 0x1B1E20);
        draw_string(m_x + 12, m_y + 6, "Приложения overOS", COLOR_ACCENT_BLUE);

        int h1 = (mouse_x >= m_x && mouse_x <= m_x + m_w && mouse_y >= m_y + 36 && mouse_y <= m_y + 64);
        if (h1) draw_rect(m_x + 4, m_y + 36, m_w - 8, 26, COLOR_DOCK_HOVER);
        draw_dolphin_folder(m_x + 12, m_y + 41);
        draw_string(m_x + 36, m_y + 41, "Dolphin (Файлы)", COLOR_TEXT_MAIN);

        int h2 = (mouse_x >= m_x && mouse_x <= m_x + m_w && mouse_y >= m_y + 68 && mouse_y <= m_y + 96);
        if (h2) draw_rect(m_x + 4, m_y + 68, m_w - 8, 26, COLOR_DOCK_HOVER);
        draw_string(m_x + 12, m_y + 73, ">_ Konsole", COLOR_TEXT_MAIN);

        int h3 = (mouse_x >= m_x && mouse_x <= m_x + m_w && mouse_y >= m_y + 120 && mouse_y <= m_y + 148);
        if (h3) draw_rect(m_x + 4, m_y + 120, m_w - 8, 26, 0x551111);
        draw_string(m_x + 12, m_y + 125, "[X] Выход в консоль", COLOR_WARN_RED);
    }

    // 5. Ползунок яркости
    if (g_active_popup == POPUP_BRIGHTNESS) {
        int sl_w = 210, sl_h = 68;
        int sl_x = g_screen_w - sl_w - 70;
        int sl_y = bar_y - sl_h - 6;

        draw_rect(sl_x - 1, sl_y - 1, sl_w + 2, sl_h + 2, COLOR_PANEL_BORDER);
        draw_rect(sl_x, sl_y, sl_w, sl_h, COLOR_PANEL_BG);

        draw_string(sl_x + 12, sl_y + 10, "ЯРКОСТЬ:", COLOR_TEXT_MAIN);
        draw_dec(sl_x + 110, sl_y + 10, g_brightness, COLOR_ACCENT_BLUE);
        draw_string(sl_x + (g_brightness == 100 ? 136 : 128), sl_y + 10, "%", COLOR_ACCENT_BLUE);

        int track_x = sl_x + 14, track_y = sl_y + 36, track_w = 172, track_h = 6;
        draw_rect(track_x, track_y, track_w, track_h, 0x141618);

        int fill_w = (track_w * g_brightness) / 100;
        draw_rect(track_x, track_y, fill_w, track_h, COLOR_ACCENT_BLUE);

        int knob_x = track_x + fill_w - 4;
        draw_rect(knob_x, track_y - 4, 8, 14, COLOR_TEXT_MAIN);
    } 
    // 6. Меню часовых поясов
    else if (g_active_popup == POPUP_CLOCK) {
        int cl_w = 220, cl_h = 108;
        int cl_x = g_screen_w - cl_w - 10;
        int cl_y = bar_y - cl_h - 6;

        draw_rect(cl_x - 1, cl_y - 1, cl_w + 2, cl_h + 2, COLOR_PANEL_BORDER);
        draw_rect(cl_x, cl_y, cl_w, cl_h, COLOR_PANEL_BG);

        draw_string(cl_x + 10, cl_y + 8, "ЧАСОВОЙ ПОЯС:", COLOR_ACCENT_BLUE);

        if (g_tz_offset == 0) draw_rect(cl_x + 4, cl_y + 28, cl_w - 8, 22, COLOR_DOCK_HOVER);
        draw_string(cl_x + 12, cl_y + 31, (g_tz_offset == 0) ? "[X] UTC+0 (London)" : "[ ] UTC+0 (London)", COLOR_TEXT_MAIN);

        if (g_tz_offset == 3) draw_rect(cl_x + 4, cl_y + 52, cl_w - 8, 22, COLOR_DOCK_HOVER);
        draw_string(cl_x + 12, cl_y + 55, (g_tz_offset == 3) ? "[X] MSK   (UTC+3)" : "[ ] MSK   (UTC+3)", COLOR_TEXT_MAIN);

        if (g_tz_offset == 9) draw_rect(cl_x + 4, cl_y + 76, cl_w - 8, 22, COLOR_DOCK_HOVER);
        draw_string(cl_x + 12, cl_y + 79, (g_tz_offset == 9) ? "[X] YAKT  (MSK+6)" : "[ ] YAKT  (MSK+6)", COLOR_TEXT_MAIN);
    } 
    else if (g_active_popup != POPUP_NONE && g_popup_timer > 0) {
        int pop_w = 220, pop_h = 56;
        int pop_x = g_screen_w - pop_w - 60;
        int pop_y = bar_y - pop_h - 6;

        draw_rect(pop_x - 1, pop_y - 1, pop_w + 2, pop_h + 2, COLOR_PANEL_BORDER);
        draw_rect(pop_x, pop_y, pop_w, pop_h, COLOR_PANEL_BG);

        if (g_active_popup == POPUP_SOUND) {
            draw_string(pop_x + 12, pop_y + 10, "АУДИО", COLOR_WARN_RED);
            draw_string(pop_x + 12, pop_y + 30, "Драйвер не найден", COLOR_TEXT_MUTED);
        } else if (g_active_popup == POPUP_NET) {
            draw_string(pop_x + 12, pop_y + 10, "СЕТЬ", COLOR_WARN_RED);
            draw_string(pop_x + 12, pop_y + 30, "Адаптер отключен", COLOR_TEXT_MUTED);
        }
    }
}

static void present(void) {
    if (g_fb_fd >= 0) {
        sys_write(g_fb_fd, g_backbuffer, g_buf_bytes);
    }
}

// -----------------------------------------------------------------------------
// Главный цикл
// -----------------------------------------------------------------------------
int main(void) {
    load_config();
    // Восстанавливаем аппаратную яркость экрана из конфига
    sys_ioctl(g_fb_fd, 0x4601, (void*)(uintptr_t)g_brightness);

    g_fb_fd = sys_open("/dev/fb0", 0);
    if (g_fb_fd < 0) sys_exit(1);

    fb_var_info_t fb_info;
    sys_ioctl(g_fb_fd, 0x4600, &fb_info);
    g_screen_w = fb_info.width;
    g_screen_h = fb_info.height;
    g_buf_bytes = g_screen_w * g_screen_h * 4;

    g_backbuffer = (uint32_t*)sys_sbrk((int64_t)g_buf_bytes);
    g_outbuffer  = (uint32_t*)sys_sbrk((int64_t)g_buf_bytes);

    window_t dolphin = {
        .id = 1,
        .x = 100, .y = 80,
        .w = 540, .h = 360,
        .title = "Dolphin - Файлы",
        .is_dragging = 0,
        .is_minimized = 0
    };

    int prev_buttons = 0;
    rtc_time_t real_time = {0, 0, 0, 1, 1, 26};

    while (1) {
        user_mouse_t ms;
        if (sys_get_mouse(&ms) == 0) {
            sys_sleep(50);
            continue;
        }

        // Переключение Alt+Shift
        uint8_t sc = sys_get_key();
        while (sc != 0) {
            if (sc == 0x38) s_alt_down = 1;
            else if (sc == 0xB8) s_alt_down = 0;
            else if (sc == 0x2A || sc == 0x36) s_shift_down = 1;
            else if (sc == 0xAA || sc == 0xB6) s_shift_down = 0;

            if (((sc == 0x2A || sc == 0x36) && s_alt_down) || (sc == 0x38 && s_shift_down)) {
                g_layout_ru = !g_layout_ru;
                save_config();
            }
            sc = sys_get_key();
        }

        int cursor_x = ms.x;
        int cursor_y = ms.y;
        int left_pressed = (ms.buttons & 1);
        int bar_y = g_screen_h - TASKBAR_HEIGHT;

        sys_get_time(&real_time);

        if (g_popup_timer > 0) g_popup_timer--;
        else if (g_active_popup != POPUP_BRIGHTNESS && g_active_popup != POPUP_CLOCK) {
            g_active_popup = POPUP_NONE;
        }

        // Слайдер яркости
        if (g_active_popup == POPUP_BRIGHTNESS) {
            int sl_w = 210, sl_h = 68;
            int sl_x = g_screen_w - sl_w - 70;
            int sl_y = bar_y - sl_h - 6;
            int track_x = sl_x + 14, track_y = sl_y + 36, track_w = 172, track_h = 6;

            if (left_pressed) {
                if (g_brightness_dragging || 
                   (cursor_x >= track_x - 10 && cursor_x <= track_x + track_w + 10 &&
                    cursor_y >= track_y - 10 && cursor_y <= track_y + track_h + 10)) {
                    int val = ((cursor_x - track_x) * 100) / track_w;
                    if (val < 10) val = 10;
                    if (val > 100) val = 100;

                    if (g_brightness != val) {
                        g_brightness = val;
                        // Только меняем ШИМ дисплея (save_config() отсюда УБРАН)
                        sys_ioctl(g_fb_fd, 0x4601, (void*)(uintptr_t)g_brightness);
                    }
                    g_brightness_dragging = 1;
                }
            } else {
                // Кнопку отпустили: сохраняем настройки на диск ровно один раз
                if (g_brightness_dragging) {
                    save_config();
                    g_brightness_dragging = 0;
                }
            }
        }

        // Клики
        if (left_pressed && !(prev_buttons & 1) && !g_brightness_dragging) {
            // Меню overOS
            if (g_menu_open) {
                int m_x = 6, m_y = bar_y - 160 - 4;
                if (cursor_x >= m_x && cursor_x <= m_x + 210 && cursor_y >= m_y + 36 && cursor_y <= m_y + 64) {
                    dolphin.is_minimized = 0;
                    g_menu_open = 0;
                } else if (cursor_x >= m_x && cursor_x <= m_x + 210 && cursor_y >= m_y + 120 && cursor_y <= m_y + 148) {
                    sys_exit(0);
                } else {
                    g_menu_open = 0;
                }
            } else if (cursor_x >= 6 && cursor_x <= 90 && cursor_y >= bar_y + 5) {
                g_menu_open = 1;
                g_active_popup = POPUP_NONE;
            }

            // Вкладка таскбара
            int task_x = 98;
            if (cursor_x >= task_x && cursor_x <= task_x + 140 && cursor_y >= bar_y + 5) {
                dolphin.is_minimized = !dolphin.is_minimized;
            }

            // Трей
            int tray_x = g_screen_w - 270;
            if (cursor_x >= tray_x && cursor_x < tray_x + 24 && cursor_y >= bar_y + 10) {
                if (g_active_popup == POPUP_SOUND) {
                    g_active_popup = POPUP_NONE;
                    g_popup_timer = 0;
                } else {
                    g_active_popup = POPUP_SOUND;
                    g_popup_timer = 180;
                }
            } else if (cursor_x >= tray_x + 28 && cursor_x < tray_x + 52 && cursor_y >= bar_y + 10) {
                if (g_active_popup == POPUP_NET) {
                    g_active_popup = POPUP_NONE;
                    g_popup_timer = 0;
                } else {
                    g_active_popup = POPUP_NET;
                    g_popup_timer = 180;
                }
            } else if (cursor_x >= tray_x + 56 && cursor_x < tray_x + 80 && cursor_y >= bar_y + 10) {
                g_active_popup = (g_active_popup == POPUP_BRIGHTNESS) ? POPUP_NONE : POPUP_BRIGHTNESS;
            } else if (cursor_x >= tray_x + 84 && cursor_x < tray_x + 120 && cursor_y >= bar_y + 10) {
                g_layout_ru = !g_layout_ru;
                save_config();
            } else if (cursor_x >= tray_x + 130 && cursor_x < tray_x + 260 && cursor_y >= bar_y + 4) {
                g_active_popup = (g_active_popup == POPUP_CLOCK) ? POPUP_NONE : POPUP_CLOCK;
            }

            // Выбор часового пояса
            if (g_active_popup == POPUP_CLOCK) {
                int cl_w = 220, cl_h = 108;
                int cl_x = g_screen_w - cl_w - 10;
                int cl_y = bar_y - cl_h - 6;

                if (cursor_x >= cl_x && cursor_x <= cl_x + cl_w) {
                    if (cursor_y >= cl_y + 28 && cursor_y < cl_y + 50) {
                        g_tz_offset = 0; save_config();
                    } else if (cursor_y >= cl_y + 52 && cursor_y < cl_y + 74) {
                        g_tz_offset = 3; save_config();
                    } else if (cursor_y >= cl_y + 76 && cursor_y < cl_y + 98) {
                        g_tz_offset = 9; save_config();
                    }
                }
            }

            // Окно Dolphin
            if (!dolphin.is_minimized) {
                if (cursor_x >= dolphin.x + dolphin.w - 22 && cursor_x <= dolphin.x + dolphin.w - 6 &&
                    cursor_y >= dolphin.y + 5 && cursor_y <= dolphin.y + 21) {
                    dolphin.is_minimized = 1;
                } else if (cursor_x >= dolphin.x && cursor_x <= dolphin.x + dolphin.w &&
                           cursor_y >= dolphin.y && cursor_y <= dolphin.y + TITLE_BAR_HEIGHT) {
                    dolphin.is_dragging = 1;
                    dolphin.drag_off_x = cursor_x - dolphin.x;
                    dolphin.drag_off_y = cursor_y - dolphin.y;
                }
            }
        }

        if (!left_pressed) dolphin.is_dragging = 0;

        if (dolphin.is_dragging) {
            dolphin.x = cursor_x - dolphin.drag_off_x;
            dolphin.y = cursor_y - dolphin.drag_off_y;
        }

        prev_buttons = ms.buttons;

        draw_rect(0, 0, g_screen_w, g_screen_h - TASKBAR_HEIGHT, COLOR_DESKTOP_BG);
        render_window(&dolphin, 1);
        render_plasma_taskbar(&dolphin, cursor_x, cursor_y, &real_time);
        draw_cursor(cursor_x, cursor_y);

        present();
        sys_sleep(16);
    }

    return 0;
}