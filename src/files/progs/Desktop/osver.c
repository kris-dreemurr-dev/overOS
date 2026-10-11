#include "osver.h"
#include "window.h"
#include "gfx.h"
#include "theme.h"
#include "icons.h"
#include "config.h"
#include "syscalls.h"

static window_t* s_osver_win = 0;

static void osver_render(window_t* win) {
    int cx = win->x + 20;
    int cy = win->y + TITLE_BAR_HEIGHT + 14;

    // Запрашиваем информацию о ядре через системный вызов
    kernel_info_t kinfo = { "overOS", "x86_64", "0.0.1" };
    sys_get_kernel_info(&kinfo);

    // 1. Логотип и заголовок
    gfx_draw_mono_icon(cx, cy + 2, icon_plasma_gear, COLOR_ACCENT_BLUE, 0, 0);
    gfx_draw_string(cx + 24, cy, "overOS", COLOR_ACCENT_BLUE);
    gfx_draw_string(cx + 24, cy + 18, "Операционная система", COLOR_TEXT_MUTED);

    // Разделитель Breeze
    gfx_draw_rect(cx, cy + 40, win->w - 40, 1, COLOR_PANEL_BORDER);

    // 2. Блок параметров
    int iy = cy + 50;

    // DE: overDesktop 0.0.1 devbuild
    gfx_draw_string(cx, iy, "DE:     " DE_NAME " " DE_VERSION, COLOR_TEXT_MAIN);

    // Kernel: overOS (x86-64) 0.0.1 devbuild
    char k_buf[64];
    int p = 0;
    const char* pfx = "Kernel: ";
    while (*pfx) k_buf[p++] = *pfx++;
    const char* kn = kinfo.name; while (*kn) k_buf[p++] = *kn++;
    k_buf[p++] = ' '; k_buf[p++] = '(';
    const char* ka = kinfo.arch; while (*ka) k_buf[p++] = *ka++;
    k_buf[p++] = ')'; k_buf[p++] = ' ';
    const char* kv = kinfo.version; while (*kv) k_buf[p++] = *kv++;
    k_buf[p] = '\0';
    gfx_draw_string(cx, iy + 20, k_buf, COLOR_TEXT_MAIN);

    // Тема: Default
    gfx_draw_string(cx, iy + 40, "Тема:   " DE_THEME, COLOR_TEXT_MUTED);

    // 3. Кнопка [OK]
    int btn_w = 70, btn_h = 24;
    int btn_x = win->x + win->w - btn_w - 20;
    int btn_y = win->y + TITLE_BAR_HEIGHT + win->h - btn_h - 12;
    gfx_draw_rect(btn_x, btn_y, btn_w, btn_h, COLOR_ACCENT_BLUE);
    gfx_draw_string(btn_x + 26, btn_y + 4, "OK", 0x000000);
}

static void osver_click(window_t* win, int lx, int ly) {
    // Клик по кнопке [OK] закрывает окно
    int btn_w = 70, btn_h = 24;
    int btn_x = win->w - btn_w - 20;
    int btn_y = win->h - btn_h - 12;

    if (lx >= btn_x && lx <= btn_x + btn_w && ly >= btn_y && ly <= btn_y + btn_h) {
        win->is_open = 0;
        win->is_active = 0;
    }
}

void osver_init(void) {
    int w = 440, h = 210;
    int x = (g_screen_w - w) / 2;
    int y = (g_screen_h - h) / 2 - 20;

    // Окно регистрируется один раз, но создаётся закрытым (is_open = 0)
    s_osver_win = window_create("О системе overOS", x, y, w, h,
                                icon_plasma_gear, osver_render, osver_click);
    if (s_osver_win) {
        s_osver_win->is_open = 0; // ПРИ СТАРТЕ ЗАКРЫТО!
    }
}

void osver_show(void) {
    if (s_osver_win) {
        s_osver_win->is_open = 1;      // Открываем только по клику из меню
        s_osver_win->is_minimized = 0;
        s_osver_win->is_active = 1;
    }
}