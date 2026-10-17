// =============================================================================
// notifier.c — уведомления рабочего стола (по образцу Plasma).
//
// Откуда берутся:
//   * от системы -- ядро кладёт их в очередь (флешка подключена, можно
//     извлечь, флешка с системой отключена), мы забираем sys_notify_poll;
//   * от программ -- сообщение WP_NOTIFY по оконному протоколу (ovnotify.h).
//     Шапку (значок и имя) берём из исполняемого файла отправителя, а не из
//     сообщения, поэтому выдать себя за систему или чужую программу нельзя.
//
// Как выглядят: карточка в правом нижнем углу над панелью, новые снизу.
// Шапка -- значок и имя программы, справа крестик; под шапкой тонкая линия
// оставшегося времени. Дальше заголовок, текст (переносится по словам) и
// крупный значок справа, внизу -- кнопки действий. Пока мышь над карточкой,
// время не идёт. Критичные (флешка с системой отключена) не исчезают сами и
// отмечены оранжевым.
// =============================================================================

#include "notifier.h"
#include "gfx.h"
#include "theme.h"
#include "appinfo.h"
#include "syscalls.h"
#include "ovos.h"


#define NOTE_MAX      5
#define NOTE_W        364
#define NOTE_PAD      12
#define NOTE_HEAD_H   34
#define NOTE_ICON     48
#define NOTE_GAP      8
#define NOTE_MARGIN   10
#define NOTE_BTN_H    26
#define NOTE_TITLE_PX 16
#define NOTE_APP_ICON 20
#define NOTE_R        10
#define NOTE_BODY_PX  14
#define NOTE_LINE_H   19
#define NOTE_MAX_LINES 4
#define NOTE_DEFAULT_MS 6000

#define COL_ORANGE    0xF67400
#define COL_BODY      0xBDC3C7
#define COL_BTN       0x31363B
#define COL_BTN_HOT   0x3B4248
#define COL_BTN_EDGE  0x4D5257

typedef struct {
    int      used;
    int      kernel;            // 1 -- от системы, 0 -- от программы
    int      owner;             // pid программы (для действий)
    uint32_t id;                // id у источника (ядро или программа)
    int      urgency;
    char     app_name[64];
    char     app_icon[96];      // значок в шапке: путь к .prg или имя в /sys/icons
    char     icon[24];          // крупный справа, "" -- нет
    char     title[64];
    char     body[200];
    char     act_label[2][20];
    char     act_cmd[2][64];    // у системных: что сделать
    int      total_ms;          // 0 -- без таймаута
    int      left_ms;
    uint32_t seq;               // порядок появления
    // раскладка, считается при отрисовке
    int      x, y, h;
    int      nlines;
    int      lstart[NOTE_MAX_LINES], llen[NOTE_MAX_LINES];
} note_t;

static note_t   s_note[NOTE_MAX];
static uint32_t s_seq = 0;
static uint64_t s_last_ms = 0, s_last_poll = 0;

static void ncopy(char* d, const char* s, int max) { ov_strlcpy(d, s ? s : "", max); }

void notifier_init(void) {
    ov_memset(s_note, 0, sizeof(s_note));
    s_last_ms = ov_ms();
    // Значки, которые понадобятся, когда корневой флешки может уже не быть:
    // "флешка с системой отключена" надо нарисовать, не читая диск.
    gfx_icon("dialog-warning", NOTE_ICON);
    gfx_icon("drive-removable", NOTE_ICON);
    gfx_icon("notifications", NOTE_APP_ICON);
}

// Свободная карточка или та, что заменяется (тот же источник и id)
static note_t* note_slot(int kernel, int owner, uint32_t id) {
    for (int i = 0; i < NOTE_MAX; i++)
        if (s_note[i].used && s_note[i].kernel == kernel && s_note[i].owner == owner && s_note[i].id == id)
            return &s_note[i];
    note_t* oldest = 0;
    for (int i = 0; i < NOTE_MAX; i++) {
        if (!s_note[i].used) return &s_note[i];
        // мест нет -- уступает самое старое некритичное
        if (s_note[i].urgency != WP_URGENCY_CRITICAL && (!oldest || s_note[i].seq < oldest->seq)) oldest = &s_note[i];
    }
    return oldest ? oldest : &s_note[0];
}

static void note_arm(note_t* n, int timeout_s) {
    n->used = 1;
    n->seq = ++s_seq;
    n->total_ms = (n->urgency == WP_URGENCY_CRITICAL) ? 0 :
                  (timeout_s > 0 ? timeout_s * 1000 : NOTE_DEFAULT_MS);
    n->left_ms = n->total_ms;
}

// ------------------------------------------------------------- источники
static void take_kernel(const ov_knotify_t* k) {
    if (k->close) {
        for (int i = 0; i < NOTE_MAX; i++)
            if (s_note[i].used && s_note[i].kernel && s_note[i].id == k->id) s_note[i].used = 0;
        return;
    }
    note_t* n = note_slot(1, 0, k->id);
    ov_memset(n, 0, sizeof(*n));
    n->kernel = 1;
    n->id = k->id;
    n->urgency = k->urgency;
    ncopy(n->app_name, "Система", sizeof(n->app_name));
    ncopy(n->app_icon, "notifications", sizeof(n->app_icon));
    ncopy(n->icon, k->icon, sizeof(n->icon));
    ncopy(n->title, k->title, sizeof(n->title));
    ncopy(n->body, k->body, sizeof(n->body));
    for (int a = 0; a < 2; a++) {
        ncopy(n->act_label[a], k->act_label[a], sizeof(n->act_label[a]));
        ncopy(n->act_cmd[a], k->act_cmd[a], sizeof(n->act_cmd[a]));
    }
    note_arm(n, k->timeout_s);
}

void notifier_post_app(const wp_notify_t* m, int sender_pid) {
    char exe[96];
    if (ov_proc_exe(sender_pid, exe, sizeof(exe)) != 0) exe[0] = '\0';
    const appinfo_t* ai = appinfo(exe);
    note_t* n = note_slot(0, sender_pid, m->id);
    ov_memset(n, 0, sizeof(*n));
    n->kernel = 0;
    n->owner = sender_pid;
    n->id = m->id;
    n->urgency = m->urgency > WP_URGENCY_CRITICAL ? WP_URGENCY_NORMAL : m->urgency;
    ncopy(n->app_name, exe[0] ? ai->name : "Программа", sizeof(n->app_name));
    ncopy(n->app_icon, ai->has_icon ? exe : "dialog-information", sizeof(n->app_icon));
    // Крупный значок: просили конкретный -- его, иначе значок самой программы
    if (m->icon[0]) ncopy(n->icon, m->icon, sizeof(n->icon));
    char t[64]; ncopy(t, m->title, sizeof(t)); ncopy(n->title, t, sizeof(n->title));
    char b[120]; ncopy(b, m->body, sizeof(b)); ncopy(n->body, b, sizeof(n->body));
    for (int a = 0; a < 2; a++) {
        char l[16]; ncopy(l, m->actions[a], sizeof(l));
        ncopy(n->act_label[a], l, sizeof(n->act_label[a]));
    }
    note_arm(n, m->timeout_s);
}

void notifier_close_app(uint32_t id, int sender_pid) {
    for (int i = 0; i < NOTE_MAX; i++)
        if (s_note[i].used && !s_note[i].kernel && s_note[i].owner == sender_pid && s_note[i].id == id)
            s_note[i].used = 0;
}

void notifier_clear_apps(void) {
    for (int i = 0; i < NOTE_MAX; i++) if (s_note[i].used && !s_note[i].kernel) s_note[i].used = 0;
}

// ------------------------------------------------------------- раскладка
// Перенос по словам в ширину w: начала и длины строк в байтах. Длинное слово
// без пробелов режется по символам.
static int wrap(const char* s, int w, int* st, int* ln, int max_lines) {
    int n = 0, pos = 0, len = 0;
    while (s[len]) len++;
    while (pos < len && n < max_lines) {
        while (s[pos] == ' ') pos++;
        if (pos >= len) break;
        int best = -1;              // последний пробел, до которого влезает
        int cut = pos;              // последний символ, до которого влезает
        char line[160];
        int i = pos;
        while (i < len) {
            int cl = 1;
            while (i + cl < len && (((uint8_t)s[i + cl]) & 0xC0) == 0x80) cl++;
            int ll = i + cl - pos;
            if (ll >= (int)sizeof(line)) break;
            for (int k = 0; k < ll; k++) line[k] = s[pos + k];
            line[ll] = '\0';
            if (gfx_text_width_px(line, NOTE_BODY_PX) > w) break;
            i += cl;
            cut = i;
            if (i < len && s[i] == ' ') best = i;
        }
        int end = (cut >= len) ? len : (best > pos ? best : cut);
        if (end <= pos) end = cut > pos ? cut : len;
        st[n] = pos;
        ln[n] = end - pos;
        n++;
        pos = end;
    }
    // Не влезло -- последнюю строку закончим многоточием при отрисовке
    return n;
}

static int text_w(const note_t* n) {
    return NOTE_W - 2 * NOTE_PAD - (n->icon[0] ? NOTE_ICON + NOTE_PAD : 0);
}

static int has_actions(const note_t* n) { return n->act_label[0][0] || n->act_label[1][0]; }

static void layout(void) {
    int bar_y = (int)g_screen_h - TASKBAR_HEIGHT;
    int y = bar_y - NOTE_MARGIN;
    // Новые -- снизу, ближе к панели: идём от самого свежего
    uint32_t done_seq = 0xFFFFFFFF;
    for (int k = 0; k < NOTE_MAX; k++) {
        note_t* best = 0;
        for (int i = 0; i < NOTE_MAX; i++)
            if (s_note[i].used && s_note[i].seq < done_seq && (!best || s_note[i].seq > best->seq)) best = &s_note[i];
        if (!best) break;
        done_seq = best->seq;
        best->nlines = wrap(best->body, text_w(best), best->lstart, best->llen, NOTE_MAX_LINES);
        int content = 24 + best->nlines * NOTE_LINE_H;
        if (best->icon[0] && content < NOTE_ICON) content = NOTE_ICON;
        best->h = NOTE_HEAD_H + 8 + content + (has_actions(best) ? NOTE_BTN_H + 12 : 0) + NOTE_PAD;
        best->x = (int)g_screen_w - NOTE_W - NOTE_MARGIN;
        best->y = y - best->h;
        if (best->y < 8) { best->h = 0; continue; }       // не влезло на экран -- подождёт
        y = best->y - NOTE_GAP;
    }
}

static int hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void close_box(const note_t* n, int* x, int* y) { *x = n->x + NOTE_W - 32; *y = n->y + (NOTE_HEAD_H - 20) / 2; }

// Кнопки действий прижаты вправо, как в Plasma
static int btn_rect(const note_t* n, int a, int* bx, int* by, int* bw) {
    if (!n->act_label[a][0]) return 0;
    int x = n->x + NOTE_W - NOTE_PAD;
    for (int k = 1; k >= a; k--) {
        if (!n->act_label[k][0]) continue;
        int w = gfx_text_width(n->act_label[k]) + 28;
        x -= w;
        if (k == a) { *bx = x; *by = n->y + n->h - NOTE_PAD - NOTE_BTN_H; *bw = w; return 1; }
        x -= 8;
    }
    return 0;
}

// ------------------------------------------------------------- время
int notifier_tick(int mx, int my) {
    uint64_t now = ov_ms();
    int dt = (int)(now - s_last_ms);
    s_last_ms = now;
    // Время идёт, только пока рабочий стол на экране: пока человек в TTY, цикл
    // композитора сюда не заходит, и без этого ограничения уведомление,
    // пришедшее в это время, гасло бы в первом же кадре после возвращения.
    if (dt > 100) dt = 100;
    int changed = 0;

    // Очередь ядра -- четыре раза в секунду: вызов дешёвый, на шину не ходит
    if (now - s_last_poll >= 250) {
        s_last_poll = now;
        ov_knotify_t k;
        for (int guard = 0; guard < 8 && ov_notify_poll(&k) == 1; guard++) { take_kernel(&k); changed = 1; }
    }

    layout();
    for (int i = 0; i < NOTE_MAX; i++) {
        note_t* n = &s_note[i];
        if (!n->used || !n->total_ms || !n->h) continue;
        if (hit(mx, my, n->x, n->y, NOTE_W, n->h)) continue;   // мышь над карточкой -- время стоит
        // Сравниваем ширину полосы в пикселях: по 1/40 она дёргалась шагами
        // почти в сантиметр раз в 150 мс.
        int before = (int)((int64_t)(NOTE_W - 2) * n->left_ms / n->total_ms);
        n->left_ms -= dt;
        if (n->left_ms <= 0) { n->used = 0; changed = 1; continue; }
        if ((int)((int64_t)(NOTE_W - 2) * n->left_ms / n->total_ms) != before) changed = 1;
    }
    return changed;
}

// ------------------------------------------------------------- отрисовка
static void draw_x(int cx, int cy, uint32_t col) {
    for (int t = -1; t <= 0; t++) {
        gfx_line(cx - 5, cy - 5 + t, cx + 5, cy + 5 + t, col);
        gfx_line(cx - 5, cy + 5 + t, cx + 5, cy - 5 + t, col);
    }
}

static void draw_note(const note_t* n, int mx, int my) {
    int x = n->x, y = n->y, h = n->h;
    int crit = (n->urgency == WP_URGENCY_CRITICAL);
    uint32_t accent = crit ? COL_ORANGE : COLOR_ACCENT_BLUE;

    // Тень повторяет форму карточки: раньше затемнялся прямоугольник, и из-под
    // скруглённых углов выглядывали тёмные квадраты.
    gfx_shadow_round(x, y + 2, NOTE_W, h, NOTE_R, 8, 120);
    gfx_round_rect_aa(x, y, NOTE_W, h, NOTE_R, crit ? COL_ORANGE : COLOR_PANEL_BORDER);
    gfx_round_rect_aa(x + 1, y + 1, NOTE_W - 2, h - 2, NOTE_R - 1, COLOR_PANEL_BG);

    // Шапка: значок и имя программы, справа крестик
    int ii = gfx_icon(n->app_icon, NOTE_APP_ICON);
    if (ii >= 0) gfx_draw_icon(x + NOTE_PAD, y + (NOTE_HEAD_H - NOTE_APP_ICON) / 2, ii, COLOR_TEXT_MAIN);
    gfx_draw_string(x + NOTE_PAD + NOTE_APP_ICON + 8, y + (NOTE_HEAD_H - 16) / 2, n->app_name, COLOR_TEXT_MUTED);
    int cx, cy;
    close_box(n, &cx, &cy);
    if (hit(mx, my, cx, cy, 22, 20)) gfx_round_rect_aa(cx, cy, 22, 20, 5, COLOR_WIN_BTN_HOVER);
    draw_x(cx + 11, cy + 10, COLOR_TEXT_MAIN);

    // Сколько осталось: полоса тает справа налево (у критичных стоит целиком)
    int line_w = NOTE_W - 2;
    if (n->total_ms) line_w = (int)((int64_t)(NOTE_W - 2) * n->left_ms / n->total_ms);
    gfx_draw_rect(x + 1, y + NOTE_HEAD_H, NOTE_W - 2, 1, COLOR_PANEL_BORDER);
    if (line_w > 0) gfx_draw_rect(x + 1, y + NOTE_HEAD_H - 1, line_w, 2, accent);

    // Заголовок (жирнее за счёт второго прохода со сдвигом) и текст
    int tx = x + NOTE_PAD, ty = y + NOTE_HEAD_H + 8;
    gfx_draw_string_px(tx, ty, n->title, COLOR_TEXT_MAIN, NOTE_TITLE_PX);
    for (int l = 0; l < n->nlines; l++) {
        char line[160];
        int len = n->llen[l];
        if (len > (int)sizeof(line) - 4) len = (int)sizeof(line) - 4;
        for (int k = 0; k < len; k++) line[k] = n->body[n->lstart[l] + k];
        line[len] = '\0';
        // Последняя строка, а текст не кончился -- многоточие
        if (l == n->nlines - 1 && n->body[n->lstart[l] + n->llen[l]] && len < (int)sizeof(line) - 4) {
            line[len++] = '.'; line[len++] = '.'; line[len++] = '.'; line[len] = '\0';
        }
        gfx_draw_string_px(tx, ty + 24 + l * NOTE_LINE_H, line, COL_BODY, NOTE_BODY_PX);
    }

    if (n->icon[0]) {
        int bi = gfx_icon(n->icon, NOTE_ICON);
        if (bi >= 0) gfx_draw_icon(x + NOTE_W - NOTE_PAD - NOTE_ICON, ty, bi, 0xFFFFFF);
    }

    for (int a = 0; a < 2; a++) {
        int bx, by, bw;
        if (!btn_rect(n, a, &bx, &by, &bw)) continue;
        int hot = hit(mx, my, bx, by, bw, NOTE_BTN_H);
        gfx_round_rect_aa(bx - 1, by - 1, bw + 2, NOTE_BTN_H + 2, 5, hot ? COLOR_ACCENT_BLUE : COL_BTN_EDGE);
        gfx_round_rect_aa(bx, by, bw, NOTE_BTN_H, 4, hot ? COL_BTN_HOT : COL_BTN);
        gfx_draw_string(bx + 14, by + 5, n->act_label[a], COLOR_TEXT_MAIN);
    }
}

void notifier_render(int mx, int my) {
    layout();
    for (int i = 0; i < NOTE_MAX; i++) if (s_note[i].used && s_note[i].h) draw_note(&s_note[i], mx, my);
}

int notifier_inside(int mx, int my) {
    for (int i = 0; i < NOTE_MAX; i++) {
        const note_t* n = &s_note[i];
        if (n->used && n->h && hit(mx, my, n->x, n->y, NOTE_W, n->h)) return 1;
    }
    return 0;
}

// Задевает ли карточка (вместе с тенью) прямоугольник: тогда окно под ней
// нельзя обновлять отдельно от всего экрана
int notifier_overlaps(int x, int y, int w, int h) {
    const int m = 12;
    for (int i = 0; i < NOTE_MAX; i++) {
        const note_t* n = &s_note[i];
        if (!n->used || !n->h) continue;
        if (n->x - m < x + w && x < n->x + NOTE_W + m && n->y - m < y + h && y < n->y + n->h + m) return 1;
    }
    return 0;
}

// ------------------------------------------------------------- действия
extern int wserver_hand_over(const char* app, const char* path);

// Системные команды: "open:/media/X" -- открыть в файловом менеджере (уже
// открытому отдаём путь, как activation в XDG), "eject:X" -- извлечь
static void run_cmd(const char* cmd) {
    if (cmd[0] == 'o' && cmd[1] == 'p' && cmd[2] == 'e' && cmd[3] == 'n' && cmd[4] == ':') {
        const char* path = cmd + 5;
        if (!wserver_hand_over("ovfiles", path)) sys_spawn_as_session("/desprogs/ovfiles.prg", path);
    } else if (cmd[0] == 'e' && cmd[1] == 'j' && cmd[2] == 'e' && cmd[3] == 'c' && cmd[4] == 't' && cmd[5] == ':') {
        ov_eject(cmd + 6);
    }
}

static void act(note_t* n, int a) {
    if (n->kernel) {
        int k = (a < 0) ? 0 : a;                     // щелчок по карточке -- первое действие
        if (n->act_cmd[k][0]) run_cmd(n->act_cmd[k]);
    } else {
        wp_notify_action_t ev = { WP_EV_NOTIFY_ACTION, n->id, a };
        ov_send(n->owner, &ev, sizeof(ev));
    }
    n->used = 0;
}

int notifier_click(int mx, int my) {
    layout();
    for (int i = 0; i < NOTE_MAX; i++) {
        note_t* n = &s_note[i];
        if (!n->used || !n->h || !hit(mx, my, n->x, n->y, NOTE_W, n->h)) continue;
        int cx, cy;
        close_box(n, &cx, &cy);
        if (hit(mx, my, cx, cy, 22, 20)) { n->used = 0; return 1; }
        for (int a = 0; a < 2; a++) {
            int bx, by, bw;
            if (btn_rect(n, a, &bx, &by, &bw) && hit(mx, my, bx, by, bw, NOTE_BTN_H)) { act(n, a); return 1; }
        }
        // Щелчок по самой карточке: у критичной -- ничего (её убирает только
        // причина), у остальных -- действие по умолчанию
        if (n->urgency != WP_URGENCY_CRITICAL) act(n, -1);
        return 1;
    }
    return 0;
}
