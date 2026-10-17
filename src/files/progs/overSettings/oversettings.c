// =============================================================================
// overSettings — параметры системы overOS.
//
// Разделы слева, содержимое справа — как в «Системных параметрах» Plasma:
//   «О системе»     — ядро, версия, пользователь, процессы и занятая память;
//   «Дата и время»  — текущее время и часовой пояс из таблицы ядра;
//   «Язык и ввод»   — язык системы и вторая раскладка клавиатуры;
//   «Пользователи»  — учётные записи из /etc/passwd и кто из них администратор.
//
// Системные настройки (пояс, язык, раскладка) пишет только root — это проверяет
// ядро (сисколл 96). Поэтому у окна есть «Разблокировать»: программа просит
// пароль и поднимает себе права через sys_sudo, ровно как это делает проводник.
// =============================================================================

#include "common/ovos.h"
#include "libui/ui.h"
#include "libui/libui_ll.h"

#define PAGE_ABOUT 0
#define PAGE_TIME  1
#define PAGE_LANG  2
#define PAGE_MOUSE 3
#define PAGE_USERS 4
#define PAGE_COUNT 5

static const uint16_t icon_settings[16] = {
    0x0180, 0x0180, 0x0DB0, 0x1FF8, 0x3FFC, 0x3E7C, 0x7C3E, 0x783C,
    0x783C, 0x7C3E, 0x3E7C, 0x3FFC, 0x1FF8, 0x0DB0, 0x0180, 0x0180
};

static int  s_page;
static int  s_tz_sel, s_tz_scroll, s_tz_count;
static int  s_user_sel, s_user_scroll;
static char s_msg[96];
static uint64_t s_msg_until;
static int  s_ask_pass;
static int  s_mouse_speed = 100;
static int  s_mouse_accel = 1;
static char s_pass[64];

static ov_sys_settings_t s_sys;
static ov_kernel_info_t  s_kern;

#define TZ_MAX 64
static ov_tz_t s_tz[TZ_MAX];
static const char* s_tz_names[TZ_MAX];

#define USERS_MAX 32
static ov_user_t s_users[USERS_MAX];
static int       s_nusers;
static const char* s_user_names[USERS_MAX];
static char      s_user_rows[USERS_MAX][64];

static void msg(const char* t) {
    int i = 0;
    while (t[i] && i < (int)sizeof(s_msg) - 1) { s_msg[i] = t[i]; i++; }
    s_msg[i] = '\0';
    s_msg_until = ov_ms() + 5000;
}

static void append(char* d, int* p, const char* s, int cap) {
    while (*s && *p < cap - 1) d[(*p)++] = *s++;
    d[*p] = '\0';
}

static void append_int(char* d, int* p, int v, int cap) {
    char num[16];
    ui_itoa(v, num);
    append(d, p, num, cap);
}

static void load_tz(void) {
    s_tz_count = ov_tz_info(0, &s_tz[0]);
    if (s_tz_count > TZ_MAX) s_tz_count = TZ_MAX;
    for (int i = 0; i < s_tz_count; i++) {
        ov_tz_info(i, &s_tz[i]);
        s_tz_names[i] = s_tz[i].city[0] ? s_tz[i].city : s_tz[i].name;
    }
}

static void load_users(void) {
    s_nusers = 0;
    // root плюс обычные пользователи (uid с 1000, как в Linux)
    int ids[USERS_MAX];
    int n = 0;
    ids[n++] = 0;
    for (int uid = 1000; uid < 1000 + USERS_MAX - 1 && n < USERS_MAX; uid++) ids[n++] = uid;
    for (int i = 0; i < n && s_nusers < USERS_MAX; i++) {
        ov_user_t u;
        if (ov_getpwuid(ids[i], &u) <= 0 || !u.name[0]) continue;
        s_users[s_nusers] = u;
        int p = 0;
        s_user_rows[s_nusers][0] = '\0';
        append(s_user_rows[s_nusers], &p, u.name, 64);
        append(s_user_rows[s_nusers], &p, "  (uid ", 64);
        append_int(s_user_rows[s_nusers], &p, u.uid, 64);
        append(s_user_rows[s_nusers], &p, ")", 64);
        if (ov_is_admin(u.uid)) append(s_user_rows[s_nusers], &p, "  — администратор", 64);
        s_user_names[s_nusers] = s_user_rows[s_nusers];
        s_nusers++;
    }
}

static int am_root(void) { return ov_getuid() == 0; }

// Применить системные настройки; при отказе предложить разблокировку
static void apply_system(void) {
    if (ov_set_system(s_sys.lang, s_sys.tz_index, s_sys.layout2) == 0) {
        msg("Сохранено.");
        ov_get_system(&s_sys);
    } else {
        s_ask_pass = 1;
        msg("Нужны права администратора.");
    }
}

static void draw_about(ui_win_t* w, int x, int y, int ww) {
    char row[128];
    int p;

    ui_text(w, x, y, "Система", UI_ACCENT);
    y += 26;

    p = 0; row[0] = '\0';
    append(row, &p, s_kern.name[0] ? s_kern.name : "overOS", (int)sizeof(row));
    append(row, &p, " ", (int)sizeof(row));
    append(row, &p, s_kern.version, (int)sizeof(row));
    append(row, &p, " (", (int)sizeof(row));
    append(row, &p, s_kern.arch, (int)sizeof(row));
    append(row, &p, ")", (int)sizeof(row));
    ui_label(w, x, y, row); y += 24;

    ov_user_t me;
    int uid = ov_getuid();
    p = 0; row[0] = '\0';
    append(row, &p, "Пользователь: ", (int)sizeof(row));
    if (ov_getpwuid(uid, &me) > 0 && me.name[0]) append(row, &p, me.name, (int)sizeof(row));
    else append(row, &p, uid == 0 ? "root" : "?", (int)sizeof(row));
    if (ov_is_admin(uid)) append(row, &p, " (администратор)", (int)sizeof(row));
    ui_label(w, x, y, row); y += 24;

    static ov_proc_t pr[128];
    int n = ov_ps(pr, 128);
    p = 0; row[0] = '\0';
    append(row, &p, "Процессов: ", (int)sizeof(row));
    append_int(row, &p, n, (int)sizeof(row));
    ui_label(w, x, y, row); y += 24;

    // Процессор: модель, ядра и НАСТОЯЩАЯ частота (ядро меряет её по APERF/MPERF)
    ov_cpuinfo_t ci;
    if (ov_cpuinfo(&ci) == 0) {
        ui_label(w, x, y, ci.brand); y += 22;
        p = 0; row[0] = '\0';
        append(row, &p, "Ядер: ", (int)sizeof(row));
        append_int(row, &p, ci.cores, (int)sizeof(row));
        append(row, &p, ", потоков: ", (int)sizeof(row));
        append_int(row, &p, ci.threads, (int)sizeof(row));
        append(row, &p, " (используется 1), ", (int)sizeof(row));
        // Текущая частота измеряется по APERF/MPERF; под гипервизором их нет,
        // и тогда показываем паспортную, а не ноль.
        append_int(row, &p, ci.cur_mhz ? ci.cur_mhz : ci.base_mhz, (int)sizeof(row));
        append(row, &p, " МГц", (int)sizeof(row));
        if (ci.max_mhz > 0) {
            append(row, &p, " из ", (int)sizeof(row));
            append_int(row, &p, ci.max_mhz, (int)sizeof(row));
        }
        ui_label(w, x, y, row); y += 22;
        p = 0; row[0] = '\0';
        append(row, &p, ci.turbo_on ? "Турбо включено, " : "Турбо выключено, ", (int)sizeof(row));
        append(row, &p, ci.governor == OV_CPU_GOV_HWP    ? "аппаратный регулятор на максимуме" :
                        ci.governor == OV_CPU_GOV_PSTATE ? "запрошен максимальный P-state" :
                                                           "частотой управляет прошивка", (int)sizeof(row));
        ui_label_muted(w, x, y, row); y += 26;
    }

    // Память берём у ядра, а не суммой по процессам: там своя учётная запись
    // (таблицы страниц, буферы TTY, образы библиотек), и сумма её не покрывает.
    ov_meminfo_t mi;
    if (ov_meminfo(&mi) == 0) {
        p = 0; row[0] = '\0';
        append(row, &p, "Оперативная память: ", (int)sizeof(row));
        append_int(row, &p, (int)(mi.used / (1024 * 1024)), (int)sizeof(row));
        append(row, &p, " МБ занято из ", (int)sizeof(row));
        append_int(row, &p, (int)(mi.total / (1024 * 1024)), (int)sizeof(row));
        append(row, &p, " МБ", (int)sizeof(row));
        ui_label(w, x, y, row); y += 24;
    }

    p = 0; row[0] = '\0';
    append(row, &p, "Система работает: ", (int)sizeof(row));
    append_int(row, &p, (int)(ov_ms() / 60000), (int)sizeof(row));
    append(row, &p, " мин", (int)sizeof(row));
    ui_label(w, x, y, row); y += 34;

    ui_label_muted(w, x, y, "Графическая оболочка overDesktop, оконный протокол overOS.");
    y += 22;
    ui_label_muted(w, x, y, "Звук и видео — /lib/ffmpeg.ll (FFmpeg, собранный под overOS).");
    (void)ww;
}

static void draw_time(ui_win_t* w, int x, int y, int ww, int hh) {
    char row[128];
    int p = 0;

    ui_text(w, x, y, "Дата и время", UI_ACCENT);
    y += 26;

    ov_rtc_t t;
    if (ov_rtc(&t) > 0) {
        row[0] = '\0';
        if (t.day < 10) append(row, &p, "0", (int)sizeof(row));
        append_int(row, &p, t.day, (int)sizeof(row));
        append(row, &p, ".", (int)sizeof(row));
        if (t.month < 10) append(row, &p, "0", (int)sizeof(row));
        append_int(row, &p, t.month, (int)sizeof(row));
        append(row, &p, ".", (int)sizeof(row));
        append_int(row, &p, 2000 + t.year, (int)sizeof(row));
        append(row, &p, "  ", (int)sizeof(row));
        if (t.hour < 10) append(row, &p, "0", (int)sizeof(row));
        append_int(row, &p, t.hour, (int)sizeof(row));
        append(row, &p, ":", (int)sizeof(row));
        if (t.min < 10) append(row, &p, "0", (int)sizeof(row));
        append_int(row, &p, t.min, (int)sizeof(row));
        ui_label(w, x, y, row);
    }
    y += 28;

    ui_label_muted(w, x, y, "Часовой пояс (из таблицы ядра, как tzdata):");
    y += 22;
    int list_h = hh - y - 60;
    if (list_h < 80) list_h = 80;
    ui_list(w, x, y, ww - 20, list_h, s_tz_names, s_tz_count, &s_tz_sel, &s_tz_scroll);
    y += list_h + 10;
    if (ui_button_accent(w, x, y, 180, 28, "Применить пояс")) {
        s_sys.tz_index = s_tz_sel;
        apply_system();
    }
}

static void draw_lang(ui_win_t* w, int x, int y, int ww) {
    ui_text(w, x, y, "Язык и ввод", UI_ACCENT);
    y += 30;

    ui_label(w, x, y, "Язык системы:");
    y += 26;
    int ru = (s_sys.lang == 0);
    if (ui_button(w, x, y, 140, 28, ru ? "> Русский" : "  Русский")) { s_sys.lang = 0; apply_system(); }
    if (ui_button(w, x + 150, y, 140, 28, !ru ? "> English" : "  English")) { s_sys.lang = 1; apply_system(); }
    y += 46;

    ui_label(w, x, y, "Раскладки клавиатуры:");
    y += 26;
    int two = s_sys.layout2;
    if (ui_button(w, x, y, 290, 28, two ? "Английская и русская (Alt — переключение)"
                                        : "Только английская")) {
        s_sys.layout2 = !s_sys.layout2;
        apply_system();
    }
    y += 40;
    ui_label_muted(w, x, y, "В текстовых консолях раскладка всегда английская,");
    y += 20;
    ui_label_muted(w, x, y, "как в консоли Linux: вторая раскладка работает в графике.");
    (void)ww;
}

// --- Мышь: скорость указателя и ускорение (общие для системы, /etc/mouse.conf) ---
static void draw_mouse(ui_win_t* w, int x, int y, int ww) {
    char row[96];
    int p = 0;

    ui_text(w, x, y, "Мышь и указатель", UI_ACCENT);
    y += 30;

    ui_label(w, x, y, "Скорость указателя:");
    y += 26;

    int v = s_mouse_speed;
    if (ui_slider(w, x, y, ww - 60, &v, 25, 400)) {
        s_mouse_speed = v;
        if (ov_mouse_set(s_mouse_speed, s_mouse_accel) != 0)
            msg("Нужны права администратора: нажмите «Разблокировать».");
    }
    y += 32;

    row[0] = '\0';
    append(row, &p, "Текущее значение: ", (int)sizeof(row));
    append_int(row, &p, s_mouse_speed, (int)sizeof(row));
    append(row, &p, "%  (100% — как отдаёт контроллер)", (int)sizeof(row));
    ui_label_muted(w, x, y, row);
    y += 34;

    if (ui_button(w, x, y, 290, 28, s_mouse_accel ? "Ускорение включено"
                                                  : "Ускорение выключено")) {
        s_mouse_accel = !s_mouse_accel;
        if (ov_mouse_set(s_mouse_speed, s_mouse_accel) != 0) {
            s_mouse_accel = !s_mouse_accel;
            msg("Нужны права администратора: нажмите «Разблокировать».");
        }
    }
    y += 40;

    ui_label_muted(w, x, y, "При ускорении быстрое движение проходит больше точек,");
    y += 20;
    ui_label_muted(w, x, y, "медленное — меньше: так проще и попасть, и пройти экран.");
    y += 28;
    ui_label_muted(w, x, y, "Настройка общая для системы: /etc/mouse.conf.");
}

static void draw_users(ui_win_t* w, int x, int y, int ww, int hh) {
    ui_text(w, x, y, "Пользователи", UI_ACCENT);
    y += 26;
    ui_label_muted(w, x, y, "Учётные записи системы (/etc/passwd):");
    y += 24;
    int list_h = hh - y - 40;
    if (list_h < 80) list_h = 80;
    ui_list(w, x, y, ww - 20, list_h, s_user_names, s_nusers, &s_user_sel, &s_user_scroll);
    y += list_h + 8;
    ui_label_muted(w, x, y, "Новых пользователей заводит команда useradd в терминале.");
}

int main(void) {
    if (libui_bind() < 0) ov_exit(1);
    ui_app_id("ovsettings");

    ui_win_t* w = ui_open("Параметры системы", 700, 480, icon_settings, WP_FLAG_RESIZABLE);
    if (!w) ov_exit(1);

    ov_kernel_info(&s_kern);
    ov_get_system(&s_sys);
    load_tz();
    load_users();
    s_tz_sel = s_sys.tz_index;

    static const char* const pages[PAGE_COUNT] = {
        "О системе", "Дата и время", "Язык и ввод", "Мышь", "Пользователи"
    };

    s_mouse_speed = ov_mouse_get_speed();
    s_mouse_accel = ov_mouse_get_accel();
    if (s_mouse_speed < 25 || s_mouse_speed > 400) s_mouse_speed = 100;

    while (ui_begin(w, UI_WAIT)) {
        int win_w = ui_width(w), win_h = ui_height(w);
        int side_w = 180;

        ui_clear(w);
        ui_fill(w, 0, 0, side_w, win_h, UI_PANEL);
        ui_fill(w, side_w, 0, win_w - side_w, 40, UI_PANEL);
        ui_hline(w, side_w, 40, win_w - side_w, UI_BORDER);
        ui_icon16(w, side_w + 14, 12, icon_settings, UI_ACCENT);
        ui_label(w, side_w + 40, 12, pages[s_page]);

        // Разделы
        for (int i = 0; i < PAGE_COUNT; i++) {
            int y = 14 + i * 36;
            if (i == s_page) ui_fill(w, 0, y - 4, side_w, 32, UI_HOVER);
            if (ui_button(w, 8, y, side_w - 16, 26, pages[i])) s_page = i;
        }

        // Замок внизу боковой панели
        if (!am_root()) {
            if (ui_button(w, 8, win_h - 40, side_w - 16, 28, "Разблокировать")) s_ask_pass = 1;
        } else {
            ui_label_muted(w, 12, win_h - 34, "Права администратора");
        }

        int cx = side_w + 20, cy = 60;
        int cw = win_w - side_w - 20;
        switch (s_page) {
            case PAGE_ABOUT: draw_about(w, cx, cy, cw); break;
            case PAGE_TIME:  draw_time(w, cx, cy, cw, win_h); break;
            case PAGE_LANG:  draw_lang(w, cx, cy, cw); break;
            case PAGE_MOUSE: draw_mouse(w, cx, cy, cw); break;
            default:         draw_users(w, cx, cy, cw, win_h); break;
        }

        if (s_msg_until > ov_ms()) ui_text(w, cx, win_h - 26, s_msg, UI_TEXT_MUTED);

        // Диалог пароля (как «Разблокировать» в проводнике)
        if (s_ask_pass) {
            int dw = 360, dh = 150;
            int dx = (win_w - dw) / 2, dy = (win_h - dh) / 2;
            ui_fill(w, dx, dy, dw, dh, UI_BG_DARK);
            ui_frame(w, dx, dy, dw, dh, UI_BORDER);
            ui_text(w, dx + 16, dy + 14, "Права администратора", UI_ACCENT);
            ui_label_muted(w, dx + 16, dy + 40, "Введите свой пароль:");
            int entered = ui_password(w, dx + 16, dy + 64, dw - 32, s_pass, sizeof(s_pass));
            int ok = ui_button_accent(w, dx + dw - 180, dy + dh - 40, 80, 28, "ОК");
            if (ui_button(w, dx + dw - 90, dy + dh - 40, 80, 28, "Отмена")) {
                s_ask_pass = 0;
                s_pass[0] = '\0';
            } else if (entered || ok) {
                if (ov_sudo(s_pass) == 0) { s_ask_pass = 0; msg("Права получены."); }
                else msg("Пароль не подошёл.");
                s_pass[0] = '\0';
            }
        }

        ui_end(w);
    }

    ui_close(w);
    ov_exit(0);
    return 0;
}
