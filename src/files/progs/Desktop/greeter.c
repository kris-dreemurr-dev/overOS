#include "syscalls.h"
#include "theme.h"
#include "gfx.h"
#include "common/wproto.h"
#include "settings.h"
#include "greeter.h"
#include "wserver.h"

// =============================================================================
// Экран входа в стиле SDDM: часы, аватары пользователей из /etc/passwd,
// поля логина и пароля. Пароль проверяет ядро (sys_auth), сам desktop его не хранит.
// =============================================================================

int  g_session_uid = -1;
int  g_session_gid = -1;
char g_session_name[32] = "";

#define MAX_GREETER_USERS 6
#define FIELD_W 300
#define FIELD_H 34

typedef struct {
    char name[32];
    int  uid;
} greeter_user_t;

static greeter_user_t s_users[MAX_GREETER_USERS];
static int  s_user_count = 0;

static char s_name[32];
static char s_pass[64];
static int  s_name_len = 0, s_pass_len = 0;
// Выбранный в списке пользователь; -1 — "Другой пользователь" (имя вводится вручную).
// Как в KDE Plasma: обычно достаточно кликнуть аватар и ввести только пароль.
static int  s_sel_user = -1;
static int  s_focus = 0;               // 0 — имя, 1 — пароль
static int  s_shift = 0;
extern int  g_caps_on;     // Caps Lock: одно состояние на весь композитор (window.c)
static int  s_e0 = 0;
static const char* s_error = 0;

// Строка на системном языке (/etc/locale.conf)
#define GT(ru, en) (g_sys_lang == SYS_LANG_EN ? (en) : (ru))

static int g_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void g_strcpy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void wipe_pass(void) {
    volatile char* v = s_pass;
    for (int i = 0; i < (int)sizeof(s_pass); i++) v[i] = 0;
    s_pass_len = 0;
}

// Обычные пользователи (uid >= 1000) из /etc/passwd — как список в SDDM
static void load_users(void) {
    static char buf[4096];
    s_user_count = 0;
    int fd = sys_open("/etc/passwd", 0);
    if (fd < 0) return;
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char* line = buf;
    while (*line && s_user_count < MAX_GREETER_USERS) {
        char* end = line;
        while (*end && *end != '\n') end++;
        char saved = *end;
        *end = '\0';

        // name:x:uid:...
        char* f[4] = {line, 0, 0, 0};
        int k = 1;
        for (char* p = line; *p && k < 4; p++) {
            if (*p == ':') { *p = '\0'; f[k++] = p + 1; }
        }
        if (k >= 4 && f[0][0] && f[0][0] != '#') {
            int uid = 0;
            for (char* p = f[2]; *p >= '0' && *p <= '9'; p++) uid = uid * 10 + (*p - '0');
            if (uid >= 1000) {
                g_strcpy(s_users[s_user_count].name, f[0], 32);
                s_users[s_user_count].uid = uid;
                s_user_count++;
            }
        }
        *end = saved;
        line = *end ? end + 1 : end;
    }
}

// US-раскладка: имена и пароли латиницей.
// caps — Caps Lock; он меняет регистр только у букв, поэтому заменить им Shift
// целиком нельзя (с включённым Caps «1» давала бы «!»).
static char scancode_to_char(uint8_t sc, int shift, int caps) {
    static const char lo[] =
        "\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char hi[] =
        "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    if (sc >= sizeof(lo) - 1) return 0;
    if (caps && lo[sc] >= 'a' && lo[sc] <= 'z') shift = !shift;
    char c = shift ? hi[sc] : lo[sc];
    if (c == '\b' || c == '\t' || c == '\n' || c == '\033') return 0;
    return c;
}

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void select_user(int i) {
    s_sel_user = i;
    g_strcpy(s_name, s_users[i].name, sizeof(s_name));
    s_name_len = g_strlen(s_name);
    wipe_pass();
    s_focus = 1;              // имя уже выбрано — курсор сразу в пароле
    s_error = 0;
}

// Перечитать /etc/passwd, сохранив выбор: пользователя могли создать в консоли,
// пока экран входа уже открыт
static void refresh_users(void) {
    char keep[32];
    g_strcpy(keep, (s_sel_user >= 0 && s_sel_user < s_user_count) ? s_users[s_sel_user].name : "", sizeof(keep));
    int before = s_user_count;
    load_users();
    if (s_user_count == before) return;

    s_sel_user = -1;
    for (int i = 0; i < s_user_count; i++) {
        int same = 1;
        for (int k = 0; same; k++) {
            if (s_users[i].name[k] != keep[k]) same = 0;
            if (!keep[k]) break;
        }
        if (same) { s_sel_user = i; break; }
    }
    if (s_sel_user < 0 && keep[0] == '\0' && s_user_count > 0) s_sel_user = 0;
    if (s_sel_user >= 0) g_strcpy(s_name, s_users[s_sel_user].name, sizeof(s_name));
    s_name_len = g_strlen(s_name);
}

// Режим ручного ввода имени (учётной записи нет в списке)
static void select_other(void) {
    s_sel_user = -1;
    s_name[0] = '\0';
    s_name_len = 0;
    wipe_pass();
    s_focus = 0;
    s_error = 0;
}

// 1 — вход выполнен
static int try_login(void) {
    if (s_name_len == 0) {
        s_error = GT("Выберите пользователя или введите имя", "Pick a user or type a name");
        s_focus = 0;
        return 0;
    }
    int uid = sys_auth(s_name, s_pass);
    wipe_pass();
    if (uid < 0) {
        s_error = GT("Неверное имя пользователя или пароль", "Wrong username or password");
        s_focus = 1;
        return 0;
    }
    user_info_t info;
    if (!sys_getpwuid(uid, &info)) {
        s_error = GT("Ошибка учетной записи", "Account error");
        return 0;
    }
    g_session_uid = info.uid;
    g_session_gid = info.gid;
    g_strcpy(g_session_name, info.name, sizeof(g_session_name));
    s_error = 0;
    return 1;
}

static void draw_avatar(int cx, int cy, int rad, const char* name, int selected) {
    if (selected) gfx_fill_circle(cx, cy, rad + 4, COLOR_ACCENT_BLUE);
    gfx_fill_circle(cx, cy, rad, selected ? 0x2F3A44 : 0x262B30);
    char ch = name[0] ? name[0] : '?';
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    char one[2] = { ch, '\0' };
    gfx_draw_string_scaled(cx - gfx_text_width_lg(one) / 2, cy - 22, one, COLOR_TEXT_MAIN, 2);
}

static void draw_field(int x, int y, const char* text, const char* placeholder, int focused, int caret, int masked) {
    gfx_draw_rect(x - 1, y - 1, FIELD_W + 2, FIELD_H + 2, focused ? COLOR_ACCENT_BLUE : COLOR_PANEL_BORDER);
    gfx_draw_rect(x, y, FIELD_W, FIELD_H, 0x1B1E20);
    int tx = x + 12, ty = y + (FIELD_H - 16) / 2;
    int len = g_strlen(text);
    if (len == 0 && !focused) {
        gfx_draw_string(tx, ty, placeholder, COLOR_TEXT_MUTED);
    } else if (masked) {
        for (int i = 0; i < len && i < 34; i++) gfx_fill_circle(tx + i * 10 + 4, ty + 8, 3, COLOR_TEXT_MAIN);
        tx += (len < 34 ? len : 34) * 10;
    } else {
        gfx_draw_string(tx, ty, text, COLOR_TEXT_MAIN);
        tx += gfx_text_width(text);
    }
    if (focused && caret) gfx_draw_rect(tx + 1, ty, 2, 16, COLOR_TEXT_MAIN);
}

static void draw_button(int x, int y, int w, int h, const char* label, int hover, uint32_t bg) {
    gfx_draw_rect(x, y, w, h, hover ? (bg == COLOR_ACCENT_BLUE ? 0x5BBEF0 : COLOR_DOCK_HOVER) : bg);
    int tw = 0;
    for (const uint8_t* s = (const uint8_t*)label; *s; s++) if ((*s & 0xC0) != 0x80) tw += 8;
    gfx_draw_string(x + (w - tw) / 2, y + (h - 16) / 2, label, COLOR_TEXT_MAIN);
}

void greeter_run(void) {
    // Язык/пояс/раскладки могли поменяться (настройка в консоли, другой сеанс) — перечитываем
    sys_settings_t ss;
    if (sys_get_system(&ss) == 0) {
        g_sys_lang = ss.lang;
        g_has_layout2 = ss.layout2;
        if (ss.tz_index >= 0) g_tz_offset = ss.tz_offset;
        if (!g_has_layout2) g_layout_ru = 0;
    }
    load_users();
    wipe_pass();
    s_name[0] = '\0';
    s_name_len = 0;
    s_focus = 0;
    s_error = 0;
    if (s_user_count > 0) select_user(0);   // как SDDM: первый пользователь уже выбран
    else select_other();

    int prev_buttons = 0, prev_mx = -1, prev_my = -1;
    uint8_t prev_sec = 255;
    int frame = 0, caret = 1;
    rtc_time_t now = {0, 0, 0, 1, 1, 26};

    while (1) {
        int redraw = 0;
        int submit = 0;

        user_mouse_t ms;
        if (sys_get_mouse(&ms) == 0) { sys_sleep(10); continue; }
        int mx = ms.x, my = ms.y;
        int click = (ms.buttons & 1) && !(prev_buttons & 1);
        if (mx != prev_mx || my != prev_my || ms.buttons != prev_buttons) redraw = 1;

        sys_get_time(&now);
        if (now.sec != prev_sec) { prev_sec = now.sec; redraw = 1; }
        if (++frame % 50 == 0) { caret = !caret; redraw = 1; }
        if (frame % 200 == 0) {                 // ~2 с: список мог пополниться из консоли
            int was = s_user_count;
            refresh_users();
            if (s_user_count != was) redraw = 1;
        }

        int cx = (int)g_screen_w / 2;
        int cy = (int)g_screen_h / 2 + 40;
        int fx = cx - FIELD_W / 2;
        // Имя выбранного пользователя подписано под аватаром, поле имени нужно
        // только в режиме "Другой пользователь"
        int manual = (s_sel_user < 0);
        int name_y = cy + 10;
        int pass_y = manual ? cy + 56 : cy + 10;
        int btn_y  = pass_y + 48;
        int other_y = btn_y + 46;
        int avatar_y = cy - 90;
        int av_rad = (s_user_count > 4) ? 30 : 40;
        int av_gap = (s_user_count > 4) ? 92 : 120;
        int av_x0 = cx - ((s_user_count - 1) * av_gap) / 2;
        int pw_w = 130, pw_h = 32;
        int reboot_x = (int)g_screen_w - 2 * pw_w - 32, power_y = (int)g_screen_h - pw_h - 20;
        int off_x = (int)g_screen_w - pw_w - 20;

        // --- Клавиатура ---
        uint8_t sc;
        while ((sc = sys_get_key()) != 0) {
            redraw = 1;
            caret = 1; frame = 0;
            if (sc == 0xE0) { s_e0 = 1; continue; }
            if (s_e0) {
                s_e0 = 0;
                if (sc == 0x1C) submit = 1;                                  // Enter на цифровом блоке
                // Стрелки листают список пользователей, как в Plasma
                if (sc == 0x4B && s_sel_user > 0) select_user(s_sel_user - 1);
                if (sc == 0x4D && s_sel_user >= 0 && s_sel_user < s_user_count - 1) select_user(s_sel_user + 1);
                continue;
            }
            if (sc == 0x2A || sc == 0x36) { s_shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { s_shift = 0; continue; }
            if (sc == 0x3A) { g_caps_on = !g_caps_on; continue; }
            if (sc & 0x80) continue;

            if (sc == 0x1C) { submit = 1; continue; }
            if (sc == 0x0F) { if (manual) s_focus = !s_focus; continue; }   // Tab (есть куда переключать)
            if (sc == 0x01) { wipe_pass(); s_error = 0; continue; }         // Esc
            if (sc == 0x0E) {                                               // Backspace
                if (s_focus == 0 && s_name_len > 0) s_name[--s_name_len] = '\0';
                if (s_focus == 1 && s_pass_len > 0) s_pass[--s_pass_len] = '\0';
                continue;
            }
            char c = scancode_to_char(sc, s_shift, g_caps_on);
            if (!c) continue;
            if (!manual) s_focus = 1;             // имя выбрано списком — всё печатаем в пароль
            if (s_focus == 0 && s_name_len < (int)sizeof(s_name) - 1 && c != ' ' && c != ':') {
                s_name[s_name_len++] = c;
                s_name[s_name_len] = '\0';
            } else if (s_focus == 1 && s_pass_len < (int)sizeof(s_pass) - 1) {
                s_pass[s_pass_len++] = c;
                s_pass[s_pass_len] = '\0';
            }
            s_error = 0;
        }

        // --- Мышь ---
        if (click) {
            redraw = 1;
            for (int i = 0; i < s_user_count; i++) {
                if (inside(mx, my, av_x0 + i * av_gap - av_rad - 6, avatar_y - av_rad - 6,
                           2 * av_rad + 12, 2 * av_rad + 40)) select_user(i);
            }
            if (s_user_count > 0 && inside(mx, my, cx - 110, other_y, 220, 24)) select_other();
            if (manual && inside(mx, my, fx, name_y, FIELD_W, FIELD_H)) s_focus = 0;
            if (inside(mx, my, fx, pass_y, FIELD_W, FIELD_H)) s_focus = 1;
            if (inside(mx, my, fx, btn_y, FIELD_W, FIELD_H)) submit = 1;
            if (inside(mx, my, reboot_x, power_y, pw_w, pw_h)) sys_system_reboot();
            if (inside(mx, my, off_x, power_y, pw_w, pw_h)) sys_system_shutdown();
        }

        if (submit) {
            // Кадр GT("Вход...", "Login...") до проверки: после неудачи ядро держит паузу ~1 с
            gfx_draw_rect(fx, btn_y, FIELD_W, FIELD_H, 0x2A6F96);
            const char* wait_s = GT("Вход...", "Login...");
            gfx_draw_string(cx - gfx_text_width(wait_s) / 2, btn_y + 9, wait_s, COLOR_TEXT_MAIN);
            gfx_present();
            if (try_login()) return;
            redraw = 1;
        }

        prev_mx = mx; prev_my = my; prev_buttons = ms.buttons;
        if (!redraw) { sys_sleep(10); continue; }

        // --- Отрисовка ---
        gfx_draw_wallpaper();   // рисует все, кроме полосы панели задач — ее закрашиваем сами
        gfx_draw_rect(0, (int)g_screen_h - TASKBAR_HEIGHT, (int)g_screen_w, TASKBAR_HEIGHT, COLOR_DESKTOP_BG);
        gfx_darken(0, 0, (int)g_screen_w, (int)g_screen_h, 110);

        // Часы и дата
        char clock[6];
        int hour = ((now.hour + g_tz_offset) % 24 + 24) % 24;
        clock[0] = (char)('0' + hour / 10); clock[1] = (char)('0' + hour % 10);
        clock[2] = ':';
        clock[3] = (char)('0' + now.min / 10); clock[4] = (char)('0' + now.min % 10);
        clock[5] = '\0';
        int clock_scale = 6;
        int clock_y = (int)g_screen_h / 8;
        gfx_draw_string_scaled(cx - gfx_text_width_lg(clock) / 2, clock_y, clock, COLOR_TEXT_MAIN, clock_scale);
        int date_y = clock_y + 44;
        char date[16];
        date[0] = (char)('0' + now.day / 10);   date[1] = (char)('0' + now.day % 10);   date[2] = '.';
        date[3] = (char)('0' + now.month / 10); date[4] = (char)('0' + now.month % 10); date[5] = '.';
        date[6] = '2'; date[7] = '0';
        date[8] = (char)('0' + now.year / 10);  date[9] = (char)('0' + now.year % 10);  date[10] = '\0';
        gfx_draw_string(cx - gfx_text_width(date) / 2, date_y, date, COLOR_TEXT_MUTED);

        // Список пользователей: выбранный подсвечен и крупнее, под ним имя
        for (int i = 0; i < s_user_count; i++) {
            int ax = av_x0 + i * av_gap;
            int sel = (i == s_sel_user);
            int hot = inside(mx, my, ax - av_rad - 6, avatar_y - av_rad - 6, 2 * av_rad + 12, 2 * av_rad + 40);
            draw_avatar(ax, avatar_y, sel ? av_rad : av_rad - 4, s_users[i].name, sel);
            int nw = gfx_text_width(s_users[i].name);
            gfx_draw_string(ax - nw / 2, avatar_y + av_rad + 12, s_users[i].name,
                            sel ? COLOR_TEXT_MAIN : (hot ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED));
        }
        if (s_sel_user < 0) draw_avatar(cx, avatar_y, av_rad, s_name_len ? s_name : "?", s_user_count == 0);

        // Поле имени — только когда учётную запись вводят вручную
        if (manual) draw_field(fx, name_y, s_name, GT("Имя пользователя", "Username"), s_focus == 0, caret, 0);
        draw_field(fx, pass_y, s_pass, GT("Пароль", "Password"), s_focus == 1, caret, 1);
        // Caps Lock: пароль не виден, и без подсказки неверный ввод выглядит
        // ошибкой пароля — Plasma предупреждает здесь же
        if (g_caps_on) {
            const char* cl = GT("Caps Lock включен", "Caps Lock is on");
            gfx_draw_string(fx + FIELD_W + 12, pass_y + (FIELD_H - 16) / 2, cl, 0xFFC84B);
        }
        draw_button(fx, btn_y, FIELD_W, FIELD_H, GT("Войти", "Log in"), inside(mx, my, fx, btn_y, FIELD_W, FIELD_H), COLOR_ACCENT_BLUE);

        // Переключение на ручной ввод имени (учётной записи нет в списке)
        if (s_user_count > 0) {
            const char* other = manual ? GT("< Выбрать из списка", "< Back to the list")
                                       : GT("Другой пользователь...", "Other user...");
            int ow = 0;
            for (const uint8_t* t = (const uint8_t*)other; *t; t++) if ((*t & 0xC0) != 0x80) ow += 8;
            int hot = inside(mx, my, cx - 110, other_y, 220, 24);
            gfx_draw_string(cx - ow / 2, other_y + 4, other, hot ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
            gfx_draw_rect(cx - ow / 2, other_y + 21, ow, 1, hot ? COLOR_ACCENT_BLUE : COLOR_PANEL_BORDER);
        }

        if (s_error) {
            int ew = 0;
            for (const uint8_t* s = (const uint8_t*)s_error; *s; s++) if ((*s & 0xC0) != 0x80) ew += 8;
            gfx_draw_string(cx - ew / 2, other_y + 30, s_error, 0xFF6B6B);
        }

        // Низ экрана: система слева, питание справа
        gfx_draw_string(20, (int)g_screen_h - 36, "overOS", COLOR_TEXT_MAIN);
        gfx_draw_string(76, (int)g_screen_h - 36,
                        GT("Enter - войти, стрелки - выбрать пользователя, Esc - сброс",
                           "Enter - log in, arrows - pick a user, Esc - clear"), COLOR_TEXT_MUTED);
        draw_button(reboot_x, power_y, pw_w, pw_h, GT("Перезагрузка", "Restart"), inside(mx, my, reboot_x, power_y, pw_w, pw_h), COLOR_DOCK_ITEM_BG);
        draw_button(off_x, power_y, pw_w, pw_h, GT("Выключение", "Shut down"), inside(mx, my, off_x, power_y, pw_w, pw_h), COLOR_DOCK_ITEM_BG);

        gfx_draw_cursor(mx, my, WP_CUR_ARROW);
        gfx_present();
        sys_sleep(10);
    }
}

void session_logout(void) {
    // Закрываем программы сеанса (desktop — root, ему можно)
    wserver_close_all();
    int st;
    for (int tries = 0; tries < 50; tries++) {
        while (sys_waitpid(-1, &st, 1) > 0) { }
        sys_sleep(10);
    }

    g_session_uid = -1;
    g_session_gid = -1;
    g_session_name[0] = '\0';
    settings_set_user(-1, -1);          // greeter снова на общесистемном конфиге
    greeter_run();
}

// =============================================================================
// Первоначальная настройка (OOBE), как GNOME Initial Setup / Calamares:
// язык -> регион (часовой пояс, раскладка) -> пользователь-администратор -> готово.
// root отдельного пароля не получает (заблокирован): права — через sudo / пароль администратора.
// =============================================================================

#define OOBE_FIELDS 3
#define CARD_W 560
#define CARD_H 520
#define TZ_ROWS 8
#define TZ_ROW_H 24
#define TZ_MAX 40

static char s_ob_name[32], s_ob_pass[64], s_ob_pass2[64];
static char* const s_ob_buf[OOBE_FIELDS] = { s_ob_name, s_ob_pass, s_ob_pass2 };
static const int   s_ob_cap[OOBE_FIELDS] = { 32, 64, 64 };
static int s_ob_lang = SYS_LANG_RU;

static tz_info_t s_tz[TZ_MAX];
static int s_tz_count = 0;

#define TR(ru, en) (s_ob_lang == SYS_LANG_EN ? (en) : (ru))

static void ob_wipe(void) {
    for (int f = 1; f < OOBE_FIELDS; f++) {
        volatile char* v = s_ob_buf[f];
        for (int i = 0; i < s_ob_cap[f]; i++) v[i] = 0;
    }
}

static int ob_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int ob_valid_name(const char* n) {
    int len = g_strlen(n);
    if (len == 0 || len > 30 || n[0] == '-') return 0;
    for (int i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int text_w(const char* s) {
    int w = 0;
    for (const uint8_t* p = (const uint8_t*)s; *p; p++) if ((*p & 0xC0) != 0x80) w += 8;
    return w;
}

static void draw_center(int cx, int y, const char* s, uint32_t col) {
    gfx_draw_string(cx - text_w(s) / 2, y, s, col);
}

// Шаги вверху карточки
static void draw_steps(int cx, int y, int page) {
    const char* names[4] = { TR("Язык", "Language"), TR("Регион", "Region"),
                             TR("Пользователь", "User"), TR("Готово", "Done") };
    int gap = 130;
    for (int i = 0; i < 4; i++) {
        int x = cx + (2 * i - 3) * gap / 2;
        if (i < 3) gfx_draw_rect(x + 12, y + 9, gap - 24, 2, i < page ? COLOR_ACCENT_BLUE : COLOR_PANEL_BORDER);
        gfx_fill_circle(x, y + 10, 10, i <= page ? COLOR_ACCENT_BLUE : COLOR_PANEL_BORDER);
        gfx_draw_char(x - 4, y + 2, (uint8_t)('1' + i), COLOR_TEXT_MAIN);
        draw_center(x, y + 26, names[i], i == page ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
    }
}

// Радиокнопка с подписью
static void draw_radio(int x, int y, const char* label, int on, int hover) {
    gfx_fill_circle(x + 8, y + 8, 8, on ? COLOR_ACCENT_BLUE : (hover ? COLOR_DOCK_HOVER : COLOR_PANEL_BORDER));
    gfx_fill_circle(x + 8, y + 8, 6, on ? COLOR_ACCENT_BLUE : 0x1B1E20);
    if (on) gfx_fill_circle(x + 8, y + 8, 3, COLOR_TEXT_MAIN);
    gfx_draw_string(x + 24, y, label, on ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
}

static void fmt_offset(char* out, int off) {
    int p = 0;
    out[p++] = 'U'; out[p++] = 'T'; out[p++] = 'C';
    out[p++] = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    if (off >= 10) out[p++] = (char)('0' + off / 10);
    out[p++] = (char)('0' + off % 10);
    out[p] = '\0';
}

static void load_timezones(int* sel) {
    s_tz_count = sys_tz_info(-1, 0);
    if (s_tz_count > TZ_MAX) s_tz_count = TZ_MAX;
    *sel = 0;
    for (int i = 0; i < s_tz_count; i++) {
        sys_tz_info(i, &s_tz[i]);
        if (ob_streq(s_tz[i].name, "Europe/Moscow")) *sel = i;
    }
}

// 1 — пользователь создан и система настроена
static int oobe_create(int tz_sel, int layout2, const char** err) {
    if (!ob_valid_name(s_ob_name)) { *err = TR("Имя: строчные латинские буквы, цифры, _ и -", "Name: lowercase latin, digits, _ and -"); return 0; }
    if (s_ob_pass[0] == '\0')      { *err = TR("Пароль не может быть пустым", "Password cannot be empty"); return 0; }
    if (!ob_streq(s_ob_pass, s_ob_pass2)) { *err = TR("Пароли не совпадают", "Passwords do not match"); return 0; }

    if (sys_useradd(s_ob_name, s_ob_pass, 1) < 0) { *err = TR("Не удалось создать (имя занято?)", "Cannot create user (name taken?)"); return 0; }
    sys_lock_user("root");                              // root без пароля, как в Ubuntu/CachyOS
    sys_set_system(s_ob_lang, s_tz_count > 0 ? tz_sel : -1, layout2);
    sys_oobe_finish();

    // Настройки рабочего стола под выбор: часы и раскладка
    g_sys_lang = s_ob_lang;
    g_has_layout2 = layout2;
    if (s_tz_count > 0) g_tz_offset = s_tz[tz_sel].offset;
    g_layout_ru = 0;
    settings_save();
    return 1;
}

void oobe_run(void) {
    int page = 0;
    int focus = 0;
    #define TZ_CLAMP() do { \
        int _max = s_tz_count - TZ_ROWS; \
        if (_max < 0) _max = 0; \
        if (tz_top > _max) tz_top = _max; \
        if (tz_top < 0) tz_top = 0; \
    } while (0)
    int shift = 0, e0 = 0;
    const char* err = 0;
    int prev_buttons = 0, prev_mx = -1, prev_my = -1, frame = 0, caret = 1;
    int tz_sel = 0, tz_top = 0, layout2 = 1;
    int tz_drag = 0, tz_drag_off = 0;    // тянем бегунок полосы прокрутки
    for (int f = 0; f < OOBE_FIELDS; f++) s_ob_buf[f][0] = '\0';
    s_ob_lang = SYS_LANG_RU;
    load_timezones(&tz_sel);
    tz_top = tz_sel - TZ_ROWS / 2;

    while (1) {
        int redraw = 0, next = 0, back = 0;

        user_mouse_t ms;
        if (sys_get_mouse(&ms) == 0) { sys_sleep(10); continue; }
        int mx = ms.x, my = ms.y;
        int click = (ms.buttons & 1) && !(prev_buttons & 1);
        if (mx != prev_mx || my != prev_my || ms.buttons != prev_buttons) redraw = 1;
        if (++frame % 50 == 0) { caret = !caret; redraw = 1; }

        // Колесо: список часовых поясов листается по 3 строки за щелчок
        int wheel = sys_get_wheel();
        if (wheel && page == 1) { tz_top += wheel * 3; TZ_CLAMP(); redraw = 1; }

        // Настройку уже прошли в консоли (Alt+F2) — мастер больше не нужен
        if (page < 3 && frame % 100 == 0 && sys_oobe_state() != 1) { ob_wipe(); return; }

        int cx = (int)g_screen_w / 2;
        int card_x = cx - CARD_W / 2;
        int card_y = (int)g_screen_h / 2 - CARD_H / 2;
        int fx = cx - FIELD_W / 2;
        int btn_y = card_y + CARD_H - 56;
        int btn_w = 140;
        int next_x = card_x + CARD_W - btn_w - 24;
        int back_x = card_x + 24;

        // Геометрия страниц
        int lang_y = card_y + 210;                              // страница 0: две большие кнопки языка
        int list_x = cx - 210, list_w = 420, list_y = card_y + 136;
        int lay_y = list_y + TZ_ROWS * TZ_ROW_H + 40;           // страница 1: радиокнопки раскладки
        int sb_x = list_x + list_w - 24, sb_h = TZ_ROWS * TZ_ROW_H;
        int knob_h = (s_tz_count > TZ_ROWS) ? (sb_h * TZ_ROWS / s_tz_count) : sb_h;
        if (knob_h < 24) knob_h = 24;
        int knob_y = list_y + ((s_tz_count > TZ_ROWS)
                     ? (sb_h - knob_h) * tz_top / (s_tz_count - TZ_ROWS) : 0);

        // Тянем бегунок: позиция списка считается прямо из позиции мыши
        int held = (ms.buttons & 1);
        if (tz_drag) {
            if (!held) {
                tz_drag = 0;
            } else {
                int span = sb_h - knob_h;
                int rel = my - tz_drag_off - list_y;
                if (rel < 0) rel = 0;
                if (rel > span) rel = span;
                tz_top = (span > 0) ? (rel * (s_tz_count - TZ_ROWS) + span / 2) / span : 0;
                TZ_CLAMP();
                knob_y = list_y + ((s_tz_count > TZ_ROWS)
                         ? (sb_h - knob_h) * tz_top / (s_tz_count - TZ_ROWS) : 0);
                redraw = 1;
            }
        }
        int field_y0 = card_y + 150, field_gap = 62;            // страница 2: поля

        // --- Клавиатура ---
        uint8_t sc;
        while ((sc = sys_get_key()) != 0) {
            redraw = 1; caret = 1; frame = 0;
            if (sc == 0xE0) { e0 = 1; continue; }
            if (e0) {
                e0 = 0;
                if (sc == 0x1C) next = 1;
                if (page == 1 && s_tz_count > 0) {             // стрелки по списку поясов
                    if (sc == 0x48 && tz_sel > 0) tz_sel--;
                    if (sc == 0x50 && tz_sel < s_tz_count - 1) tz_sel++;
                    if (tz_sel < tz_top) tz_top = tz_sel;
                    if (tz_sel >= tz_top + TZ_ROWS) tz_top = tz_sel - TZ_ROWS + 1;
                    TZ_CLAMP();
                }
                if (page == 0 && (sc == 0x48 || sc == 0x50)) s_ob_lang = !s_ob_lang;
                continue;
            }
            if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
            if (sc == 0x3A) { g_caps_on = !g_caps_on; continue; }
            if (sc & 0x80) continue;
            if (sc == 0x1C) {
                if (page == 2 && focus < OOBE_FIELDS - 1) focus++;
                else next = 1;
                continue;
            }
            if (sc == 0x01 && page > 0 && page < 3) { back = 1; continue; }
            if (page != 2) continue;
            if (sc == 0x0F) { focus = (focus + (shift ? OOBE_FIELDS - 1 : 1)) % OOBE_FIELDS; continue; }
            char* buf = s_ob_buf[focus];
            int len = g_strlen(buf);
            if (sc == 0x0E) { if (len > 0) buf[len - 1] = '\0'; continue; }
            char c = scancode_to_char(sc, shift, g_caps_on);
            if (!c) continue;
            if (focus == 0) {
                if (c >= 'A' && c <= 'Z') c += 32;          // имена в Linux — строчные
                if (c == ' ' || c == ':') continue;
            }
            if (len < s_ob_cap[focus] - 1) { buf[len] = c; buf[len + 1] = '\0'; }
            err = 0;
        }

        // --- Мышь ---
        if (click) {
            redraw = 1;
            if (inside(mx, my, next_x, btn_y, btn_w, FIELD_H)) next = 1;
            if (page > 0 && page < 3 && inside(mx, my, back_x, btn_y, btn_w, FIELD_H)) back = 1;
            if (page == 0) {
                if (inside(mx, my, cx - 150, lang_y, 300, 40)) s_ob_lang = SYS_LANG_RU;
                if (inside(mx, my, cx - 150, lang_y + 52, 300, 40)) s_ob_lang = SYS_LANG_EN;
            } else if (page == 1) {
                for (int r = 0; r < TZ_ROWS && tz_top + r < s_tz_count; r++)
                    if (inside(mx, my, list_x, list_y + r * TZ_ROW_H, list_w - 28, TZ_ROW_H)) tz_sel = tz_top + r;

                // Полоса прокрутки: по бегунку — тянем, мимо — страница в эту сторону
                if (s_tz_count > TZ_ROWS && inside(mx, my, sb_x, list_y, 24, sb_h)) {
                    if (my >= knob_y && my < knob_y + knob_h) {
                        tz_drag = 1;
                        tz_drag_off = my - knob_y;
                    } else {
                        tz_top += (my < knob_y) ? -(TZ_ROWS - 1) : (TZ_ROWS - 1);
                        TZ_CLAMP();
                    }
                }
                if (inside(mx, my, list_x, lay_y + 24, 300, 20)) layout2 = 1;
                if (inside(mx, my, list_x, lay_y + 50, 300, 20)) layout2 = 0;
            } else if (page == 2) {
                for (int f = 0; f < OOBE_FIELDS; f++)
                    if (inside(mx, my, fx, field_y0 + f * field_gap, FIELD_W, FIELD_H)) focus = f;
            }
        }

        if (back) { page--; err = 0; }
        if (next) {
            if (page == 0 || page == 1) { page++; focus = 0; err = 0; }
            else if (page == 2) {
                if (oobe_create(tz_sel, layout2, &err)) { page = 3; ob_wipe(); }
            } else {
                ob_wipe();
                return;   // дальше — экран входа, новый пользователь уже выбран
            }
        }

        prev_mx = mx; prev_my = my; prev_buttons = ms.buttons;
        if (!redraw) { sys_sleep(10); continue; }

        // --- Отрисовка ---
        gfx_draw_wallpaper();
        gfx_draw_rect(0, (int)g_screen_h - TASKBAR_HEIGHT, (int)g_screen_w, TASKBAR_HEIGHT, COLOR_DESKTOP_BG);
        gfx_darken(0, 0, (int)g_screen_w, (int)g_screen_h, 110);

        gfx_draw_rect(card_x - 1, card_y - 1, CARD_W + 2, CARD_H + 2, COLOR_PANEL_BORDER);
        gfx_draw_rect(card_x, card_y, CARD_W, CARD_H, COLOR_PANEL_BG);
        draw_steps(cx, card_y + 20, page);
        gfx_draw_rect(card_x + 16, card_y + 74, CARD_W - 32, 1, COLOR_PANEL_BORDER);

        if (page == 0) {
            gfx_draw_string_scaled(cx - gfx_text_width_lg("overOS") / 2, card_y + 100, "overOS", COLOR_ACCENT_BLUE, 3);
            draw_center(cx, card_y + 172, TR("Добро пожаловать! Выберите язык системы", "Welcome! Choose the system language"), COLOR_TEXT_MAIN);
            const char* langs[2] = { "Русский", "English" };
            for (int i = 0; i < 2; i++) {
                int y = lang_y + i * 52;
                int on = (s_ob_lang == i);
                int hov = inside(mx, my, cx - 150, y, 300, 40);
                gfx_draw_rect(cx - 151, y - 1, 302, 42, on ? COLOR_ACCENT_BLUE : COLOR_PANEL_BORDER);
                gfx_draw_rect(cx - 150, y, 300, 40, on ? 0x2F3A44 : (hov ? COLOR_DOCK_HOVER : 0x1B1E20));
                draw_center(cx, y + 12, langs[i], COLOR_TEXT_MAIN);
            }
            draw_center(cx, lang_y + 120, TR("Интерфейс рабочего стола пока на русском", "Desktop UI is Russian-only for now"), COLOR_TEXT_MUTED);
        } else if (page == 1) {
            draw_center(cx, card_y + 92, TR("Часовой пояс (Регион/Город)", "Time zone (Region/City)"), COLOR_TEXT_MAIN);
            gfx_draw_rect(list_x - 1, list_y - 1, list_w + 2, TZ_ROWS * TZ_ROW_H + 2, COLOR_PANEL_BORDER);
            gfx_draw_rect(list_x, list_y, list_w, TZ_ROWS * TZ_ROW_H, 0x1B1E20);
            for (int r = 0; r < TZ_ROWS && tz_top + r < s_tz_count; r++) {
                int i = tz_top + r;
                int y = list_y + r * TZ_ROW_H;
                int hov = inside(mx, my, list_x, y, list_w - 28, TZ_ROW_H);
                if (i == tz_sel) gfx_draw_rect(list_x, y, list_w - 26, TZ_ROW_H, COLOR_ACCENT_BLUE);
                else if (hov) gfx_draw_rect(list_x, y, list_w - 26, TZ_ROW_H, COLOR_DOCK_HOVER);
                char off[8];
                fmt_offset(off, s_tz[i].offset);
                gfx_draw_string(list_x + 8, y + 4, off, i == tz_sel ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
                gfx_draw_string(list_x + 64, y + 4, s_ob_lang == SYS_LANG_EN ? s_tz[i].name : s_tz[i].city,
                                COLOR_TEXT_MAIN);
            }
            // Полоса прокрутки: бегунок тянется мышью, клик по дорожке листает страницу
            gfx_draw_rect(sb_x, list_y, 24, sb_h, COLOR_DOCK_ITEM_BG);
            if (s_tz_count > TZ_ROWS) {
                int hot = tz_drag || inside(mx, my, sb_x, knob_y, 24, knob_h);
                gfx_draw_rect(sb_x + 7, knob_y, 10, knob_h,
                              tz_drag ? COLOR_ACCENT_BLUE : (hot ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED));
            }
            if (s_tz_count > 0) {
                gfx_draw_string(list_x, list_y + sb_h + 8, s_tz[tz_sel].name, COLOR_TEXT_MUTED);
            }

            gfx_draw_string(list_x, lay_y, TR("Дополнительная раскладка (основная - English US):",
                                              "Additional layout (primary is English US):"), COLOR_TEXT_MAIN);
            draw_radio(list_x, lay_y + 24, TR("Русская (Alt+Shift)", "Russian (Alt+Shift)"), layout2 == 1, inside(mx, my, list_x, lay_y + 24, 300, 20));
            draw_radio(list_x, lay_y + 50, TR("Нет", "None"), layout2 == 0, inside(mx, my, list_x, lay_y + 50, 300, 20));
        } else if (page == 2) {
            const char* labels[OOBE_FIELDS] = {
                TR("Имя пользователя", "Username"), TR("Пароль", "Password"), TR("Повторите пароль", "Retype password")
            };
            const char* holders[OOBE_FIELDS] = { TR("например, kris", "e.g. kris"), "", "" };
            draw_center(cx, card_y + 92, TR("Создание пользователя", "Create your user"), COLOR_TEXT_MAIN);
            for (int f = 0; f < OOBE_FIELDS; f++) {
                int y = field_y0 + f * field_gap;
                gfx_draw_string(fx, y - 20, labels[f], COLOR_TEXT_MUTED);
                draw_field(fx, y, s_ob_buf[f], holders[f], focus == f, caret, f != 0);
            }
            draw_center(cx, field_y0 + 3 * field_gap - 4,
                        TR("Пользователь будет администратором: права root", "This user is an administrator: root access"),
                        COLOR_TEXT_MUTED);
            draw_center(cx, field_y0 + 3 * field_gap + 14,
                        TR("запрашиваются его же паролем (sudo).", "is granted with the same password (sudo)."),
                        COLOR_TEXT_MUTED);
            if (err) draw_center(cx, btn_y - 28, err, 0xFF6B6B);
        } else {
            gfx_fill_circle(cx, card_y + 160, 36, COLOR_ACCENT_BLUE);
            // Галочка: две толстые диагонали
            for (int i = 0; i <= 10; i++) gfx_draw_rect(cx - 16 + i, card_y + 158 + i, 5, 5, COLOR_TEXT_MAIN);
            for (int i = 0; i <= 20; i++) gfx_draw_rect(cx - 6 + i, card_y + 168 - i, 5, 5, COLOR_TEXT_MAIN);
            draw_center(cx, card_y + 220, TR("Все готово!", "All set!"), COLOR_TEXT_MAIN);
            draw_center(cx, card_y + 256, TR("Создан администратор:", "Administrator created:"), COLOR_TEXT_MUTED);
            draw_center(cx, card_y + 276, s_ob_name, COLOR_TEXT_MAIN);
            draw_center(cx, card_y + 316, TR("Войдите в систему со своим паролем.", "Log in with your password."), COLOR_TEXT_MUTED);
        }

        if (page > 0 && page < 3)
            draw_button(back_x, btn_y, btn_w, FIELD_H, TR("Назад", "Back"), inside(mx, my, back_x, btn_y, btn_w, FIELD_H), COLOR_DOCK_ITEM_BG);
        const char* next_label = (page < 2) ? TR("Далее", "Next") : (page == 2) ? TR("Создать", "Create") : TR("Начать", "Start");
        draw_button(next_x, btn_y, btn_w, FIELD_H, next_label, inside(mx, my, next_x, btn_y, btn_w, FIELD_H), COLOR_ACCENT_BLUE);

        gfx_draw_cursor(mx, my, WP_CUR_ARROW);
        gfx_present();
        sys_sleep(10);
    }
}
