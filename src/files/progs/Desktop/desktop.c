#include "syscalls.h"
#include "theme.h"
#include "gfx.h"
#include "settings.h"
#include "window.h"
#include "osver.h"
#include "taskbar.h"
#include "greeter.h"
#include "wserver.h"
#include "wproto.h"
#include "ovos.h"
#include "notifier.h"

static int s_alt_down   = 0;
static int s_shift_down = 0;
static int s_fb_fd      = -1;

// Настройки сеанса (~/.config/desktop.json): у каждого пользователя свои обои,
// громкость и яркость. До входа действует общесистемный /configs/desktop.json.
static void apply_session_settings(void) {
    settings_set_user(g_session_uid, g_session_gid);
    sys_ioctl(s_fb_fd, 0x4601, (void*)(uintptr_t)g_brightness);
    sys_audio_set_volume(g_volume);
    gfx_load_wallpaper(g_wallpaper_path);
}

// Часы для ограничения частоты кадров: TSC делим на калибровку ядра
static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    int fb_fd = sys_open("/dev/fb0", 0);
    if (fb_fd < 0) sys_exit(1);
    s_fb_fd = fb_fd;

    gfx_init(fb_fd);
    settings_load();

    // Восстановление подсветки дисплея
    sys_ioctl(fb_fd, 0x4601, (void*)(uintptr_t)g_brightness);

    // Инициализация оконной подсистемы и оверлея
    window_manager_init();
    osver_init();

    taskbar_refresh_menu_apps();

    // Загрузка обоев из пути, указанного в desktop.json
    gfx_load_wallpaper(g_wallpaper_path);

    // Композитор: принимаем окна приложений по IPC (имя "desktop")
    wserver_init();
    // Уведомления: системные (из ядра) и программ (WP_NOTIFY)
    notifier_init();

    // Первый запуск: пользователей ещё нет — мастер настройки (как GNOME Initial Setup)
    if (sys_oobe_state() == 1) oobe_run();

    // Окно входа: дальше desktop работает как сеанс вошедшего пользователя
    greeter_run();
    apply_session_settings();

    int prev_mx = -1, prev_my = -1;
    int prev_buttons = 0;
    uint8_t prev_sec = 255;
    rtc_time_t real_time = {0, 0, 0, 1, 1, 26};

    int redraw_next = 0;
    uint64_t tsc_ms = sys_get_tsc_per_ms();     // 0 — калибровки нет, тогда без ограничения кадров
    uint64_t last_frame = 0, last_cursor = 0;
    int cursor_pending = 0;
    while (1) {
        int need_redraw = redraw_next;
        redraw_next = 0;

        // Запросы клиентов (новые окна, готовые кадры) и упавшие клиенты
        if (wserver_poll(0)) need_redraw = 1;
        if (wserver_tick()) need_redraw = 1;

        if (g_logout_requested) {
            g_logout_requested = 0;
            g_menu_open = 0;
            g_power_menu_open = 0;
            session_logout();
            notifier_clear_apps();
            apply_session_settings();
            taskbar_refresh_menu_apps();
            prev_mx = -1;
            continue;
        }

        int st, zp;
        while ((zp = sys_waitpid(-1, &st, 1)) > 0) {
            if (st > 0) taskbar_report_child_exit(zp, st);
            need_redraw = 1;
        }

        user_mouse_t ms;
        if (sys_get_mouse(&ms) == 0) {
            redraw_next = wserver_poll(10);
            continue;
        }

        int cursor_x = ms.x;
        int cursor_y = ms.y;
        // Мышь захвачена игрой: смещения уходят ей, а для остального рабочего
        // стола курсор стоит в центре её окна и кнопки не нажаты
        if (window_grab_active()) {
            window_grab_feed(&cursor_x, &cursor_y);
            ms.buttons = 0;
        }
        int left_pressed  = (ms.buttons & 1);
        int right_pressed = (ms.buttons & 2);
        int bar_y = g_screen_h - TASKBAR_HEIGHT;

        // Движение мыши само по себе сцену не меняет: курсор рисуется поверх
        // сохранённого куска буфера (gfx_cursor_*). Полная перерисовка нужна
        // только там, где от позиции курсора зависит картинка — панель, меню,
        // шапки и края окон. Раньше любой шаг мыши перерисовывал весь экран,
        // и композитор съедал почти половину процессора.
        int cursor_moved = (cursor_x != prev_mx || cursor_y != prev_my);
        if (ms.buttons != prev_buttons) need_redraw = 1;
        if (cursor_moved && (cursor_y >= bar_y || g_menu_open || g_ctx_menu.active ||
                             notifier_inside(cursor_x, cursor_y) || notifier_inside(prev_mx, prev_my) ||
                             g_active_popup != POPUP_NONE || window_hover_dirty(cursor_x, cursor_y)))
            need_redraw = 1;

        sys_get_time(&real_time);
        if (real_time.sec != prev_sec) {
            prev_sec = real_time.sec;
            need_redraw = 1;
        }
        // Уведомления: новые из ядра, отсчёт времени показа
        if (notifier_tick(cursor_x, cursor_y)) need_redraw = 1;

        // --- Клавиатура: Meta-кнопка, переключение раскладки и отправка окнам ---
        uint8_t sc = sys_get_key();
        while (sc != 0) {
            need_redraw = 1;

            // [КЛАВИША META]: Открытие и закрытие меню Пуск
            if (sc == 0x5B || sc == 0x5C) {
                window_grab_release();          // Meta всегда возвращает мышь рабочему столу
                desktop_ctx_close();
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

            if (g_has_layout2 && (((sc == 0x2A || sc == 0x36) && s_alt_down) || (sc == 0x38 && s_shift_down))) {
                g_layout_ru = !g_layout_ru;
                settings_save();
            }

            window_post_key(sc);
            sc = sys_get_key();
        }

        // --- Обработка нажатия ПКМ (Контекстное меню) ---
        int right_click_started = right_pressed && !(prev_buttons & 2);
        if (right_click_started) {
            g_menu_open = 0;
            g_active_popup = POPUP_NONE;

            // Окно под курсором — по раскладке окон desktop (раньше брались координаты из
            // реестра ядра, которые не совпадают с реальными, и меню открывалось не там)
            window_t* hw = window_at(cursor_x, cursor_y);
            int is_explorer = 0;
            if (hw && hw->proto) {
                is_explorer = (hw->flags & WP_FLAG_FILE_BROWSER) != 0;
            }

            if (cursor_y >= bar_y) {
                taskbar_ctx_at(cursor_x, cursor_y);     // меню значка в панели задач
            } else if (is_explorer) {
                desktop_ctx_open(cursor_x, cursor_y, CTX_TYPE_EXPLORER, hw->id);
            } else if (!hw && cursor_y < bar_y) {
                desktop_ctx_open(cursor_x, cursor_y, CTX_TYPE_DESKTOP, -1);
            }
            need_redraw = 1;
        }

        // --- Обработка нажатия ЛКМ ---
        int click_started = left_pressed && !(prev_buttons & 1);
        // Уведомления лежат поверх всего: клик по ним дальше не идёт
        if (click_started && notifier_click(cursor_x, cursor_y)) {
            prev_buttons = ms.buttons;
            prev_mx = cursor_x; prev_my = cursor_y;
            redraw_next = 1;
            continue;
        }
        int click_on_ctx = g_ctx_menu.active && click_started;

        if (click_on_ctx) {
            desktop_ctx_handle_click(cursor_x, cursor_y);
            need_redraw = 1;
        }

        int click_on_ui = click_started && (
                          g_brightness_dragging ||
                          g_volume_dragging ||
                          click_on_ctx ||
                          cursor_y >= bar_y ||
                          taskbar_is_inside_menu(cursor_x, cursor_y) ||
                          taskbar_is_inside_popup(cursor_x, cursor_y));


        int wheel = sys_get_wheel();
        if (wheel) {
            if (taskbar_popup_scroll(cursor_x, cursor_y, wheel)) need_redraw = 1;
            else window_post_scroll(cursor_x, cursor_y, wheel);
        }
        if (taskbar_popup_drag(cursor_y, left_pressed)) need_redraw = 1;
        if (window_handle_mouse(cursor_x, cursor_y, click_on_ui ? 0 : left_pressed, prev_buttons & 1)) {
            need_redraw = 1;
        }

        // --- Слайдер яркости ---
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
                    if (val < 0) val = 0;
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

        // --- Слайдер громкости ---
        if (g_active_popup == POPUP_SOUND) {
            int pop_w = 260, pop_h = 210;
            int pop_x = g_screen_w - pop_w - 60;
            int pop_y = bar_y - pop_h - 6;
            int track_x = pop_x + 14, track_y = pop_y + 36, track_w = 180, track_h = 6;

            if (left_pressed) {
                if (g_volume_dragging ||
                   (cursor_x >= track_x - 10 && cursor_x <= track_x + track_w + 10 &&
                    cursor_y >= track_y - 10 && cursor_y <= track_y + track_h + 10)) {
                    int val = ((cursor_x - track_x) * 100) / track_w;
                    if (val < 0) val = 0;
                    if (val > 100) val = 100;

                    if (g_volume != val) {
                        g_volume = val;
                    }
                    g_volume_dragging = 1;
                    need_redraw = 1;
                }
            } else if (g_volume_dragging) {
                settings_save();
                g_volume_dragging = 0;
                need_redraw = 1;
            }
        }

        // Закрытие меню при клике в пустое место
        if (click_started && !g_brightness_dragging && !g_volume_dragging && !click_on_ctx) {
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

        cursor_moved |= cursor_pending;
        int commits = wserver_has_commits();     // окна прислали новые кадры
        if (!need_redraw && !cursor_moved && !commits) {
            // Ждём сообщения клиентов (новый кадр будит нас сразу) или следующий опрос мыши
            cursor_pending = 0;
            redraw_next = wserver_poll(8);
            continue;
        }

        // Не чаще 60 кадров в секунду. Мышь опрашивается каждые 2 мс, и без
        // этого ограничения сцена рисовалась сотни раз в секунду впустую.
        // У курсора свой отсчёт: раньше он тратил общий «слот» кадра, и окно,
        // приславшее кадр сразу после шага мыши, ждало лишние 16 мс — игра
        // падала до 30 FPS, стоило подвигать мышью.
        uint64_t now_ms = tsc_ms ? (rdtsc_now() / tsc_ms) : 0;
        if (!need_redraw && !commits) {
            if (tsc_ms && (now_ms - last_cursor) < 16) {
                cursor_pending = 1;
                redraw_next = wserver_poll(2);
                continue;
            }
            last_cursor = now_ms;
            cursor_pending = 0;
            // Переехал только курсор: возвращаем на место то, что было под ним,
            // рисуем в новой точке и выводим лишь затронутые строки.
            int oy0, oy1;
            gfx_cursor_rows(&oy0, &oy1);
            gfx_cursor_restore();
            gfx_draw_cursor(cursor_x, cursor_y, window_cursor_at(cursor_x, cursor_y));
            int ny0, ny1;
            gfx_cursor_rows(&ny0, &ny1);
            int y0 = ny0, y1 = ny1;
            if (oy1 > oy0) {
                if (oy0 < y0) y0 = oy0;
                if (oy1 > y1) y1 = oy1;
            }
            gfx_present_rows(y0, y1);
            redraw_next = wserver_poll(4);
            continue;
        }
        if (tsc_ms && (now_ms - last_frame) < 16) {
            redraw_next = need_redraw;          // причина перерисовки не теряется
            cursor_pending = cursor_moved;
            redraw_next |= wserver_poll(2);
            continue;
        }
        cursor_pending = 0;
        last_frame = now_ms;
        last_cursor = now_ms;

        // Сменились только кадры окон, и поверх них ничего нет: перекладываем
        // их клиентские области и выводим только эти строки. Обои, шапки и
        // панель не трогаем — на окне во весь экран это половина работы кадра.
        int fy0, fy1;
        if (!need_redraw && !g_menu_open && !g_power_menu_open && !g_ctx_menu.active &&
            g_active_popup == POPUP_NONE) {
            int oy0, oy1;
            gfx_cursor_rows(&oy0, &oy1);
            gfx_cursor_restore();                // под курсором -- старый кадр окна
            if (window_blit_committed(&fy0, &fy1)) {
                wserver_frame_done();            // буферы клиентов прочитаны
                gfx_draw_cursor(cursor_x, cursor_y, window_cursor_at(cursor_x, cursor_y));
                int ny0, ny1;
                gfx_cursor_rows(&ny0, &ny1);
                int ys[3][2] = { { fy0, fy1 }, { oy0, oy1 }, { ny0, ny1 } };
                int y0 = 1 << 30, y1 = 0;
                for (int i = 0; i < 3; i++) {
                    if (ys[i][1] <= ys[i][0]) continue;
                    if (ys[i][0] < y0) y0 = ys[i][0];
                    if (ys[i][1] > y1) y1 = ys[i][1];
                }
                if (y1 > y0) gfx_present_rows(y0, y1);
                redraw_next = wserver_poll(4);
                continue;
            }
            // Быстро нельзя -- ниже полный кадр, курсор он нарисует сам
        }

        // Рендеринг рабочего стола
        gfx_cursor_forget();               // буфер сейчас перерисуется целиком
        if (!window_covers_desktop()) gfx_draw_wallpaper();
        window_render_all(cursor_x, cursor_y);
        // Кадры клиентов уже лежат в заднем буфере: отпускаем их рисовать
        // следующий, пока мы дорисовываем панель и выводим экран. Раньше
        // WP_FRAME_DONE уходил после вывода, и клиент с композитором работали
        // строго по очереди: кадр игры стоил её время плюс наше.
        wserver_frame_done();
        taskbar_render(cursor_x, cursor_y, &real_time);
        desktop_ctx_render(cursor_x, cursor_y);
        window_draw_resize_hint();
        notifier_render(cursor_x, cursor_y);   // поверх окон и панели: это важнее любого из них
        // Форма курсора: края окон знает композитор, внутри окна её просит сама
        // программа (WP_CURSOR)
        gfx_draw_cursor(cursor_x, cursor_y, window_cursor_at(cursor_x, cursor_y));

        gfx_present();
        redraw_next = wserver_poll(4);
    }

    return 0;
}