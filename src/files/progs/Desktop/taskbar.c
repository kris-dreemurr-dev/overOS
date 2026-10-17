#include "appinfo.h"
#include "taskbar.h"
#include "window.h"
#include "osver.h"
#include "gfx.h"
#include "theme.h"
#include "settings.h"
#include "icons.h"
#include "greeter.h"
#include "wproto.h"
#include "ovos.h"

int        g_active_popup = POPUP_NONE;
uint32_t   g_popup_timer = 0;
int        g_menu_open = 0;
int        g_brightness_dragging = 0;
menu_app_t g_menu_apps[MAX_MENU_APPS];
int        g_menu_app_count = 0;
int        g_power_menu_open = 0;
int        g_logout_requested = 0;
int        g_net_driver_ok = 0;
int g_volume_dragging = 0;

// --- Часовые пояса: полный список берём из ядра (syscall 97), выбранный лежит
// в /etc/timezone. Раньше здесь были три зашитых города и часы игнорировали /etc.
#define TB_TZ_MAX   40
#define TB_TZ_ROWS  7
#define TB_TZ_ROW_H 24
#define TB_TZ_W     300
#define TB_TZ_H     (34 + TB_TZ_ROWS * TB_TZ_ROW_H + 8)
#define TB_TZ_SB_W  16

static tz_info_t s_tz[TB_TZ_MAX];
static int s_tz_count  = 0;
static int s_tz_loaded = 0;
static int s_tz_top    = 0;
static int s_tz_drag   = 0;      // тянем бегунок полосы прокрутки
static int s_tz_drag_off = 0;

static void tz_clamp(void) {
    int max = s_tz_count - TB_TZ_ROWS;
    if (max < 0) max = 0;
    if (s_tz_top > max) s_tz_top = max;
    if (s_tz_top < 0) s_tz_top = 0;
}

static void tz_load(void) {
    if (s_tz_loaded) return;
    s_tz_loaded = 1;
    s_tz_count = sys_tz_info(-1, 0);
    if (s_tz_count < 0) s_tz_count = 0;
    if (s_tz_count > TB_TZ_MAX) s_tz_count = TB_TZ_MAX;
    for (int i = 0; i < s_tz_count; i++) sys_tz_info(i, &s_tz[i]);
}

// Открыли попап — показываем страницу с текущим поясом
static void tz_popup_opened(void) {
    tz_load();
    s_tz_drag = 0;
    s_tz_top = (g_tz_index >= 0) ? g_tz_index - TB_TZ_ROWS / 2 : 0;
    tz_clamp();
}

static void tz_geom(int* x, int* y) {
    int bar_y = g_screen_h - TASKBAR_HEIGHT;
    *x = g_screen_w - TB_TZ_W - 10;
    *y = bar_y - TB_TZ_H - 6;
}

static int tz_knob_h(void) {
    if (s_tz_count <= TB_TZ_ROWS) return TB_TZ_ROWS * TB_TZ_ROW_H;
    int h = (TB_TZ_ROWS * TB_TZ_ROW_H) * TB_TZ_ROWS / s_tz_count;
    return (h < 24) ? 24 : h;
}

static int tz_knob_y(int list_y) {
    if (s_tz_count <= TB_TZ_ROWS) return list_y;
    int span = TB_TZ_ROWS * TB_TZ_ROW_H - tz_knob_h();
    return list_y + span * s_tz_top / (s_tz_count - TB_TZ_ROWS);
}

static window_t* s_err_win = 0;
static char      s_err_msg[512] = "";

sys_net_info_t g_net_info = {0};
static uint8_t s_last_net_check_sec = 255;

sys_hda_outputs_t g_audio_outputs = {0};
int               g_audio_status  = HDA_STATUS_NOT_FOUND;
static uint8_t    s_last_audio_check_sec = 255;
static int        g_audio_target  = 0;   // 0 никуда, 1 аналог, 2 HDMI (сисколл 70/17)
static int        s_tone_playing  = 0;
static int        s_tone_frames   = 0; // обратный отсчет кадров для 1 сек звука
static int        s_audio_boot_applied = 0;

// Длительность тестового тона при выборе устройства вывода (мс). Тон НЕ блокирует десктоп:
// запускается сразу, а гасится из taskbar_render() по таймеру TSC.
// Если в QEMU 200 мс слышно слабо (бэкенд копит буфер перед выводом), подними до 300-400: зависания не будет.
#define TONE_TEST_MS 200

static uint64_t s_tsc_per_ms   = 0;
static uint64_t s_tone_stop_at = 0;      // TSC, когда погасить тон; 0 = тон не ждёт остановки
static int      s_muted        = 0;      // локальное состояние кнопки Mute в попапе
static int      s_vol_applied  = -1;     // какую громкость последний раз отправили в драйвер

static inline uint64_t tb_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Запустить короткий тон на текущем выходе без блокировки интерфейса.
static void tone_start(void) {
    // Тон теперь -- обычный голос микшера, он подмешивается к чужому звуку.
    // Раньше здесь стоял отказ, если драйвер уже занят, и при открытом видео
    // выбор устройства вывода не давал вообще никакого звука.
    if (!s_tsc_per_ms) s_tsc_per_ms = sys_get_tsc_per_ms();
    sys_audio_play_test();
    if (s_tsc_per_ms) {
        s_tone_stop_at = tb_rdtsc() + (uint64_t)TONE_TEST_MS * s_tsc_per_ms;
    } else {                              // TSC не откалиброван: запасной вариант с короткой паузой
        sys_sleep(TONE_TEST_MS);
        sys_audio_stop();
    }
}

// Вызывать каждый кадр: гасит тон по истечении времени.
static void tone_tick(void) {
    if (s_tone_stop_at && tb_rdtsc() >= s_tone_stop_at) {
        s_tone_stop_at = 0;
        sys_audio_stop();
    }
}

// Любое изменение g_volume (клик, перетаскивание в desktop-модуле, настройки) уходит в драйвер.
static void volume_sync(void) {
    if (g_audio_status == HDA_STATUS_NOT_FOUND) return;
    if (g_volume != s_vol_applied) {
        s_vol_applied = g_volume;
        sys_audio_set_volume(g_volume);
    }
}

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
        window_activate(s_err_win);   // активное + поверх всех окон, а не где-то сзади
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

    // 1. Защита от дурака: блокируем повторный запуск desktop.prg
    const char* base_fn = filepath;
    for (int i = 0; filepath[i]; i++) {
        if (filepath[i] == '/') base_fn = &filepath[i + 1];
    }
    const char* dt = "desktop.prg";
    int is_dt = 1;
    for (int i = 0; dt[i] || base_fn[i]; i++) {
        char c1 = base_fn[i], c2 = dt[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) { is_dt = 0; break; }
    }
    if (is_dt) {
        show_error_dialog("file cant be executed");
        return -1;
    }

    // 2. Блокировка .sys файлов
    if (len > 4 && (filepath[len-4] == '.' && 
       (filepath[len-3] == 's' || filepath[len-3] == 'S') &&
       (filepath[len-2] == 'y' || filepath[len-2] == 'Y') &&
       (filepath[len-1] == 's' || filepath[len-1] == 'S'))) {
        show_error_dialog("file cant be executed");
        return -1;
    }

    // 3. Открытие файла и чтение заголовка DPRG
    int fd = sys_open(filepath, 0);
    if (fd < 0) {
        show_error_dialog("file cant be executed");
        return -1;
    }

    uint8_t hdr_buf[64];
    int rd = sys_read(fd, hdr_buf, 64);
    sys_close(fd);

    if (rd < 64 || hdr_buf[0] != 'D' || hdr_buf[1] != 'P' || hdr_buf[2] != 'R' || hdr_buf[3] != 'G') {
        show_error_dialog("file cant be executed");
        return -2;
    }

    uint64_t flags = *(uint64_t*)&hdr_buf[48];
    if (!(flags & 0x01)) { // Флаг PRG_FLAG_DESKTOP отсутствует
        show_error_dialog("file cant be executed");
        return -2;
    }

    // 4. Запуск от имени пользователя сеанса — spawn, без fork: раньше desktop копировал
    // всю свою память (буферы экрана и обоев, десятки МБ) ради одного execve
    int pid = sys_spawn_as_session(filepath, 0);
    if (pid < 0) { show_error_dialog(pid == -126 ? "Permission denied" : "file cant be executed"); return -3; }
    return 0;
}

// Проверка попадания клика внутрь меню Пуск
int taskbar_is_inside_menu(int mx, int my) {
    if (!g_menu_open) return 0;
    int bar_y = g_screen_h - TASKBAR_HEIGHT;
    int item_h = 26;
    int m_w = 262;   // хватает самому длинному названию («Параметры системы»)
    int m_h = 50 + ((g_menu_app_count + 1) * item_h) + 38;
    int m_x = 6, m_y = bar_y - m_h - 4;

    // Проверяем само меню
    if (mx >= m_x && mx <= m_x + m_w && my >= m_y && my <= m_y + m_h) return 1;

    // Проверяем выпадающее подменю питания
    if (g_power_menu_open) {
        int sub_x = m_x + m_w + 2;
        int sub_w = 170;
        int sub_h = 4 * 24 + 4;
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
        int cl_x, cl_y;
        tz_geom(&cl_x, &cl_y);
        return (mx >= cl_x && mx <= cl_x + TB_TZ_W && my >= cl_y && my <= cl_y + TB_TZ_H);
    } else if (g_active_popup == POPUP_NET) {
        int pop_w = 230, pop_h = 104;
        int pop_x = g_screen_w - pop_w - 60, pop_y = bar_y - pop_h - 6;
        return (mx >= pop_x && mx <= pop_x + pop_w && my >= pop_y && my <= pop_y + pop_h);
    } else if (g_active_popup == POPUP_SOUND) {
        int pop_w = 260, pop_h = 214;
        int pop_x = g_screen_w - pop_w - 60, pop_y = bar_y - pop_h - 6;
        return (mx >= pop_x && mx <= pop_x + pop_w && my >= pop_y && my <= pop_y + pop_h);
    }
    return 0;
}

static int is_prg(const char* fn) {
    int len = 0;
    while (fn[len]) len++;
    return len > 4 && fn[len - 4] == '.' &&
           (fn[len - 3] == 'p' || fn[len - 3] == 'P') &&
           (fn[len - 2] == 'r' || fn[len - 2] == 'R') &&
           (fn[len - 1] == 'g' || fn[len - 1] == 'G');
}

// Пункт меню: имя -- из ресурсов программы (NAME), как Name= в .desktop
static void menu_add_app(const char* full) {
    if (g_menu_app_count >= MAX_MENU_APPS) return;
    menu_app_t* a = &g_menu_apps[g_menu_app_count++];
    ov_strlcpy(a->name, appinfo(full)->name, sizeof(a->name));
    ov_strlcpy(a->path, full, sizeof(a->path));
}

// Программы рабочего стола -- /desprogs. Два вида:
//   * файл /desprogs/x.prg -- программа сама по себе;
//   * папка /desprogs/Игра/ с .prg внутри -- программа со своими файлами
//     (библиотеки, resources/), как .app на macOS: всё нужное лежит рядом,
//     а программа находит папку по пути своего исполняемого файла.
void taskbar_refresh_menu_apps(void) {
    g_menu_app_count = 0;
    static sys_file_info_t files[32];
    static sys_file_info_t inner[32];
    int count = sys_get_dir_files("/desprogs", files, 32);

    for (int i = 0; i < count && g_menu_app_count < MAX_MENU_APPS; i++) {
        const char* fn = files[i].name;
        char full[96];
        ov_strlcpy(full, "/desprogs/", sizeof(full));
        int fl = 0;
        while (full[fl]) fl++;
        for (int q = 0; fn[q] && fl < (int)sizeof(full) - 1; q++) full[fl++] = fn[q];
        full[fl] = '\0';

        if (!(files[i].attr & 0x10)) {
            if (is_prg(fn)) menu_add_app(full);
            continue;
        }
        if (fn[0] == '.') continue;                 // "." и ".."
        int n = sys_get_dir_files(full, inner, 32);
        for (int k = 0; k < n; k++) {
            if ((inner[k].attr & 0x10) || !is_prg(inner[k].name)) continue;
            char app[96];
            ov_strlcpy(app, full, sizeof(app));
            int al = 0;
            while (app[al]) al++;
            if (al < (int)sizeof(app) - 1) app[al++] = '/';
            for (int q = 0; inner[k].name[q] && al < (int)sizeof(app) - 1; q++) app[al++] = inner[k].name[q];
            app[al] = '\0';
            menu_add_app(app);
            break;                                  // одна программа на папку
        }
    }
}


// =============================================================================
// Док панели задач: значки закреплённых и запущенных программ (как в Plasma).
// Закреплённые хранятся в desktop.json (g_pinned, пути через ';'), запущенные
// подтягиваются по классу программы (wp_create_t.app), поэтому у работающего
// приложения не появляется второй значок рядом с ярлыком.
// =============================================================================
#define DOCK_X       56      // слева только кнопка меню приложений
#define DOCK_ITEM_W  40
#define DOCK_ICON_Y  14
#define MAX_DOCK     16

typedef struct {
    char path[64];       // чем запускать (пусто — программа не закреплена)
    char app[24];        // класс программы
    char label[64];      // подпись всплывающей подсказки (UTF-8: русская буква — два байта)
    int  win;            // индекс окна в списке окон (-1 — не запущена)
    int  pinned;
} dock_item_t;

static dock_item_t s_dock[MAX_DOCK];
static int         s_dock_count = 0;

// Две цифры в строку (часы, минуты, дата)
static void fmt2(char* d, int v) { d[0] = (char)('0' + (v / 10) % 10); d[1] = (char)('0' + v % 10); }

static void dock_copy(char* dst, int cap, const char* src, int len) {
    int i = 0;
    while (i < len && i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Собрать список значков: сначала закреплённые по порядку, потом остальные окна
static void dock_build(void) {
    s_dock_count = 0;
    const char* p = g_pinned;
    while (*p && s_dock_count < MAX_DOCK) {
        const char* start = p;
        while (*p && *p != ';') p++;
        int len = (int)(p - start);
        if (*p == ';') p++;
        if (len <= 0) continue;
        dock_item_t* d = &s_dock[s_dock_count++];
        dock_copy(d->path, sizeof(d->path), start, len);
        const appinfo_t* ai = appinfo(d->path);              // класс и имя -- из самой программы
        ov_strlcpy(d->app, ai->app, sizeof(d->app));
        ov_strlcpy(d->label, ai->name, sizeof(d->label));
        d->win = -1;
        d->pinned = 1;
    }

    int win_cnt = 0;
    window_t* wins = window_get_list(&win_cnt);
    for (int i = 0; i < win_cnt; i++) {
        window_t* w = &wins[i];
        if (!w->is_open) continue;
        // Окно -- к своему ярлыку: по исполняемому файлу, а если он другой
        // (запустили копию из другой папки) -- по классу программы
        int found = -1;
        for (int k = 0; k < s_dock_count && found < 0; k++)
            if (s_dock[k].win < 0 && w->exe[0] && ov_streq(s_dock[k].path, w->exe)) found = k;
        if (found < 0 && w->app[0]) {
            for (int k = 0; k < s_dock_count; k++)
                if (s_dock[k].win < 0 && ov_streq(s_dock[k].app, w->app)) { found = k; break; }
        }
        if (found >= 0) {
            s_dock[found].win = i;
            dock_copy(s_dock[found].label, sizeof(s_dock[found].label), w->title, (int)sizeof(s_dock[found].label) - 1);
        } else if (s_dock_count < MAX_DOCK) {
            dock_item_t* d = &s_dock[s_dock_count++];
            d->path[0] = '\0';
            dock_copy(d->app, sizeof(d->app), w->app, (int)sizeof(d->app) - 1);
            dock_copy(d->label, sizeof(d->label), w->title, (int)sizeof(d->label) - 1);
            d->win = i;
            d->pinned = 0;
        }
    }
}

// Значок дока под курсором (-1 — мимо)
static int dock_at(int mx, int my) {
    int bar_y = (int)g_screen_h - TASKBAR_HEIGHT;
    if (my < bar_y + 4) return -1;
    dock_build();
    for (int i = 0; i < s_dock_count; i++) {
        int x = DOCK_X + i * DOCK_ITEM_W;
        if (mx >= x && mx < x + DOCK_ITEM_W) return i;
    }
    return -1;
}

// Закрепить/открепить программу: правим строку g_pinned и сохраняем конфиг
static void dock_pin(const char* path, int on) {
    char out[sizeof(g_pinned)];
    int o = 0;
    const char* p = g_pinned;
    int seen = 0;
    while (*p) {
        const char* start = p;
        while (*p && *p != ';') p++;
        int len = (int)(p - start);
        if (*p == ';') p++;
        if (len <= 0) continue;
        int same = 1;
        for (int i = 0; i < len; i++) if (start[i] != path[i]) { same = 0; break; }
        if (same && path[len] != '\0') same = 0;
        if (same) { seen = 1; if (!on) continue; }
        if (o + len + 1 >= (int)sizeof(out)) break;
        if (o) out[o++] = ';';
        for (int i = 0; i < len; i++) out[o++] = start[i];
    }
    if (on && !seen) {
        int len = 0; while (path[len]) len++;
        if (o + len + 1 < (int)sizeof(out)) {
            if (o) out[o++] = ';';
            for (int i = 0; i < len; i++) out[o++] = path[i];
        }
    }
    out[o] = '\0';
    ov_strlcpy(g_pinned, out, sizeof(g_pinned));
    settings_save();
}

desktop_ctx_menu_t g_ctx_menu = {0};

void desktop_ctx_open(int mx, int my, int type, int win_id) {
    g_ctx_menu.active = 1;
    g_ctx_menu.type = type;
    g_ctx_menu.x = mx;
    g_ctx_menu.y = my;
    if (type == CTX_TYPE_EXPLORER)      { g_ctx_menu.w = 190; g_ctx_menu.h = 62; }
    else if (type == CTX_TYPE_TASKBAR)  { g_ctx_menu.w = 230; g_ctx_menu.h = 62; }
    else                                 { g_ctx_menu.w = 160; g_ctx_menu.h = 36; }
    g_ctx_menu.target_win_id = win_id;

    // Корректировка, чтобы меню не уходило за границы экрана
    if (g_ctx_menu.x + g_ctx_menu.w > (int)g_screen_w) {
        g_ctx_menu.x = g_screen_w - g_ctx_menu.w - 4;
    }
    if (g_ctx_menu.y + g_ctx_menu.h > (int)(g_screen_h - TASKBAR_HEIGHT)) {
        g_ctx_menu.y = g_screen_h - TASKBAR_HEIGHT - g_ctx_menu.h - 4;
    }
}

// ПКМ по панели задач: меню значка (закрепить/открепить/закрыть)
int taskbar_ctx_at(int mx, int my) {
    int i = dock_at(mx, my);
    if (i < 0) return 0;
    desktop_ctx_open(mx, my, CTX_TYPE_TASKBAR, -1);
    g_ctx_menu.dock_item = i;
    return 1;
}

void desktop_ctx_close(void) {
    g_ctx_menu.active = 0;
}

void desktop_ctx_render(int mouse_x, int mouse_y) {
    if (!g_ctx_menu.active) return;

    int mx = g_ctx_menu.x;
    int my = g_ctx_menu.y;
    int mw = g_ctx_menu.w;
    int mh = g_ctx_menu.h;

    // Фон и фирменная вишневая рамка как на референсе CachyOS
    gfx_round_rect(mx - 1, my - 1, mw + 2, mh + 2, 7, GFX_CORNER_ALL, 0x8A2234);
    gfx_round_rect(mx, my, mw, mh, 6, GFX_CORNER_ALL, 0x1E2227);

    int item_h = 26;

    if (g_ctx_menu.type == CTX_TYPE_EXPLORER) {
        // Пункт 1: [+] Папку...
        int hov1 = (mouse_x >= mx + 2 && mouse_x <= mx + mw - 2 && mouse_y >= my + 4 && mouse_y < my + 4 + item_h);
        if (hov1) gfx_draw_rect(mx + 2, my + 4, mw - 4, item_h, 0x3A2027);
        gfx_draw_string(mx + 10, my + 8, "[+] Папку...", hov1 ? 0xFFFFFF : COLOR_TEXT_MAIN);

        // Разделитель
        gfx_draw_rect(mx + 8, my + 31, mw - 16, 1, 0x2E343B);

        // Пункт 2: [+] Текстовый файл...
        int hov2 = (mouse_x >= mx + 2 && mouse_x <= mx + mw - 2 && mouse_y >= my + 33 && mouse_y < my + 33 + item_h);
        if (hov2) gfx_draw_rect(mx + 2, my + 33, mw - 4, item_h, 0x3A2027);
        gfx_draw_string(mx + 10, my + 37, "[+] Текстовый файл...", hov2 ? 0xFFFFFF : COLOR_TEXT_MAIN);
    } else if (g_ctx_menu.type == CTX_TYPE_TASKBAR) {
        // Меню значка в панели: закрепить/открепить и закрыть окно
        int i = g_ctx_menu.dock_item;
        int pinned = (i >= 0 && i < s_dock_count) ? s_dock[i].pinned : 0;
        int running = (i >= 0 && i < s_dock_count) ? (s_dock[i].win >= 0) : 0;

        int hov1 = (mouse_x >= mx + 2 && mouse_x <= mx + mw - 2 && mouse_y >= my + 4 && mouse_y < my + 4 + item_h);
        if (hov1) gfx_draw_rect(mx + 2, my + 4, mw - 4, item_h, COLOR_DOCK_HOVER);
        gfx_draw_string(mx + 10, my + 8, pinned ? "Открепить от панели" : "Закрепить в панели",
                        hov1 ? 0xFFFFFF : COLOR_TEXT_MAIN);

        gfx_draw_rect(mx + 8, my + 31, mw - 16, 1, 0x2E343B);

        int hov2 = (mouse_x >= mx + 2 && mouse_x <= mx + mw - 2 && mouse_y >= my + 33 && mouse_y < my + 33 + item_h);
        if (hov2 && running) gfx_draw_rect(mx + 2, my + 33, mw - 4, item_h, COLOR_DOCK_HOVER);
        gfx_draw_string(mx + 10, my + 37, "Закрыть окно",
                        running ? (hov2 ? 0xFFFFFF : COLOR_TEXT_MAIN) : COLOR_TEXT_MUTED);
    } else {
        // Контекстное меню рабочего стола
        int hov = (mouse_x >= mx + 2 && mouse_x <= mx + mw - 2 && mouse_y >= my + 4 && mouse_y < my + 4 + item_h);
        if (hov) gfx_draw_rect(mx + 2, my + 4, mw - 4, item_h, COLOR_DOCK_HOVER);
        gfx_draw_string(mx + 10, my + 9, "Контекстное меню", hov ? 0xFFFFFF : COLOR_TEXT_MAIN);
    }
}

int desktop_ctx_handle_click(int mx, int my) {
    if (!g_ctx_menu.active) return 0;

    int cx = g_ctx_menu.x;
    int cy = g_ctx_menu.y;
    int cw = g_ctx_menu.w;
    int ch = g_ctx_menu.h;

    // Клик мимо закрывает меню
    if (mx < cx || mx > cx + cw || my < cy || my > cy + ch) {
        desktop_ctx_close();
        return 1;
    }

    if (g_ctx_menu.type == CTX_TYPE_TASKBAR) {
        int i = g_ctx_menu.dock_item;
        if (i >= 0 && i < s_dock_count) {
            if (my >= cy + 4 && my < cy + 30) {
                // Закрепить можно только то, что запускается файлом: у окна без
                // ярлыка путь берём по классу программы из /desprogs
                char path[64];
                if (s_dock[i].path[0]) {
                    ov_strlcpy(path, s_dock[i].path, sizeof(path));
                } else {
                    int p = 0;
                    const char* pre = "/desprogs/";
                    while (*pre) path[p++] = *pre++;
                    for (int k = 0; s_dock[i].app[k] && p < (int)sizeof(path) - 5; k++) path[p++] = s_dock[i].app[k];
                    const char* ext = ".prg";
                    while (*ext) path[p++] = *ext++;
                    path[p] = '\0';
                }
                dock_pin(path, !s_dock[i].pinned);
            } else if (my >= cy + 31 && my <= cy + ch && s_dock[i].win >= 0) {
                window_t* w = &window_get_list(0)[s_dock[i].win];
                window_request_close(w);
            }
        }
        desktop_ctx_close();
        return 1;
    }

    if (g_ctx_menu.type == CTX_TYPE_EXPLORER && g_ctx_menu.target_win_id > 0) {
        if (my >= cy + 4 && my < cy + 30) {
            window_send_command(g_ctx_menu.target_win_id, WP_CMD_CREATE_FOLDER);   // Создать папку
        } else if (my >= cy + 31 && my <= cy + ch) {
            window_send_command(g_ctx_menu.target_win_id, WP_CMD_CREATE_FILE);     // Создать файл
        }
    }

    desktop_ctx_close();
    return 1;
}

void taskbar_render(int mouse_x, int mouse_y, const rtc_time_t* t) {
    // Периодический опрос сети раз в 5 секунд
    if ((t->sec % 5 == 0) && (t->sec != s_last_net_check_sec)) {
        s_last_net_check_sec = t->sec;
        int res = sys_get_net_info(&g_net_info);
        g_net_driver_ok = (res >= 0);
    }

    tone_tick();
    volume_sync();

    // Опрос аудио-разъёмов -- операция с кодеком: на каждый пин идёт триггер
    // Presence Detect с миллисекундной паузой, и всё это внутри сисколла, то
    // есть композитор в это время стоит. Раз в четыре секунды -- и только так:
    // при открытом окошке звука опрос раз в секунду давал заметный рывок
    // интерфейса ровно тогда, когда человек тянет ползунок громкости.
    // Свежие данные при открытии окошка берутся в taskbar_handle_click.
    int audio_due = (t->sec != s_last_audio_check_sec) && ((t->sec & 3) == 0);
    if (audio_due) {
        s_last_audio_check_sec = t->sec;
        g_audio_status = sys_audio_get_status();
        if (g_audio_status != HDA_STATUS_NOT_FOUND) {
            sys_audio_jack_poll();
            sys_audio_get_outputs(&g_audio_outputs);
            g_audio_target = sys_audio_get_target();
            if (!s_audio_boot_applied) {
                sys_audio_set_output(g_audio_output);
                s_audio_boot_applied = 1;
            }
        }
    }

    int bar_y = g_screen_h - TASKBAR_HEIGHT;

    gfx_draw_rect(0, bar_y, g_screen_w, 1, COLOR_PANEL_BORDER);
    gfx_draw_rect(0, bar_y + 1, g_screen_w, TASKBAR_HEIGHT - 1, COLOR_PANEL_BG);

    // 1. Кнопка overOS
    int app_btn_w = DOCK_X - 12;   // кнопка меню приложений: только значок, как в Plasma
    int app_hover = (mouse_x >= 6 && mouse_x <= 6 + app_btn_w && mouse_y >= bar_y + 4);
    uint32_t btn_bg = g_menu_open ? COLOR_ACCENT_BLUE : (app_hover ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
    gfx_round_rect(6, bar_y + 4, app_btn_w, TASKBAR_HEIGHT - 8, 6, GFX_CORNER_ALL, btn_bg);
    gfx_icon_draw(6 + (app_btn_w - 24) / 2, bar_y + (TASKBAR_HEIGHT - 24) / 2, "launcher", 24,
                  g_menu_open ? 0x101010 : COLOR_TEXT_MAIN);

    // 2. Док: значки закреплённых и запущенных программ
    dock_build();
    int dock_limit = (int)g_screen_w - 280 - (g_caps_on ? 28 : 0);
    int tip_i = -1, tip_x = 0;
    for (int i = 0; i < s_dock_count; i++) {
        int x = DOCK_X + i * DOCK_ITEM_W;
        if (x + DOCK_ITEM_W > dock_limit) break;
        window_t* win = (s_dock[i].win >= 0) ? &window_get_list(0)[s_dock[i].win] : 0;
        int hover = (mouse_x >= x && mouse_x < x + DOCK_ITEM_W && mouse_y >= bar_y + 4);
        int active = win && win->is_active && !win->is_minimized;

        if (hover || active) {
            gfx_round_rect(x + 2, bar_y + 4, DOCK_ITEM_W - 4, TASKBAR_HEIGHT - 10, 5, GFX_CORNER_ALL,
                           hover ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
        }
        uint32_t tint = (win && !win->is_minimized) ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED;
        // Значок -- из ресурсов программы: у ярлыка по его пути, у окна без
        // ярлыка -- по исполняемому файлу владельца
        const char* svg = s_dock[i].path[0] ? s_dock[i].path : (win && win->exe[0] ? win->exe : 0);
        int icon_x = x + (DOCK_ITEM_W - 24) / 2, icon_y = bar_y + (TASKBAR_HEIGHT - 24) / 2;
        if (svg && gfx_icon(svg, 24) >= 0) {
            gfx_icon_draw(icon_x, icon_y, svg, 24, tint);
        } else {
            // В программе нет значка — рисуем тот, что прислало её окно
            const uint16_t* ico = (win && win->has_icon && win->icon) ? win->icon : icon_placeholder;
            gfx_draw_mono_icon(x + (DOCK_ITEM_W - 16) / 2, bar_y + DOCK_ICON_Y, ico, tint, 0, 0);
        }

        // Запущенная программа отмечена полоской снизу, как в Plasma
        if (win) {
            int mark_w = win->is_minimized ? 6 : DOCK_ITEM_W - 16;
            gfx_draw_rect(x + (DOCK_ITEM_W - mark_w) / 2, bar_y + TASKBAR_HEIGHT - 4, mark_w, 2,
                          active ? COLOR_ACCENT_BLUE : COLOR_TEXT_MUTED);
        }
        if (hover) { tip_i = i; tip_x = x; }
    }

    // Подсказка с названием: значки без подписей, иначе непонятно, что где
    if (tip_i >= 0 && s_dock[tip_i].label[0]) {
        const char* lbl = s_dock[tip_i].label;
        int tw = gfx_text_width(lbl) + 16;
        int tx = tip_x + DOCK_ITEM_W / 2 - tw / 2;
        if (tx < 4) tx = 4;
        if (tx + tw > (int)g_screen_w - 4) tx = (int)g_screen_w - 4 - tw;
        int ty = bar_y - 28;
        gfx_round_rect(tx, ty, tw, 24, 5, GFX_CORNER_ALL, COLOR_PANEL_BORDER);
        gfx_round_rect(tx + 1, ty + 1, tw - 2, 22, 4, GFX_CORNER_ALL, COLOR_DOCK_ITEM_BG);
        gfx_draw_string(tx + 8, ty + 4, lbl, COLOR_TEXT_MAIN);
    }

    // 3. Системный трей
    int tray_x = g_screen_w - 270;

    // Индикатор Caps Lock: появляется слева от трея только когда он включён
    if (g_caps_on)
        gfx_icon_draw(tray_x - 30, bar_y + (TASKBAR_HEIGHT - 22) / 2, "capslock", 22, COLOR_ACCENT_BLUE);

    
    // Аудио иконка в трее (KDE Breeze: звук с волнами либо перечеркнутый динамик)
    int hda_ok = (g_audio_status != HDA_STATUS_NOT_FOUND);
    uint32_t snd_col = (g_active_popup == POPUP_SOUND) ? COLOR_ACCENT_BLUE : (hda_ok ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
    gfx_icon_draw(tray_x - 2, bar_y + (TASKBAR_HEIGHT - 22) / 2, hda_ok ? "sound" : "sound-mute", 22, snd_col);

    // [СЕТЬ]: 4 состояния в стиле KDE Breeze
    int net_y = bar_y + (TASKBAR_HEIGHT - 22) / 2;
    if (!g_net_driver_ok) {
        gfx_icon_draw(tray_x + 26, net_y, "network-off", 22, COLOR_TEXT_MUTED);
    } else if (g_net_info.state == NET_STATE_ONLINE) {
        uint32_t col = (g_active_popup == POPUP_NET) ? COLOR_ACCENT_BLUE : COLOR_TEXT_MAIN;
        gfx_icon_draw(tray_x + 26, net_y, "network", 22, col);
    } else if (g_net_info.state == NET_STATE_LOCAL_ONLY) {
        gfx_icon_draw(tray_x + 26, net_y, "network-warn", 22, COLOR_TEXT_MAIN);
    } else {
        gfx_icon_draw(tray_x + 26, net_y, "network-off", 22, COLOR_TEXT_MUTED);
    }

    gfx_icon_draw(tray_x + 54, net_y, "brightness", 22,
                  (g_active_popup == POPUP_BRIGHTNESS) ? COLOR_ACCENT_BLUE : COLOR_TEXT_MAIN);

    // Раскладка
    const char* lay = g_layout_ru ? "RU" : "EN";
    gfx_round_rect(tray_x + 84, bar_y + 10, 36, 24, 5, GFX_CORNER_ALL, COLOR_DOCK_ITEM_BG);
    gfx_draw_string(tray_x + 84 + (36 - gfx_text_width(lay)) / 2, bar_y + 14, lay, COLOR_TEXT_MAIN);

    // Часы
    uint8_t local_hour = ((t->hour + g_tz_offset) % 24 + 24) % 24;
    gfx_round_rect(tray_x + 130, bar_y + 4, 130, 36, 6, GFX_CORNER_ALL,
                   (g_active_popup == POPUP_CLOCK) ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
    // Часы и дату собираем строками: шрифт переменной ширины, и рисовать цифры
    // по фиксированным отступам больше нельзя
    char hhmm[8], secs[6], date[16];
    fmt2(hhmm, local_hour); hhmm[2] = ':'; fmt2(hhmm + 3, t->min); hhmm[5] = '\0';
    secs[0] = ':'; fmt2(secs + 1, t->sec); secs[3] = '\0';
    fmt2(date, t->day); date[2] = '.'; fmt2(date + 3, t->month);
    date[5] = '.'; date[6] = '2'; date[7] = '0'; fmt2(date + 8, t->year); date[10] = '\0';

    int clock_w = gfx_text_width(hhmm) + gfx_text_width(secs);
    int clock_x = tray_x + 130 + (130 - clock_w) / 2;
    gfx_draw_string(clock_x, bar_y + 4, hhmm, COLOR_TEXT_MAIN);
    gfx_draw_string(clock_x + gfx_text_width(hhmm), bar_y + 4, secs, COLOR_ACCENT_BLUE);
    gfx_draw_string(tray_x + 130 + (130 - gfx_text_width(date)) / 2, bar_y + 22, date, COLOR_TEXT_MUTED);

    // 4. Меню "Пуск"
    if (g_menu_open) {
        int item_h = 26;
        int m_w = 262;   // хватает самому длинному названию («Параметры системы»)
        int m_h = 50 + ((g_menu_app_count + 1) * item_h) + 38;
        int m_x = 6, m_y = bar_y - m_h - 4;

        gfx_round_rect(m_x - 1, m_y - 1, m_w + 2, m_h + 2, 9, GFX_CORNER_ALL, COLOR_PANEL_BORDER);
        gfx_round_rect(m_x, m_y, m_w, m_h, 8, GFX_CORNER_ALL, COLOR_PANEL_BG);

        // --- ШАПКА: Плейсхолдер пользователя system ---
        gfx_round_rect(m_x, m_y, m_w, 42, 8, GFX_CORNER_TOP, 0x1B1E20);
        gfx_draw_mono_icon(m_x + 12, m_y + 13, icon_placeholder, COLOR_ACCENT_BLUE, 0, 0);
        gfx_draw_string(m_x + 36, m_y + 8, g_session_name[0] ? g_session_name : "system", COLOR_TEXT_MAIN);
        gfx_draw_string(m_x + 36, m_y + 23, sys_is_admin(g_session_uid) ? "Администратор" : "Пользователь", COLOR_TEXT_MUTED);
        gfx_draw_rect(m_x + 8, m_y + 42, m_w - 16, 1, COLOR_PANEL_BORDER);

        // Список приложений из /desprogs
        int start_apps_y = m_y + 48;
        for (int i = 0; i < g_menu_app_count; i++) {
            int iy = start_apps_y + (i * item_h);
            int hover = (mouse_x >= m_x && mouse_x <= m_x + m_w && mouse_y >= iy && mouse_y < iy + item_h);
            if (hover) gfx_round_rect(m_x + 4, iy, m_w - 8, item_h - 2, 4, GFX_CORNER_ALL, COLOR_DOCK_HOVER);
            gfx_draw_string(m_x + 16, iy + 4, g_menu_apps[i].name, COLOR_TEXT_MAIN);
        }

        // Встроенный osver
        int osver_y = start_apps_y + (g_menu_app_count * item_h);
        int osver_h = (mouse_x >= m_x && mouse_x <= m_x + m_w && mouse_y >= osver_y && mouse_y < osver_y + item_h);
        if (osver_h) gfx_round_rect(m_x + 4, osver_y, m_w - 8, item_h - 2, 4, GFX_CORNER_ALL, COLOR_DOCK_HOVER);
        gfx_draw_string(m_x + 16, osver_y + 4, "О системе (osver)", COLOR_TEXT_MUTED);

        // Нижняя панель питания
        int exit_y = m_y + m_h - 32;
        int btn_main_w = m_w - 38;
        int btn_arrow_w = 26;

        // Основная кнопка: Завершить работу
        int shut_h = (mouse_x >= m_x + 4 && mouse_x < m_x + 4 + btn_main_w && mouse_y >= exit_y && mouse_y <= exit_y + 26);
        gfx_round_rect(m_x + 4, exit_y, btn_main_w, 26, 5, GFX_CORNER_ALL, shut_h ? 0x551111 : COLOR_DOCK_ITEM_BG);
        gfx_draw_string(m_x + 10, exit_y + 6, "Завершение работы", shut_h ? COLOR_WARN_RED : COLOR_TEXT_MAIN);

        // Кнопка стрелки [>]
        int arrow_x = m_x + 4 + btn_main_w + 2;
        int arrow_h = (mouse_x >= arrow_x && mouse_x <= arrow_x + btn_arrow_w && mouse_y >= exit_y && mouse_y <= exit_y + 26);
        gfx_round_rect(arrow_x, exit_y, btn_arrow_w, 26, 5, GFX_CORNER_ALL, (arrow_h || g_power_menu_open) ? COLOR_DOCK_HOVER : COLOR_DOCK_ITEM_BG);
        gfx_draw_string(arrow_x + 9, exit_y + 6, ">", COLOR_TEXT_MAIN);

        // Каскадное подменю питания
        if (g_power_menu_open) {
            int sub_x = m_x + m_w + 2;
            int sub_w = 170;
            int sub_h = 4 * 24 + 6;
            int sub_y = m_y + m_h - sub_h;

            gfx_round_rect(sub_x - 1, sub_y - 1, sub_w + 2, sub_h + 2, 7, GFX_CORNER_ALL, COLOR_PANEL_BORDER);
            gfx_round_rect(sub_x, sub_y, sub_w, sub_h, 6, GFX_CORNER_ALL, COLOR_PANEL_BG);

            const char* items[] = {
                "Выход в консоль",
                "Выйти из сеанса",
                "Перезагрузка",
                "Выключение"
            };

            for (int k = 0; k < 4; k++) {
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
        // Число и знак процента — одной строкой: ширина цифр у шрифта своя
        char pct[8];
        int pv = g_brightness, pl = 0;
        if (pv >= 100) pct[pl++] = '1';
        if (pv >= 10) pct[pl++] = (char)('0' + (pv / 10) % 10);
        pct[pl++] = (char)('0' + pv % 10);
        pct[pl++] = '%';
        pct[pl] = '\0';
        gfx_draw_string(sl_x + 110, sl_y + 10, pct, COLOR_ACCENT_BLUE);

        int track_x = sl_x + 14, track_y = sl_y + 36, track_w = 172, track_h = 6;
        gfx_draw_rect(track_x, track_y, track_w, track_h, 0x141618);
        int fill_w = (track_w * g_brightness) / 100;
        gfx_draw_rect(track_x, track_y, fill_w, track_h, COLOR_ACCENT_BLUE);
        int knob_x = track_x + fill_w - 4;
        gfx_draw_rect(knob_x, track_y - 4, 8, 14, COLOR_TEXT_MAIN);
    } 
    // 6. Всплывающее окно часового пояса: список городов из /etc/timezone
    else if (g_active_popup == POPUP_CLOCK) {
        tz_load();
        int cl_x, cl_y;
        tz_geom(&cl_x, &cl_y);
        int list_y = cl_y + 32;
        int list_w = TB_TZ_W - 8 - ((s_tz_count > TB_TZ_ROWS) ? TB_TZ_SB_W + 4 : 0);

        gfx_draw_rect(cl_x - 1, cl_y - 1, TB_TZ_W + 2, TB_TZ_H + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(cl_x, cl_y, TB_TZ_W, TB_TZ_H, COLOR_PANEL_BG);
        gfx_draw_string(cl_x + 10, cl_y + 8, "ЧАСОВОЙ ПОЯС:", COLOR_ACCENT_BLUE);

        for (int r = 0; r < TB_TZ_ROWS; r++) {
            int idx = s_tz_top + r;
            if (idx >= s_tz_count) break;
            int ry  = list_y + r * TB_TZ_ROW_H;
            int sel = (idx == g_tz_index);
            int hov = (mouse_x >= cl_x + 4 && mouse_x < cl_x + 4 + list_w &&
                       mouse_y >= ry && mouse_y < ry + TB_TZ_ROW_H);
            if (sel)      gfx_draw_rect(cl_x + 4, ry, list_w, TB_TZ_ROW_H - 2, COLOR_ACCENT_BLUE);
            else if (hov) gfx_draw_rect(cl_x + 4, ry, list_w, TB_TZ_ROW_H - 2, COLOR_DOCK_HOVER);

            gfx_draw_string(cl_x + 12, ry + 4, s_tz[idx].city, COLOR_TEXT_MAIN);
            int ox = cl_x + 4 + list_w - 66;
            gfx_draw_string(ox, ry + 4, "UTC", sel ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
            int off = s_tz[idx].offset;
            gfx_draw_string(ox + 24, ry + 4, (off < 0) ? "-" : "+", sel ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
            gfx_draw_dec(ox + 32, ry + 4, (off < 0) ? -off : off, sel ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
        }

        // Полоса прокрутки с плавным бегунком
        if (s_tz_count > TB_TZ_ROWS) {
            int sb_x = cl_x + TB_TZ_W - 4 - TB_TZ_SB_W;
            int sb_h = TB_TZ_ROWS * TB_TZ_ROW_H;
            int kh = tz_knob_h(), ky = tz_knob_y(list_y);
            gfx_draw_rect(sb_x, list_y, TB_TZ_SB_W, sb_h, 0x141618);
            int hov = (mouse_x >= sb_x && mouse_x < sb_x + TB_TZ_SB_W &&
                       mouse_y >= ky && mouse_y < ky + kh);
            gfx_draw_rect(sb_x + 3, ky, TB_TZ_SB_W - 6, kh,
                          s_tz_drag ? COLOR_ACCENT_BLUE : (hov ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED));
        }
    }
    // 7. Карточка состояния сети Ethernet
    else if (g_active_popup == POPUP_NET) {
        int pop_w = 230, pop_h = 104;
        int pop_x = g_screen_w - pop_w - 60;
        int pop_y = bar_y - pop_h - 6;

        gfx_draw_rect(pop_x - 1, pop_y - 1, pop_w + 2, pop_h + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(pop_x, pop_y, pop_w, pop_h, COLOR_PANEL_BG);

        if (!g_net_driver_ok) {
            gfx_draw_mono_icon(pop_x + 12, pop_y + 12, icon_network_none, COLOR_WARN_RED, COLOR_WARN_RED, 1);
            gfx_draw_string(pop_x + 34, pop_y + 8, "Ethernet (LAN)", COLOR_TEXT_MAIN);
            gfx_draw_string(pop_x + 34, pop_y + 22, "Драйвер не работает", COLOR_WARN_RED);
        } else if (g_net_info.state == NET_STATE_ONLINE) {
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

        if (!g_net_driver_ok) {
            gfx_draw_string(pop_x + 12, pop_y + 54, "Статус:", COLOR_TEXT_MUTED);
            gfx_draw_string(pop_x + 75, pop_y + 54, "Драйвер не найден", COLOR_WARN_RED);
            gfx_draw_string(pop_x + 12, pop_y + 74, "Адаптер:", COLOR_TEXT_MUTED);
            gfx_draw_string(pop_x + 95, pop_y + 74, "Недоступен", COLOR_TEXT_MUTED);
        } else if (g_net_info.state != NET_STATE_DISCONNECTED) {
            char ip_str[24];
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
            gfx_draw_string(pop_x + 80, pop_y + 54, "Кабель не подключен", COLOR_TEXT_MUTED);
        }
    }
    // 8. Полноценное окно аудио в стиле KDE Plasma
    else if (g_active_popup == POPUP_SOUND) {
        int pop_w = 260, pop_h = 214;
        int pop_x = g_screen_w - pop_w - 60;
        int pop_y = bar_y - pop_h - 6;

        gfx_draw_rect(pop_x - 1, pop_y - 1, pop_w + 2, pop_h + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(pop_x, pop_y, pop_w, pop_h, COLOR_PANEL_BG);

        // Шапка
        gfx_draw_string(pop_x + 12, pop_y + 10, "Громкость", COLOR_TEXT_MAIN);
        if (g_audio_status != HDA_STATUS_NOT_FOUND) {
            gfx_draw_string(pop_x + 160, pop_y + 10, "[HDA: Готов]", 0x55FF55);
        } else {
            gfx_draw_string(pop_x + 152, pop_y + 10, "[HDA: Нет]", COLOR_WARN_RED);
        }

        // Кнопка Mute
        gfx_draw_string(pop_x + 96, pop_y + 10, "[Mute]", s_muted ? COLOR_WARN_RED : COLOR_TEXT_MAIN);

        // Слайдер громкости
        int track_x = pop_x + 14, track_y = pop_y + 36, track_w = 180, track_h = 6;
        gfx_draw_rect(track_x, track_y, track_w, track_h, 0x141618);
        int fill_w = (track_w * g_volume) / 100;
        gfx_draw_rect(track_x, track_y, fill_w, track_h, s_muted ? 0x555555 : COLOR_ACCENT_BLUE);
        int knob_x = track_x + fill_w - 4;
        gfx_draw_rect(knob_x, track_y - 4, 8, 14, COLOR_TEXT_MAIN);
        gfx_draw_dec(pop_x + 204, pop_y + 32, g_volume, COLOR_ACCENT_BLUE);
        gfx_draw_string(pop_x + (g_volume == 100 ? 230 : 222), pop_y + 32, "%", COLOR_ACCENT_BLUE);

        // Разделитель
        gfx_draw_rect(pop_x + 10, pop_y + 56, pop_w - 20, 1, COLOR_PANEL_BORDER);

        // Список физических устройств вывода
        const char* names[4] = {
            "Автовыбор (AUTO)",
            "Встроенные динамики",
            "Наушники (Jack 3.5)",
            "HDMI / DisplayPort"
        };

        int list_y = pop_y + 64;
        int item_h = 26;

        for (int i = 0; i < 4; i++) {
            int iy = list_y + (i * item_h);
            int is_selected = (g_audio_output == i);

            // Проверка физической доступности выхода
            int is_available = 1;
            if (i == 1 && !g_audio_outputs.has_speakers && !g_audio_outputs.analog_playback) is_available = 0;
            if (i == 2 && !g_audio_outputs.has_headphones && !g_audio_outputs.has_lineout) is_available = 0;
            if (i == 3 && (!g_audio_outputs.has_hdmi || !g_audio_outputs.hdmi_playback)) is_available = 0;

            int hover = (mouse_x >= pop_x + 4 && mouse_x <= pop_x + pop_w - 4 && mouse_y >= iy && mouse_y < iy + item_h);
            uint32_t bg = hover ? COLOR_DOCK_HOVER : (is_selected ? 0x2A323D : 0);
            if (bg) {
                gfx_draw_rect(pop_x + 4, iy, pop_w - 8, item_h - 2, bg);
            }

            // Маркер выбора и имя
            uint32_t title_color = is_available ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED;
            gfx_draw_string(pop_x + 10, iy + 4, is_selected ? "[X]" : "[ ]", is_selected ? COLOR_ACCENT_BLUE : COLOR_TEXT_MUTED);
            gfx_draw_string(pop_x + 36, iy + 4, names[i], title_color);

            // Статус подключения
            if (!is_available) {
                gfx_draw_string(pop_x + 185, iy + 4, "Нет", COLOR_TEXT_MUTED);
            } else if (i == 2) {
                if (g_audio_outputs.headphones_plugged) {
                    gfx_draw_string(pop_x + 185, iy + 4, "Подкл.", 0x55FF55);
                } else {
                    gfx_draw_string(pop_x + 185, iy + 4, "Откл.", COLOR_TEXT_MUTED);
                }
            } else if (i == 3) {
                if (g_audio_outputs.hdmi_connected > 0) {
                    gfx_draw_string(pop_x + 185, iy + 4, "Сигнал", 0x55FF55);
                } else {
                    gfx_draw_string(pop_x + 185, iy + 4, "Спит", COLOR_TEXT_MUTED);
                }
            }
        }

        // Что драйвер делает на самом деле: по одному виду списка не понять,
        // применился выбор или выход физически промолчал.
        const char* tgt = (g_audio_target == 2) ? "HDMI"
                        : (g_audio_target == 1) ? "аналоговый выход" : "никуда";
        gfx_draw_string(pop_x + 12, pop_y + 170, "Звук идёт:", COLOR_TEXT_MUTED);
        gfx_draw_string(pop_x + 104, pop_y + 170, tgt,
                        g_audio_target ? 0x55FF55 : COLOR_WARN_RED);

        gfx_draw_string(pop_x + 16, pop_y + 188, "Клик: переключить и тест (1с)", COLOR_TEXT_MUTED);
    }
}

int taskbar_handle_click(int mx, int my) {
    int bar_y = g_screen_h - TASKBAR_HEIGHT;

    // 1. Повторный клик по кнопке [overOS] (Пуск)
    if (mx >= 6 && mx < DOCK_X - 6 && my >= bar_y + 4) {
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
        int m_w = 262;   // хватает самому длинному названию («Параметры системы»)
        int m_h = 50 + ((g_menu_app_count + 1) * item_h) + 38;
        int m_x = 6, m_y = bar_y - m_h - 4;

        // Клики внутри каскадного подменю
        if (g_power_menu_open) {
            int sub_x = m_x + m_w + 2;
            int sub_w = 170;
            int sub_h = 4 * 24 + 6;
            int sub_y = m_y + m_h - sub_h;

            if (mx >= sub_x && mx <= sub_x + sub_w && my >= sub_y && my <= sub_y + sub_h) {
                int clicked_item = (my - (sub_y + 3)) / 24;
                if (clicked_item == 0) { // Выход в консоль
                    sys_exit(0);
                } else if (clicked_item == 1) { // Выйти из сеанса -> экран входа
                    g_logout_requested = 1;
                } else if (clicked_item == 2) { // Перезагрузка
                    sys_system_reboot();
                } else if (clicked_item == 3) { // Выключение
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

    // 3. Значки дока: запущенную программу показываем/сворачиваем, остальные запускаем
    int di = dock_at(mx, my);
    if (di >= 0) {
        g_menu_open = 0;
        g_active_popup = POPUP_NONE;
        if (s_dock[di].win >= 0) {
            window_t* w = &window_get_list(0)[s_dock[di].win];
            if (w->is_minimized || !w->is_active) {
                w->is_minimized = 0;
                window_activate(w);
            } else {
                w->is_minimized = 1;
                w->is_active = 0;
            }
        } else if (s_dock[di].path[0]) {
            launch_file_safe(s_dock[di].path);
        }
        return 1;
    }

    // 4. Трей — переключение окон
    int tray_x = g_screen_w - 270;
    if (mx >= tray_x && mx < tray_x + 24 && my >= bar_y + 10) {
        g_menu_open = 0;
        if (g_active_popup == POPUP_SOUND) {
            g_active_popup = POPUP_NONE;
        } else {
            g_active_popup = POPUP_SOUND;
            g_audio_status = sys_audio_get_status();
            if (g_audio_status != HDA_STATUS_NOT_FOUND) {
                sys_audio_jack_poll();
                sys_audio_get_outputs(&g_audio_outputs);
                g_audio_target = sys_audio_get_target();
                g_volume = sys_audio_get_volume();     // громкость могла измениться в плеере
                s_vol_applied = g_volume;
            }
        }
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
        if (g_active_popup == POPUP_CLOCK) {
            g_active_popup = POPUP_NONE;
        } else {
            g_active_popup = POPUP_CLOCK;
            tz_popup_opened();
        }
        return 1;
    }

    // Клик по выбору часового пояса внутри попапа часов
    if (g_active_popup == POPUP_CLOCK) {
        tz_load();
        int cl_x, cl_y;
        tz_geom(&cl_x, &cl_y);
        int list_y = cl_y + 32;
        int list_w = TB_TZ_W - 8 - ((s_tz_count > TB_TZ_ROWS) ? TB_TZ_SB_W + 4 : 0);
        int sb_h   = TB_TZ_ROWS * TB_TZ_ROW_H;

        if (mx >= cl_x && mx <= cl_x + TB_TZ_W && my >= list_y && my < list_y + sb_h) {
            if (s_tz_count > TB_TZ_ROWS && mx >= cl_x + TB_TZ_W - 4 - TB_TZ_SB_W) {
                int kh = tz_knob_h(), ky = tz_knob_y(list_y);
                if (my >= ky && my < ky + kh) {          // взяли бегунок
                    s_tz_drag = 1;
                    s_tz_drag_off = my - ky;
                } else {                                  // клик по дорожке — страница
                    s_tz_top += (my < ky) ? -(TB_TZ_ROWS - 1) : (TB_TZ_ROWS - 1);
                    tz_clamp();
                }
                return 1;
            }
            int r = (my - list_y) / TB_TZ_ROW_H;
            int idx = s_tz_top + r;
            if (mx < cl_x + 4 + list_w && idx >= 0 && idx < s_tz_count) {
                // Пояс — системная настройка: пишем в /etc/timezone, как timedatectl
                if (sys_set_system(-1, idx, -1) == 0) {
                    g_tz_index  = idx;
                    g_tz_offset = s_tz[idx].offset;
                }
            }
            return 1;
        }
    }

    // Клик внутри попапа звука
    if (g_active_popup == POPUP_SOUND) {
        int pop_w = 260, pop_h = 214;
        int pop_x = g_screen_w - pop_w - 60;
        int pop_y = bar_y - pop_h - 6;

        if (mx >= pop_x && mx <= pop_x + pop_w && my >= pop_y && my <= pop_y + pop_h) {

            // 0. Кнопка Mute
            if (mx >= pop_x + 92 && mx <= pop_x + 148 && my >= pop_y + 4 && my <= pop_y + 22) {
                s_muted = !s_muted;
                sys_audio_set_mute(s_muted);
                return 1;
            }

            // 1. Клик по полосе громкости
            int track_x = pop_x + 14, track_y = pop_y + 36, track_w = 180, track_h = 6;
            if (mx >= track_x - 6 && mx <= track_x + track_w + 6 &&
                my >= track_y - 8 && my <= track_y + track_h + 8) {
                int val = ((mx - track_x) * 100) / track_w;
                if (val < 0) val = 0;
                if (val > 100) val = 100;
                g_volume = val;
                g_volume_dragging = 1;
                settings_save();
                return 1;
            }

            // 2. Выбор устройства вывода
            int list_y = pop_y + 64;
            int item_h = 26;

            for (int i = 0; i < 4; i++) {
                int iy = list_y + (i * item_h);
                if (my >= iy && my < iy + item_h) {
                    int can_switch = 1;
                    if (i == 1 && !g_audio_outputs.has_speakers && !g_audio_outputs.analog_playback) can_switch = 0;
                    if (i == 2 && !g_audio_outputs.has_headphones && !g_audio_outputs.has_lineout) can_switch = 0;
                    if (i == 3 && (!g_audio_outputs.has_hdmi || !g_audio_outputs.hdmi_playback)) can_switch = 0;

                    if (can_switch && sys_audio_set_output(i) == 0) {
                        g_audio_output = i;
                        settings_save();

                        // Тест выбранного выхода через таймер ядра
                        tone_start();

                        sys_audio_get_outputs(&g_audio_outputs);
                        g_audio_target = sys_audio_get_target();
                    }
                    return 1;
                }
            }
            return 1;
        }
    }

    // Клик по иконке сети в трее
    else if (mx >= tray_x + 28 && mx < tray_x + 52 && my >= bar_y + 10) {
        g_menu_open = 0;
        if (g_active_popup == POPUP_NET) {
            g_active_popup = POPUP_NONE;
        } else {
            g_active_popup = POPUP_NET;
            int res = sys_get_net_info(&g_net_info);
            g_net_driver_ok = (res >= 0);
        }
        return 1;
    }

    return 0;
}

// Колесо мыши над попапом часов прокручивает список поясов
int taskbar_popup_scroll(int mx, int my, int wheel) {
    if (g_active_popup != POPUP_CLOCK || !wheel) return 0;
    if (!taskbar_is_inside_popup(mx, my)) return 0;
    s_tz_top += wheel * 3;   // колесо вниз (+) — список вниз, как в greeter
    tz_clamp();
    return 1;
}

// Удержание бегунка: вызывается каждый кадр из desktop.c
int taskbar_popup_drag(int my, int pressed) {
    if (!s_tz_drag) return 0;
    if (!pressed || g_active_popup != POPUP_CLOCK) { s_tz_drag = 0; return 1; }

    int cl_x, cl_y;
    tz_geom(&cl_x, &cl_y);
    int list_y = cl_y + 32;
    int span = TB_TZ_ROWS * TB_TZ_ROW_H - tz_knob_h();
    int rel = my - s_tz_drag_off - list_y;
    int old = s_tz_top;
    s_tz_top = (span > 0) ? (rel * (s_tz_count - TB_TZ_ROWS) + span / 2) / span : 0;
    tz_clamp();
    return s_tz_top != old;
}
