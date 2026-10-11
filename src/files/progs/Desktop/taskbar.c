#include "taskbar.h"
#include "window.h"
#include "osver.h"
#include "gfx.h"
#include "theme.h"
#include "settings.h"
#include "icons.h"

int        g_active_popup = POPUP_NONE;
uint32_t   g_popup_timer = 0;
int        g_menu_open = 0;
int        g_brightness_dragging = 0;
menu_app_t g_menu_apps[MAX_MENU_APPS];
int        g_menu_app_count = 0;
int        g_power_menu_open = 0;

static window_t* s_err_win = 0;
static char      s_err_msg[512] = "";

sys_net_info_t g_net_info = {0};
static uint8_t s_last_net_check_sec = 255;

static void itoa(int n, char* str) {
    int i = 0;
    int is_negative = 0;

    if (n == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }

    if (n < 0) {
        is_negative = 1;
        n = -n;
    }

    while (n != 0) {
        str[i++] = (n % 10) + '0';
        n = n / 10;
    }

    if (is_negative) {
        str[i++] = '-';
    }
    str[i] = '\0';

    // Разворачиваем строку
    int start = 0, end = i - 1;
    while (start < end) {
        char tmp = str[start];
        str[start] = str[end];
        str[end] = tmp;
        start++;
        end--;
    }
}

static void error_dialog_render(window_t* win) {
    gfx_draw_mono_icon(win->x + 20, win->y + TITLE_BAR_HEIGHT + 24, icon_placeholder, COLOR_WARN_RED, 0, 0);
    gfx_draw_string(win->x + 48, win->y + TITLE_BAR_HEIGHT + 24, s_err_msg, COLOR_TEXT_MAIN);

    int btn_w = 70, btn_h = 24;
    int btn_x = win->x + (win->w - btn_w) / 2;
    int btn_y = win->y + TITLE_BAR_HEIGHT + win->h - btn_h - 12;
    gfx_draw_rect(btn_x, btn_y, btn_w, btn_h, COLOR_ACCENT_BLUE);
    gfx_draw_string(btn_x + 26, btn_y + 4, "OK", 0x000000);
}

static void error_dialog_click(window_t* win, int lx, int ly) {
    int btn_w = 70, btn_h = 24;
    int btn_x = (win->w - btn_w) / 2;
    int btn_y = win->h - btn_h - 12;
    if (lx >= btn_x && lx <= btn_x + btn_w && ly >= btn_y && ly <= btn_y + btn_h) {
        win->is_open = 0;
        win->is_active = 0;
    }
}

void show_error_dialog(const char* msg) {
    int p = 0;
    while (msg[p] && p < 127) { s_err_msg[p] = msg[p]; p++; }
    s_err_msg[p] = '\0';

    if (!s_err_win) {
        int w = 480, h = 130;
        int x = (g_screen_w - w) / 2;
        int y = (g_screen_h - h) / 2;
        s_err_win = window_create("Ошибка", x, y, w, h, icon_placeholder, error_dialog_render, error_dialog_click);
    }
    if (s_err_win) {
        s_err_win->is_open = 1;
        s_err_win->is_minimized = 0;
        s_err_win->is_active = 1;
    }
}

void taskbar_report_child_exit(int pid, int code) {
    if (code == 0) return;
    char msg[64] = "Процесс завершился с кодом ";
    int p = 0; while (msg[p]) p++;
    if (code < 0) { msg[p++] = '-'; code = -code; }
    char tmp[12]; int t = 0;
    do { tmp[t++] = '0' + (code % 10); code /= 10; } while (code && t < 11);
    while (t) msg[p++] = tmp[--t];
    msg[p] = '\0';
    (void)pid;
    show_error_dialog(msg);
}

int launch_file_safe(const char* filepath) {
    int len = 0;
    while (filepath[len]) len++;

    // 1. Блокировка .sys файлов
    if (len > 4 && (filepath[len-4] == '.' && 
       (filepath[len-3] == 's' || filepath[len-3] == 'S') &&
       (filepath[len-2] == 'y' || filepath[len-2] == 'Y') &&
       (filepath[len-1] == 's' || filepath[len-1] == 'S'))) {
        show_error_dialog(".sys файлы не поддерживаются графической оболочкой");
        return -1;
    }

    // 2. Открытие файла и чтение заголовка DPRG
    int fd = sys_open(filepath, 0);
    if (fd < 0) {
        show_error_dialog("Не удалось открыть исполняемый файл");
        return -1;
    }

    uint8_t hdr_buf[64];
    int rd = sys_read(fd, hdr_buf, 64);
    sys_close(fd);

    if (rd < 64 || hdr_buf[0] != 'D' || hdr_buf[1] != 'P' || hdr_buf[2] != 'R' || hdr_buf[3] != 'G') {
        show_error_dialog(".PRG файл не поддерживается графической оболочкой");
        return -2;
    }

    uint64_t flags = *(uint64_t*)&hdr_buf[48];
    if (!(flags & 0x01)) { // Флаг PRG_FLAG_DESKTOP отсутствует
        show_error_dialog(".PRG файл не поддерживается графической оболочкой");
        return -2;
    }

    // 3. Запуск проверенной программы
    int pid = sys_fork();
    if (pid < 0) { show_error_dialog("fork не удался"); return -3; }
    if (pid == 0) {
        int r = sys_execve(filepath, 0);
        sys_exit(100 + (-r));   // 101 = -1, 102 = read fail, 103.. см. case 31
    }
return 0;
}

// Проверка попадания клика внутрь меню Пуск
int taskbar_is_inside_menu(int mx, int my) {
    if (!g_menu_open) return 0;
    int bar_y = g_screen_h - TASKBAR_HEIGHT;
    int item_h = 26;
    int m_w = 230;
    int m_h = 50 + ((g_menu_app_count + 1) * item_h) + 38;
    int m_x = 6, m_y = bar_y - m_h - 4;

    // Проверяем само меню
    if (mx >= m_x && mx <= m_x + m_w && my >= m_y && my <= m_y + m_h) return 1;

    // Проверяем выпадающее подменю питания
    if (g_power_menu_open) {
        int sub_x = m_x + m_w + 2;
        int sub_w = 170;
        int sub_h = 3 * 24 + 4;
        int sub_y = m_y + m_h - sub_h;
        if (mx >= sub_x && mx <= sub_x + sub_w && my >= sub_y && my <= sub_y + sub_h) {
            return 1;
        }
    }

    return 0;
}

// Проверка попадания клика внутрь открытого попапа
int taskbar_is_inside_popup(int mx, int my) {
    int bar_y = g_screen_h - TASKBAR_HEIGHT;
    if (g_active_popup == POPUP_BRIGHTNESS) {
        int sl_w = 210, sl_h = 68;
        int sl_x = g_screen_w - sl_w - 70, sl_y = bar_y - sl_h - 6;
        return (mx >= sl_x && mx <= sl_x + sl_w && my >= sl_y && my <= sl_y + sl_h);
    } else if (g_active_popup == POPUP_CLOCK) {
        int cl_w = 220, cl_h = 108;
        int cl_x = g_screen_w - cl_w - 10, cl_y = bar_y - cl_h - 6;
        return (mx >= cl_x && mx <= cl_x + cl_w && my >= cl_y && my <= cl_y + cl_h);
    } else if (g_active_popup == POPUP_NET) {
        int pop_w = 230, pop_h = 104;
        int pop_x = g_screen_w - pop_w - 60, pop_y = bar_y - pop_h - 6;
        return (mx >= pop_x && mx <= pop_x + pop_w && my >= pop_y && my <= pop_y + pop_h);
    } else if (g_active_popup == POPUP_SOUND) {
        int pop_w = 220, pop_h = 56;
        int pop_x = g_screen_w - pop_w - 60, pop_y = bar_y - pop_h - 6;
        return (mx >= pop_x && mx <= pop_x + pop_w && my >= pop_y && my <= pop_y + pop_h);
    }
    return 0;
}

void taskbar_refresh_menu_apps(void) {
    g_menu_app_count = 0;
    sys_file_info_t files[MAX_MENU_APPS];
    
    // Запрашиваем каталог строго от корня диска
    int count = sys_get_dir_files("/desprogs", files, MAX_MENU_APPS);

    for (int i = 0; i < count; i++) {
        // Пропускаем папки
        if (files[i].attr & 0x10) continue;

        const char* fn = files[i].name;
        int len = 0;
        while (fn[len]) len++;

        if (len > 4 && (fn[len-4] == '.' && 
           (fn[len-3] == 'p' || fn[len-3] == 'P') &&
           (fn[len-2] == 'r' || fn[len-2] == 'R') &&
           (fn[len-1] == 'g' || fn[len-1] == 'G'))) {

            // Имя для отображения в меню
            int p = 0;
            while (fn[p] && p < 31) { 
                g_menu_apps[g_menu_app_count].name[p] = fn[p]; 
                p++; 
            }
            g_menu_apps[g_menu_app_count].name[p] = '\0';

            // Полный путь для запуска: /desprogs/ovfiles.prg
            const char* prefix = "/desprogs/";
            int pp = 0;
            while (prefix[pp]) { 
                g_menu_apps[g_menu_app_count].path[pp] = prefix[pp]; 
                pp++; 
            }
            p = 0;
            while (fn[p] && pp < 47) { 
                g_menu_apps[g_menu_app_count].path[pp++] = fn[p++]; 
            }
            g_menu_apps[g_menu_app_count].path[pp] = '\0';

            g_menu_app_count++;
            if (g_menu_app_count >= MAX_MENU_APPS) break;
        }
    }
}

void taskbar_render(int mouse_x, int mouse_y, const rtc_time_t* t) {
    // Периодический опрос сети раз в 5 секунд
    if ((t->sec % 5 == 0) && (t->sec != s_last_net_check_sec)) {
        s_last_net_check_sec = t->sec;
        sys_get_net_info(&g_net_info);
    }

    int bar_y = g_screen_h - TASKBAR_HEIGHT;

    gfx_draw_rect(0, bar_y, g_screen_w, 1, COLOR_PANEL_BORDER);
    gfx_draw_rect(0, bar_y + 1, g_screen_w, TASKBAR_HEIGHT - 1, COLOR_PANEL_BG);

    // 1. Кнопка overOS
    int app_btn_w = 84;
    int app_hover = (mouse_x >= 6 && mouse_x <= 6 + app_btn_w && mouse_y >= bar_y + 4);
    uint32_t btn_bg = g_menu_open ? COLOR_ACCENT_BLUE : (app_hover ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
    gfx_draw_rect(6, bar_y + 5, app_btn_w, TASKBAR_HEIGHT - 10, btn_bg);
    gfx_draw_mono_icon(12, bar_y + 14, icon_plasma_gear, g_menu_open ? 0x000000 : COLOR_TEXT_MAIN, 0, 0);
    gfx_draw_string(32, bar_y + 14, "overOS", g_menu_open ? 0x000000 : COLOR_TEXT_MAIN);

    // 2. Вкладки запущенных окон
    int win_cnt = 0;
    window_t* wins = window_get_list(&win_cnt);
    int tab_x = 6 + app_btn_w + 8;
    int tab_w = 140;

    for (int i = 0; i < win_cnt; i++) {
        window_t* w = &wins[i];
        if (!w->is_open) continue;
        if (tab_x + tab_w > (int)g_screen_w - 280) break;

        int tab_hover = (mouse_x >= tab_x && mouse_x < tab_x + tab_w && mouse_y >= bar_y + 5);
        uint32_t tab_bg = tab_hover ? COLOR_DOCK_HOVER : (!w->is_minimized ? 0x2E3338 : COLOR_DOCK_ITEM_BG);
        gfx_draw_rect(tab_x, bar_y + 5, tab_w, TASKBAR_HEIGHT - 10, tab_bg);

        // Безопасное копирование первых символов заголовка UTF-8
        char tab_title[32];
        int s = 0, d = 0, chars = 0;
        while (w->title[s] && chars < 10 && d < 28) {
            uint8_t c = (uint8_t)w->title[s];
            if (c >= 0xC0) {
                if (!w->title[s + 1]) break;
                tab_title[d++] = w->title[s++];
                tab_title[d++] = w->title[s++];
                chars++;
            } else {
                tab_title[d++] = w->title[s++];
                chars++;
            }
        }
        tab_title[d] = '\0';
        gfx_draw_string(tab_x + 10, bar_y + 14, tab_title, COLOR_TEXT_MAIN);

        if (!w->is_minimized) {
            gfx_draw_rect(tab_x + 6, bar_y + TASKBAR_HEIGHT - 3, tab_w - 12, 2, COLOR_ACCENT_BLUE);
        }

        tab_x += tab_w + 4;
    }

    // 3. Системный трей
    int tray_x = g_screen_w - 270;
    
    // Аудио иконка в трее (подсветка синим при открытом попапе)
    uint32_t snd_col = (g_active_popup == POPUP_SOUND) ? COLOR_ACCENT_BLUE : COLOR_TEXT_MUTED;
    gfx_draw_mono_icon(tray_x, bar_y + 14, icon_sound_mute, snd_col, COLOR_WARN_RED, 1);

    // [СЕТЬ]: 3 состояния в стиле KDE Breeze
    if (g_net_info.state == NET_STATE_ONLINE) {
        // Состояние 2: Интернет подключен (чистый монитор LAN с кабелем)
        uint32_t col = (g_active_popup == POPUP_NET) ? COLOR_ACCENT_BLUE : COLOR_TEXT_MAIN;
        gfx_draw_mono_icon(tray_x + 28, bar_y + 14, icon_network_eth, col, 0, 0);
    } 
    else if (g_net_info.state == NET_STATE_LOCAL_ONLY) {
        // Состояние 1: Локальная сеть без интернета (восклицательный знак)
        gfx_draw_mono_icon(tray_x + 28, bar_y + 14, icon_network_alert, 0xFFFF55, 0, 0);
    } 
    else {
        // Состояние 0: Кабель отключен (разрыв связи)
        gfx_draw_mono_icon(tray_x + 28, bar_y + 14, icon_network_none, COLOR_TEXT_MUTED, COLOR_WARN_RED, 1);
    }

    gfx_draw_mono_icon(tray_x + 56, bar_y + 14, icon_brightness, (g_active_popup == POPUP_BRIGHTNESS) ? COLOR_ACCENT_BLUE : COLOR_TEXT_MAIN, 0, 0);

    // Раскладка
    gfx_draw_rect(tray_x + 84, bar_y + 10, 36, 24, COLOR_DOCK_ITEM_BG);
    gfx_draw_string(tray_x + 94, bar_y + 14, g_layout_ru ? "RU" : "EN", COLOR_TEXT_MAIN);

    // Часы
    uint8_t local_hour = (t->hour + g_tz_offset) % 24;
    gfx_draw_rect(tray_x + 130, bar_y + 4, 130, 36, (g_active_popup == POPUP_CLOCK) ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
    gfx_draw_dec2(tray_x + 142, bar_y + 4, local_hour, COLOR_TEXT_MAIN);
    gfx_draw_string(tray_x + 158, bar_y + 4, ":", COLOR_TEXT_MAIN);
    gfx_draw_dec2(tray_x + 166, bar_y + 4, t->min, COLOR_TEXT_MAIN);
    gfx_draw_string(tray_x + 182, bar_y + 4, ":", COLOR_TEXT_MAIN);
    gfx_draw_dec2(tray_x + 190, bar_y + 4, t->sec, COLOR_ACCENT_BLUE);

    gfx_draw_dec2(tray_x + 142, bar_y + 22, t->day, COLOR_TEXT_MUTED);
    gfx_draw_string(tray_x + 158, bar_y + 22, ".", COLOR_TEXT_MUTED);
    gfx_draw_dec2(tray_x + 166, bar_y + 22, t->month, COLOR_TEXT_MUTED);
    gfx_draw_string(tray_x + 182, bar_y + 22, ".20", COLOR_TEXT_MUTED);
    gfx_draw_dec2(tray_x + 206, bar_y + 22, t->year, COLOR_TEXT_MUTED);

    // 4. Меню "Пуск"
    if (g_menu_open) {
        int item_h = 26;
        int m_w = 230;
        int m_h = 50 + ((g_menu_app_count + 1) * item_h) + 38;
        int m_x = 6, m_y = bar_y - m_h - 4;

        gfx_draw_rect(m_x - 1, m_y - 1, m_w + 2, m_h + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(m_x, m_y, m_w, m_h, COLOR_PANEL_BG);

        // --- ШАПКА: Плейсхолдер пользователя system ---
        gfx_draw_rect(m_x, m_y, m_w, 42, 0x1B1E20);
        gfx_draw_mono_icon(m_x + 12, m_y + 13, icon_placeholder, COLOR_ACCENT_BLUE, 0, 0);
        gfx_draw_string(m_x + 36, m_y + 8, "system", COLOR_TEXT_MAIN);
        gfx_draw_string(m_x + 36, m_y + 23, "Пользователь", COLOR_TEXT_MUTED);
        gfx_draw_rect(m_x + 8, m_y + 42, m_w - 16, 1, COLOR_PANEL_BORDER);

        // Список приложений из /desprogs
        int start_apps_y = m_y + 48;
        for (int i = 0; i < g_menu_app_count; i++) {
            int iy = start_apps_y + (i * item_h);
            int hover = (mouse_x >= m_x && mouse_x <= m_x + m_w && mouse_y >= iy && mouse_y < iy + item_h);
            if (hover) gfx_draw_rect(m_x + 4, iy, m_w - 8, item_h - 2, COLOR_DOCK_HOVER);
            gfx_draw_string(m_x + 16, iy + 4, g_menu_apps[i].name, COLOR_TEXT_MAIN);
        }

        // Встроенный osver
        int osver_y = start_apps_y + (g_menu_app_count * item_h);
        int osver_h = (mouse_x >= m_x && mouse_x <= m_x + m_w && mouse_y >= osver_y && mouse_y < osver_y + item_h);
        if (osver_h) gfx_draw_rect(m_x + 4, osver_y, m_w - 8, item_h - 2, COLOR_DOCK_HOVER);
        gfx_draw_string(m_x + 16, osver_y + 4, "О системе (osver)", COLOR_TEXT_MUTED);

        // Нижняя панель питания
        int exit_y = m_y + m_h - 32;
        int btn_main_w = m_w - 38;
        int btn_arrow_w = 26;

        // Основная кнопка: Завершить работу
        int shut_h = (mouse_x >= m_x + 4 && mouse_x < m_x + 4 + btn_main_w && mouse_y >= exit_y && mouse_y <= exit_y + 26);
        gfx_draw_rect(m_x + 4, exit_y, btn_main_w, 26, shut_h ? 0x551111 : COLOR_DOCK_ITEM_BG);
        gfx_draw_string(m_x + 10, exit_y + 6, "Завершение работы", shut_h ? COLOR_WARN_RED : COLOR_TEXT_MAIN);

        // Кнопка стрелки [>]
        int arrow_x = m_x + 4 + btn_main_w + 2;
        int arrow_h = (mouse_x >= arrow_x && mouse_x <= arrow_x + btn_arrow_w && mouse_y >= exit_y && mouse_y <= exit_y + 26);
        gfx_draw_rect(arrow_x, exit_y, btn_arrow_w, 26, (arrow_h || g_power_menu_open) ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
        gfx_draw_string(arrow_x + 9, exit_y + 6, ">", COLOR_TEXT_MAIN);

        // Каскадное подменю питания
        if (g_power_menu_open) {
            int sub_x = m_x + m_w + 2;
            int sub_w = 170;
            int sub_h = 3 * 24 + 6;
            int sub_y = m_y + m_h - sub_h;

            gfx_draw_rect(sub_x - 1, sub_y - 1, sub_w + 2, sub_h + 2, COLOR_PANEL_BORDER);
            gfx_draw_rect(sub_x, sub_y, sub_w, sub_h, COLOR_PANEL_BG);

            const char* items[] = {
                "Выход в консоль",
                "Перезагрузка",
                "Выключение"
            };

            for (int k = 0; k < 3; k++) {
                int iy = sub_y + 3 + (k * 24);
                int hov = (mouse_x >= sub_x && mouse_x <= sub_x + sub_w && mouse_y >= iy && mouse_y < iy + 24);
                if (hov) gfx_draw_rect(sub_x + 2, iy, sub_w - 4, 22, (k == 0) ? 0x442222 : COLOR_DOCK_HOVER);
                gfx_draw_string(sub_x + 10, iy + 4, items[k], (k == 0) ? COLOR_WARN_RED : COLOR_TEXT_MAIN);
            }
        }
    }

    // 5. Всплывающее окно яркости
    if (g_active_popup == POPUP_BRIGHTNESS) {
        int sl_w = 210, sl_h = 68;
        int sl_x = g_screen_w - sl_w - 70;
        int sl_y = bar_y - sl_h - 6;

        gfx_draw_rect(sl_x - 1, sl_y - 1, sl_w + 2, sl_h + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(sl_x, sl_y, sl_w, sl_h, COLOR_PANEL_BG);

        gfx_draw_string(sl_x + 12, sl_y + 10, "ЯРКОСТЬ:", COLOR_TEXT_MAIN);
        gfx_draw_dec(sl_x + 110, sl_y + 10, g_brightness, COLOR_ACCENT_BLUE);
        gfx_draw_string(sl_x + (g_brightness == 100 ? 136 : 128), sl_y + 10, "%", COLOR_ACCENT_BLUE);

        int track_x = sl_x + 14, track_y = sl_y + 36, track_w = 172, track_h = 6;
        gfx_draw_rect(track_x, track_y, track_w, track_h, 0x141618);
        int fill_w = (track_w * g_brightness) / 100;
        gfx_draw_rect(track_x, track_y, fill_w, track_h, COLOR_ACCENT_BLUE);
        int knob_x = track_x + fill_w - 4;
        gfx_draw_rect(knob_x, track_y - 4, 8, 14, COLOR_TEXT_MAIN);
    } 
    // 6. Всплывающее окно часового пояса
    else if (g_active_popup == POPUP_CLOCK) {
        int cl_w = 220, cl_h = 108;
        int cl_x = g_screen_w - cl_w - 10;
        int cl_y = bar_y - cl_h - 6;

        gfx_draw_rect(cl_x - 1, cl_y - 1, cl_w + 2, cl_h + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(cl_x, cl_y, cl_w, cl_h, COLOR_PANEL_BG);
        gfx_draw_string(cl_x + 10, cl_y + 8, "ЧАСОВОЙ ПОЯС:", COLOR_ACCENT_BLUE);

        if (g_tz_offset == 0) gfx_draw_rect(cl_x + 4, cl_y + 28, cl_w - 8, 22, COLOR_DOCK_HOVER);
        gfx_draw_string(cl_x + 12, cl_y + 31, (g_tz_offset == 0) ? "[X] UTC+0 (London)" : "[ ] UTC+0 (London)", COLOR_TEXT_MAIN);

        if (g_tz_offset == 3) gfx_draw_rect(cl_x + 4, cl_y + 52, cl_w - 8, 22, COLOR_DOCK_HOVER);
        gfx_draw_string(cl_x + 12, cl_y + 55, (g_tz_offset == 3) ? "[X] MSK   (UTC+3)" : "[ ] MSK   (UTC+3)", COLOR_TEXT_MAIN);

        if (g_tz_offset == 9) gfx_draw_rect(cl_x + 4, cl_y + 76, cl_w - 8, 22, COLOR_DOCK_HOVER);
        gfx_draw_string(cl_x + 12, cl_y + 79, (g_tz_offset == 9) ? "[X] YAKT  (MSK+6)" : "[ ] YAKT  (MSK+6)", COLOR_TEXT_MAIN);
    } 
    // 7. Карточка состояния сети Ethernet
    else if (g_active_popup == POPUP_NET) {
        int pop_w = 230, pop_h = 104;
        int pop_x = g_screen_w - pop_w - 60;
        int pop_y = bar_y - pop_h - 6;

        gfx_draw_rect(pop_x - 1, pop_y - 1, pop_w + 2, pop_h + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(pop_x, pop_y, pop_w, pop_h, COLOR_PANEL_BG);

        if (g_net_info.state == NET_STATE_ONLINE) {
            gfx_draw_mono_icon(pop_x + 12, pop_y + 12, icon_network_eth, COLOR_ACCENT_BLUE, 0, 0);
            gfx_draw_string(pop_x + 34, pop_y + 8, "Ethernet (LAN)", COLOR_TEXT_MAIN);
            gfx_draw_string(pop_x + 34, pop_y + 22, "Подключено к Сети", 0x55FF55);
        } else if (g_net_info.state == NET_STATE_LOCAL_ONLY) {
            gfx_draw_mono_icon(pop_x + 12, pop_y + 12, icon_network_alert, 0xFFFF55, 0, 0);
            gfx_draw_string(pop_x + 34, pop_y + 8, "Ethernet (LAN)", COLOR_TEXT_MAIN);
            gfx_draw_string(pop_x + 34, pop_y + 22, "Без доступа в Интернет", 0xFFFF55);
        } else {
            gfx_draw_mono_icon(pop_x + 12, pop_y + 12, icon_network_none, COLOR_WARN_RED, 0, 0);
            gfx_draw_string(pop_x + 34, pop_y + 8, "Ethernet (LAN)", COLOR_TEXT_MAIN);
            gfx_draw_string(pop_x + 34, pop_y + 22, "Кабель не подключен", COLOR_WARN_RED);
        }

        gfx_draw_rect(pop_x + 10, pop_y + 38, pop_w - 20, 1, COLOR_PANEL_BORDER);

        char ip_str[24];
        if (g_net_info.state != NET_STATE_DISCONNECTED) {
            gfx_draw_string(pop_x + 12, pop_y + 46, "IP:", COLOR_TEXT_MUTED);
            int p = 0;
            for (int i = 3; i >= 0; i--) {
                uint8_t oc = (g_net_info.ip >> (i * 8)) & 0xFF;
                char b[8]; itoa(oc, b);
                for (int j = 0; b[j]; j++) ip_str[p++] = b[j];
                if (i > 0) ip_str[p++] = '.';
            }
            ip_str[p] = '\0';
            gfx_draw_string(pop_x + 58, pop_y + 46, ip_str, COLOR_TEXT_MAIN);

            gfx_draw_string(pop_x + 12, pop_y + 64, "Шлюз:", COLOR_TEXT_MUTED);
            p = 0;
            for (int i = 3; i >= 0; i--) {
                uint8_t oc = (g_net_info.gateway_ip >> (i * 8)) & 0xFF;
                char b[8]; itoa(oc, b);
                for (int j = 0; b[j]; j++) ip_str[p++] = b[j];
                if (i > 0) ip_str[p++] = '.';
            }
            ip_str[p] = '\0';
            gfx_draw_string(pop_x + 58, pop_y + 64, ip_str, COLOR_TEXT_MAIN);

            gfx_draw_string(pop_x + 12, pop_y + 82, "Канал:", COLOR_TEXT_MUTED);
            char sp_str[16]; itoa((int)g_net_info.link_speed, sp_str);
            int sp_len = 0; while (sp_str[sp_len]) sp_len++;
            sp_str[sp_len++] = ' '; sp_str[sp_len++] = 'M'; sp_str[sp_len++] = 'b'; sp_str[sp_len++] = 'p'; sp_str[sp_len++] = 's'; sp_str[sp_len] = '\0';
            gfx_draw_string(pop_x + 58, pop_y + 82, sp_str, 0x55FFFF);
        } else {
            gfx_draw_string(pop_x + 12, pop_y + 54, "Статус:", COLOR_TEXT_MUTED);
            gfx_draw_string(pop_x + 80, pop_y + 54, "Соединения нету", COLOR_TEXT_MUTED);
        }
    } 
    // 8. Полноценное окно аудио
    else if (g_active_popup == POPUP_SOUND) {
        int pop_w = 220, pop_h = 56;
        int pop_x = g_screen_w - pop_w - 60;
        int pop_y = bar_y - pop_h - 6;

        gfx_draw_rect(pop_x - 1, pop_y - 1, pop_w + 2, pop_h + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(pop_x, pop_y, pop_w, pop_h, COLOR_PANEL_BG);

        gfx_draw_string(pop_x + 12, pop_y + 10, "АУДИО", COLOR_WARN_RED);
        gfx_draw_string(pop_x + 12, pop_y + 30, "Драйвер не найден", COLOR_TEXT_MUTED);
    }
}

int taskbar_handle_click(int mx, int my) {
    int bar_y = g_screen_h - TASKBAR_HEIGHT;

    // 1. Повторный клик по кнопке [overOS] (Пуск) — ТОГГЛ (закрыть, если открыто)
    if (mx >= 6 && mx <= 90 && my >= bar_y + 5) {
        if (g_menu_open) {
            g_menu_open = 0; // Закрываем при повторном клике!
        } else {
            taskbar_refresh_menu_apps();
            g_menu_open = 1;
            g_active_popup = POPUP_NONE; // Закрываем любые другие попапы
        }
        return 1;
    }

    // Клики по меню "Пуск"
    if (g_menu_open) {
        int item_h = 26;
        int m_w = 230;
        int m_h = 50 + ((g_menu_app_count + 1) * item_h) + 38;
        int m_x = 6, m_y = bar_y - m_h - 4;

        // Клики внутри каскадного подменю
        if (g_power_menu_open) {
            int sub_x = m_x + m_w + 2;
            int sub_w = 170;
            int sub_h = 3 * 24 + 6;
            int sub_y = m_y + m_h - sub_h;

            if (mx >= sub_x && mx <= sub_x + sub_w && my >= sub_y && my <= sub_y + sub_h) {
                int clicked_item = (my - (sub_y + 3)) / 24;
                if (clicked_item == 0) { // Выход в консоль
                    sys_exit(0);
                } else if (clicked_item == 1) { // Перезагрузка
                    sys_system_reboot();
                } else if (clicked_item == 2) { // Выключение
                    sys_system_shutdown();
                }
                g_menu_open = 0;
                g_power_menu_open = 0;
                return 1;
            }
        }

        // Клики внутри основного окна меню
        if (mx >= m_x && mx <= m_x + m_w) {
            int start_apps_y = m_y + 48;
            for (int i = 0; i < g_menu_app_count; i++) {
                int iy = start_apps_y + (i * item_h);
                if (my >= iy && my < iy + item_h) {
                    launch_file_safe(g_menu_apps[i].path);
                    g_menu_open = 0;
                    g_power_menu_open = 0;
                    return 1;
                }
            }

            int osver_y = start_apps_y + (g_menu_app_count * item_h);
            if (my >= osver_y && my < osver_y + item_h) {
                osver_show();
                g_menu_open = 0;
                g_power_menu_open = 0;
                return 1;
            }

            // Клик по кнопкам питания
            int exit_y = m_y + m_h - 32;
            int btn_main_w = m_w - 38;
            int arrow_x = m_x + 4 + btn_main_w + 2;

            // Клик по стрелочке [>]
            if (mx >= arrow_x && mx <= arrow_x + 26 && my >= exit_y && my <= exit_y + 26) {
                g_power_menu_open = !g_power_menu_open;
                return 1;
            }

            // Клик по основной кнопке "Завершение работы"
            if (mx >= m_x + 4 && mx < arrow_x && my >= exit_y && my <= exit_y + 26) {
                sys_system_shutdown();
                return 1;
            }
        }
        return 1;
    }

    // 3. Вкладки запущенных окон на таскбаре
    int win_cnt = 0;
    window_t* wins = window_get_list(&win_cnt);
    int tab_x = 98;
    int tab_w = 140;

    for (int i = 0; i < win_cnt; i++) {
        if (!wins[i].is_open) continue;
        if (mx >= tab_x && mx < tab_x + tab_w && my >= bar_y + 5) {
            wins[i].is_minimized = !wins[i].is_minimized;
            if (!wins[i].is_minimized) {
                for (int k = 0; k < win_cnt; k++) wins[k].is_active = 0;
                wins[i].is_active = 1;
            }
            g_menu_open = 0;
            g_active_popup = POPUP_NONE;
            return 1;
        }
        tab_x += tab_w + 4;
    }

    // 4. Трей — переключение окон
    int tray_x = g_screen_w - 270;
    if (mx >= tray_x && mx < tray_x + 24 && my >= bar_y + 10) {
        g_menu_open = 0;
        g_active_popup = (g_active_popup == POPUP_SOUND) ? POPUP_NONE : POPUP_SOUND;
        return 1;
    } else if (mx >= tray_x + 28 && mx < tray_x + 52 && my >= bar_y + 10) {
        g_menu_open = 0;
        g_active_popup = (g_active_popup == POPUP_NET) ? POPUP_NONE : POPUP_NET;
        return 1;
    } else if (mx >= tray_x + 56 && mx < tray_x + 80 && my >= bar_y + 10) {
        g_menu_open = 0;
        g_active_popup = (g_active_popup == POPUP_BRIGHTNESS) ? POPUP_NONE : POPUP_BRIGHTNESS;
        return 1;
    } else if (mx >= tray_x + 84 && mx < tray_x + 120 && my >= bar_y + 10) {
        g_layout_ru = !g_layout_ru;
        settings_save();
        g_menu_open = 0;
        g_active_popup = POPUP_NONE;
        return 1;
    } else if (mx >= tray_x + 130 && mx < tray_x + 260 && my >= bar_y + 4) {
        g_menu_open = 0;
        g_active_popup = (g_active_popup == POPUP_CLOCK) ? POPUP_NONE : POPUP_CLOCK;
        return 1;
    }

    // Клик по выбору часового пояса внутри попапа часов
    if (g_active_popup == POPUP_CLOCK) {
        int cl_w = 220, cl_h = 108;
        int cl_x = g_screen_w - cl_w - 10;
        int cl_y = bar_y - cl_h - 6;

        if (mx >= cl_x && mx <= cl_x + cl_w) {
            if (my >= cl_y + 28 && my < cl_y + 50) { g_tz_offset = 0; settings_save(); return 1; }
            else if (my >= cl_y + 52 && my < cl_y + 74) { g_tz_offset = 3; settings_save(); return 1; }
            else if (my >= cl_y + 76 && my < cl_y + 98) { g_tz_offset = 9; settings_save(); return 1; }
        }
    }

    return 0;
}