#include "syscalls.h"
#include "theme.h"
#include "gfx.h"
#include "settings.h"
#include "window.h"
#include "osver.h"
#include "taskbar.h"

static int s_alt_down   = 0;
static int s_shift_down = 0;

int main(void) {
    int fb_fd = sys_open("/dev/fb0", 0);
    if (fb_fd < 0) sys_exit(1);

    gfx_init(fb_fd);
    settings_load();

    // Восстановление подсветки
    sys_ioctl(fb_fd, 0x4601, (void*)(uintptr_t)g_brightness);

    // Инициализация оконной подсистемы
    window_manager_init();
    osver_init();

    taskbar_refresh_menu_apps();

    // Загрузка обоев из пути, указанного в desktop.json
    gfx_load_wallpaper(g_wallpaper_path);

    int prev_mx = -1, prev_my = -1;
    int prev_buttons = 0;
    uint8_t prev_sec = 255;
    rtc_time_t real_time = {0, 0, 0, 1, 1, 26};

    while (1) {
        int need_redraw = 0;

        int st, zp;
        while ((zp = sys_waitpid(-1, &st, 1)) > 0) {
            if (st > 0) taskbar_report_child_exit(zp, st);
            need_redraw = 1;
        }

        user_mouse_t ms;
        if (sys_get_mouse(&ms) == 0) {
            sys_sleep(10);
            continue;
        }

        int cursor_x = ms.x;
        int cursor_y = ms.y;
        int left_pressed = (ms.buttons & 1);
        int bar_y = g_screen_h - TASKBAR_HEIGHT;

        if (cursor_x != prev_mx || cursor_y != prev_my || ms.buttons != prev_buttons) {
            need_redraw = 1;
        }

        sys_get_time(&real_time);
        if (real_time.sec != prev_sec) {
            prev_sec = real_time.sec;
            need_redraw = 1;
        }

        // Клавиатура: Meta-кнопка, переключение раскладки и отправка окнам
        uint8_t sc = sys_get_key();
        while (sc != 0) {
            need_redraw = 1;

            // [КЛАВИША META / SUPER / WIN]: Открытие и закрытие меню Пуск
            if (sc == 0x5B || sc == 0x5C) {
                if (g_menu_open) {
                    g_menu_open = 0;
                } else {
                    taskbar_refresh_menu_apps();
                    g_menu_open = 1;
                    g_active_popup = POPUP_NONE;
                }
            }

            if (sc == 0x38) s_alt_down = 1;
            else if (sc == 0xB8) s_alt_down = 0;
            else if (sc == 0x2A || sc == 0x36) s_shift_down = 1;
            else if (sc == 0xAA || sc == 0xB6) s_shift_down = 0;

            if (((sc == 0x2A || sc == 0x36) && s_alt_down) || (sc == 0x38 && s_shift_down)) {
                g_layout_ru = !g_layout_ru;
                settings_save();
            }

            window_post_key(sc);
            sc = sys_get_key();
        }

        int click_started = left_pressed && !(prev_buttons & 1);
        int click_on_ui = click_started && !g_brightness_dragging &&
                          (cursor_y >= bar_y ||
                           taskbar_is_inside_menu(cursor_x, cursor_y) ||
                           taskbar_is_inside_popup(cursor_x, cursor_y));

        window_sync_from_kernel();
        if (window_handle_mouse(cursor_x, cursor_y, click_on_ui ? 0 : left_pressed, prev_buttons & 1)) {
            need_redraw = 1;
        }

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
                        sys_ioctl(fb_fd, 0x4601, (void*)(uintptr_t)g_brightness);
                    }
                    g_brightness_dragging = 1;
                    need_redraw = 1;
                }
            } else if (g_brightness_dragging) {
                settings_save();
                g_brightness_dragging = 0;
                need_redraw = 1;
            }
        }

        if (click_started && !g_brightness_dragging) {
            if (g_menu_open && !taskbar_is_inside_menu(cursor_x, cursor_y) && cursor_y < bar_y) {
                g_menu_open = 0;
                need_redraw = 1;
            }
            if (g_active_popup != POPUP_NONE && !taskbar_is_inside_popup(cursor_x, cursor_y) && cursor_y < bar_y) {
                g_active_popup = POPUP_NONE;
                need_redraw = 1;
            }

            if (click_on_ui) {
                if (taskbar_handle_click(cursor_x, cursor_y)) {
                    need_redraw = 1;
                }
            }
        }

        prev_mx = cursor_x;
        prev_my = cursor_y;
        prev_buttons = ms.buttons;

        if (!need_redraw) {
            sys_sleep(10);
            continue;
        }

        // Рендеринг обоев рабочего стола с авто-фоллбэком на цвет
        gfx_draw_wallpaper();

        window_render_all();
        taskbar_render(cursor_x, cursor_y, &real_time);
        gfx_draw_cursor(cursor_x, cursor_y);

        gfx_present();
        sys_sleep(4);
    }

    return 0;
}