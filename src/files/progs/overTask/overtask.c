// =============================================================================
// overTask — диспетчер задач overOS.
//
// Показывает таблицу процессов (сисколл 113), их состояние, владельца, память и
// долю процессора, сверху — общую нагрузку: ЦП, ОЗУ (сисколл 128) и накопитель
// со скоростью обмена (сисколл 131). Обновляется раз в секунду и умеет снимать
// выбранный процесс (сисколл 33, как kill(1)).
//
// Проценты ЦП считаются как в Linux: ядро начисляет каждый тик таймера той
// задаче, которая была на процессоре, а мы делим прирост этого счётчика на
// прошедшее время. Простой — это доля pid 0 (его цикл стоит на hlt), поэтому
// общая загрузка складывается из всех остальных задач. Своего пользователя снять можно всегда, чужого — только root,
// это проверяет ядро, а программа просто показывает отказ.
//
// Строки колонок печатаются моноширинным шрифтом (ui_text_mono): в таблице
// колонки должны стоять друг под другом, а шрифт интерфейса пропорциональный.
// =============================================================================

#include "common/ovos.h"
#include "libui/ui.h"
#include "libui/libui_ll.h"

#define MAX_PROC   128
#define ROW_H      20
#define REFRESH_MS 1000

static const uint16_t icon_task[16] = {
    0x0000, 0x7FFE, 0x4002, 0x4002, 0x5552, 0x4002, 0x4AA2, 0x4002,
    0x5552, 0x4002, 0x4AA2, 0x4002, 0x4002, 0x7FFE, 0x0000, 0x0000
};

static ov_proc_t s_proc[MAX_PROC];
static int       s_count;
static int       s_cpu_pct[MAX_PROC];      // доля ЦП процесса за последний интервал
static int       s_prev_pid[MAX_PROC];     // снимок прошлого обновления
static uint64_t  s_prev_cpu[MAX_PROC];
static int       s_prev_n;
static uint64_t  s_prev_ms;
static int       s_cpu_total;              // общая загрузка, %
static ov_meminfo_t s_mem;
static ov_diskinfo_t s_disk, s_disk_prev;
static int       s_disk_kbs;               // обмен с накопителем, КБ/с
static int       s_sel = -1;      // pid выбранного процесса (не индекс: список шевелится)
static int       s_scroll;
static char      s_msg[96];
static uint64_t  s_msg_until;
static uint64_t  s_next_refresh;
static int       s_me;

static void msg(const char* text) {
    int i = 0;
    while (text[i] && i < (int)sizeof(s_msg) - 1) { s_msg[i] = text[i]; i++; }
    s_msg[i] = '\0';
    s_msg_until = ov_ms() + 4000;
}

static const char* state_name(int st) {
    switch (st) {
        case 0: return "готов";
        case 1: return "работает";
        case 2: return "спит";
        case 3: return "мёртв";      // TASK_DEAD
        default: return "зомби";     // TASK_ZOMBIE — ждёт wait() от родителя
    }
}

// Имя владельца по uid (root знаем сразу, остальных спрашиваем у ядра)
static const char* user_name(int uid) {
    static char buf[32];
    if (uid == 0) return "root";
    ov_user_t u;
    if (ov_getpwuid(uid, &u) > 0 && u.name[0]) {
        int i = 0;
        while (u.name[i] && i < (int)sizeof(buf) - 1) { buf[i] = u.name[i]; i++; }
        buf[i] = '\0';
        return buf;
    }
    ui_itoa(uid, buf);
    return buf;
}

static void append(char* dst, int* pos, const char* src, int cap) {
    while (*src && *pos < cap - 1) dst[(*pos)++] = *src++;
    dst[*pos] = '\0';
}

// Сколько символов в строке UTF-8 (а не байт: русская буква занимает два)
static int utf8_chars(const char* s) {
    int n = 0;
    for (int i = 0; s[i]; i++) if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
    return n;
}

// Дополнить строку пробелами до колонки n. Колонка считается в символах:
// моноширинный шрифт рисует 8 px на символ, а в байтах русские слова длиннее,
// и колонки «СОСТОЯНИЕ» и «ПАМЯТЬ» наезжали друг на друга.
static void pad_to(char* dst, int* pos, int n, int cap) {
    int chars = utf8_chars(dst);
    while (chars < n && *pos < cap - 1) { dst[(*pos)++] = ' '; chars++; }
    dst[*pos] = '\0';
}

static void refresh(void) {
    uint64_t now = ov_ms();
    s_count = ov_ps(s_proc, MAX_PROC);
    if (s_count < 0) s_count = 0;

    uint64_t dt = (s_prev_ms && now > s_prev_ms) ? (now - s_prev_ms) : 0;
    uint64_t busy = 0;
    for (int i = 0; i < s_count; i++) {
        uint64_t prev = 0;
        for (int k = 0; k < s_prev_n; k++)
            if (s_prev_pid[k] == s_proc[i].pid) { prev = s_prev_cpu[k]; break; }
        uint64_t d = (s_proc[i].cpu_ms > prev) ? (s_proc[i].cpu_ms - prev) : 0;
        int pct = dt ? (int)((d * 100) / dt) : 0;
        s_cpu_pct[i] = pct > 100 ? 100 : pct;
        if (s_proc[i].pid != 0) busy += d;      // pid 0 — простой системы
    }
    s_cpu_total = dt ? (int)((busy * 100) / dt) : 0;
    if (s_cpu_total > 100) s_cpu_total = 100;

    s_prev_n = s_count;
    for (int i = 0; i < s_count; i++) { s_prev_pid[i] = s_proc[i].pid; s_prev_cpu[i] = s_proc[i].cpu_ms; }

    ov_meminfo(&s_mem);

    ov_diskinfo_t d;
    if (ov_diskinfo(&d) == 0) {
        if (dt) {
            uint64_t moved = (d.read_kb + d.write_kb) - (s_disk_prev.read_kb + s_disk_prev.write_kb);
            s_disk_kbs = (int)((moved * 1000) / dt);
        }
        s_disk_prev = d;
        s_disk = d;
    }

    s_prev_ms = now;
    s_next_refresh = now + REFRESH_MS;
}

// Полоска загрузки: подпись, значение и сама шкала. Цвет — как в fastfetch:
// зелёный до половины, жёлтый до 80%, красный выше.
static void meter(ui_win_t* w, int x, int y, int bw, const char* title,
                  int pct, const char* value) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t col = (pct > 80) ? UI_DANGER : (pct > 50) ? 0xE5C07B : UI_SUCCESS;
    ui_label(w, x, y, title);
    ui_text(w, x + bw - ui_text_width(value), y, value, UI_TEXT_MUTED);
    ui_fill(w, x, y + 20, bw, 8, UI_PANEL);
    ui_fill(w, x, y + 20, bw * pct / 100, 8, col);
}

static int sel_index(void) {
    for (int i = 0; i < s_count; i++) if (s_proc[i].pid == s_sel) return i;
    return -1;
}

int main(void) {
    if (libui_bind() < 0) ov_exit(1);
    ui_app_id("ovtask");

    ui_win_t* w = ui_open("Диспетчер задач", 640, 460, icon_task, WP_FLAG_RESIZABLE);
    if (!w) ov_exit(1);

    s_me = ov_getpid();
    refresh();

    while (ui_begin(w, 250)) {
        if (ov_ms() >= s_next_refresh) refresh();

        int win_w = ui_width(w), win_h = ui_height(w);
        int list_y = 136, bottom_h = 54;
        int list_h = win_h - list_y - bottom_h;
        if (list_h < ROW_H) list_h = ROW_H;
        int rows = list_h / ROW_H;

        // --- ввод ---
        int sel_i = sel_index();
        s_scroll -= ui_scroll(w) * -1;            // колесо вниз — список вниз
        if (ui_key_pressed(w, WP_KEY_DOWN) && sel_i + 1 < s_count) s_sel = s_proc[sel_i + 1].pid;
        if (ui_key_pressed(w, WP_KEY_UP)   && sel_i > 0)           s_sel = s_proc[sel_i - 1].pid;
        if (s_scroll > s_count - rows) s_scroll = s_count - rows;
        if (s_scroll < 0) s_scroll = 0;

        if (ui_mouse_clicked(w)) {
            int my = ui_mouse_y(w), mx = ui_mouse_x(w);
            if (my >= list_y && my < list_y + list_h && mx < win_w) {
                int idx = s_scroll + (my - list_y) / ROW_H;
                if (idx >= 0 && idx < s_count) s_sel = s_proc[idx].pid;
            }
        }

        // --- рисование ---
        ui_clear(w);
        ui_fill(w, 0, 0, win_w, 40, UI_PANEL);
        ui_hline(w, 0, 40, win_w, UI_BORDER);
        ui_icon16(w, 14, 12, icon_task, UI_ACCENT);

        char head[64];
        int p = 0;
        head[0] = '\0';
        char num[16];
        ui_itoa(s_count, num);
        append(head, &p, "Процессов: ", (int)sizeof(head));
        append(head, &p, num, (int)sizeof(head));
        ui_label(w, 40, 12, head);

        if (ui_button(w, win_w - 110, 8, 96, 24, "Обновить")) refresh();

        // --- панель нагрузки ---
        {
            int gap = 12;
            int bw = (win_w - gap * 4) / 3;
            if (bw < 90) bw = 90;
            int bx = gap, by2 = 52;
            char val[40];

            ui_itoa(s_cpu_total, num);
            val[0] = '\0'; int vp = 0;
            append(val, &vp, num, (int)sizeof(val));
            append(val, &vp, "%", (int)sizeof(val));
            meter(w, bx, by2, bw, "Процессор", s_cpu_total, val);

            int mem_pct = s_mem.total ? (int)((s_mem.used * 100) / s_mem.total) : 0;
            vp = 0; val[0] = '\0';
            ui_itoa((int)(s_mem.used >> 20), num);  append(val, &vp, num, (int)sizeof(val));
            append(val, &vp, " / ", (int)sizeof(val));
            ui_itoa((int)(s_mem.total >> 20), num); append(val, &vp, num, (int)sizeof(val));
            append(val, &vp, " МБ", (int)sizeof(val));
            meter(w, bx + bw + gap, by2, bw, "Память", mem_pct, val);

            int disk_pct = s_disk.total_mb ? (int)((s_disk.used_mb * 100) / s_disk.total_mb) : 0;
            vp = 0; val[0] = '\0';
            ui_itoa((int)s_disk.used_mb, num);  append(val, &vp, num, (int)sizeof(val));
            append(val, &vp, " / ", (int)sizeof(val));
            ui_itoa((int)s_disk.total_mb, num); append(val, &vp, num, (int)sizeof(val));
            append(val, &vp, " МБ", (int)sizeof(val));
            meter(w, bx + (bw + gap) * 2, by2, bw, "Накопитель", disk_pct, val);

            // Под шкалой накопителя — скорость обмена: по ней видно, что
            // тормозит именно флешка, а не процессор.
            vp = 0; val[0] = '\0';
            append(val, &vp, "обмен ", (int)sizeof(val));
            ui_itoa(s_disk_kbs, num); append(val, &vp, num, (int)sizeof(val));
            append(val, &vp, " КБ/с", (int)sizeof(val));
            ui_label_muted(w, bx + (bw + gap) * 2, by2 + 32, val);

            ui_hline(w, 0, list_y - 30, win_w, UI_BORDER);
        }

        // Заголовок таблицы
        char row[160];
        p = 0; row[0] = '\0';
        append(row, &p, "PID", (int)sizeof(row));      pad_to(row, &p, 7, (int)sizeof(row));
        append(row, &p, "ИМЯ", (int)sizeof(row));      pad_to(row, &p, 26, (int)sizeof(row));
        append(row, &p, "ВЛАДЕЛЕЦ", (int)sizeof(row)); pad_to(row, &p, 40, (int)sizeof(row));
        append(row, &p, "СОСТОЯНИЕ", (int)sizeof(row));pad_to(row, &p, 53, (int)sizeof(row));
        append(row, &p, "ЦП", (int)sizeof(row));       pad_to(row, &p, 60, (int)sizeof(row));
        append(row, &p, "ПАМЯТЬ", (int)sizeof(row));
        ui_text_mono(w, 12, list_y - 22, row, UI_TEXT_MUTED);
        ui_hline(w, 0, list_y - 4, win_w, UI_BORDER);

        for (int r = 0; r < rows; r++) {
            int idx = s_scroll + r;
            if (idx >= s_count) break;
            const ov_proc_t* pr = &s_proc[idx];
            int y = list_y + r * ROW_H;
            int selected = (pr->pid == s_sel);
            if (selected) ui_fill(w, 0, y, win_w, ROW_H, UI_ACCENT);

            p = 0; row[0] = '\0';
            ui_itoa(pr->pid, num);
            append(row, &p, num, (int)sizeof(row));                 pad_to(row, &p, 7, (int)sizeof(row));
            append(row, &p, pr->name[0] ? pr->name : "-", (int)sizeof(row));
            pad_to(row, &p, 26, (int)sizeof(row));
            append(row, &p, user_name(pr->uid), (int)sizeof(row));  pad_to(row, &p, 40, (int)sizeof(row));
            append(row, &p, state_name(pr->state), (int)sizeof(row));
            pad_to(row, &p, 53, (int)sizeof(row));
            ui_itoa(s_cpu_pct[idx], num);
            append(row, &p, num, (int)sizeof(row));
            append(row, &p, "%", (int)sizeof(row));
            pad_to(row, &p, 60, (int)sizeof(row));
            ui_itoa((int)pr->mem_kb, num);
            append(row, &p, num, (int)sizeof(row));
            append(row, &p, " КБ", (int)sizeof(row));
            uint32_t col = selected ? 0xFFFFFF
                         : (pr->state == 3) ? UI_DANGER
                         : (pr->pid == s_me) ? UI_ACCENT : UI_TEXT;
            ui_text_mono(w, 12, y + 2, row, col);
        }

        // Полоса прокрутки
        if (s_count > rows) {
            int track_h = list_h;
            int bar_h = track_h * rows / s_count;
            if (bar_h < 20) bar_h = 20;
            int bar_y = list_y + (track_h - bar_h) * s_scroll / (s_count - rows);
            ui_fill(w, win_w - 6, list_y, 4, track_h, UI_PANEL);
            ui_fill(w, win_w - 6, bar_y, 4, bar_h, UI_BORDER);
        }

        // --- низ окна ---
        int by = win_h - bottom_h + 12;
        ui_hline(w, 0, win_h - bottom_h, win_w, UI_BORDER);
        sel_i = sel_index();
        if (sel_i >= 0) {
            if (ui_button(w, 12, by, 150, 28, "Снять задачу")) {
                int pid = s_proc[sel_i].pid;
                if (pid == s_me) msg("Это сам диспетчер задач.");
                else if (ov_kill(pid, 9) == 0) { msg("Процесс снят."); refresh(); }
                else msg("Не удалось снять: нет прав.");
            }
            if (s_msg_until > ov_ms()) ui_text(w, 180, by + 6, s_msg, UI_TEXT_MUTED);
        } else {
            ui_label_muted(w, 12, by + 6, "Выберите процесс в списке");
        }

        ui_end(w);
    }

    ui_close(w);
    ov_exit(0);
    return 0;
}
