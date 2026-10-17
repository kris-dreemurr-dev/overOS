// Notepad — текстовый редактор overOS на тулките libui (src/libs/libui/ui.h).
// Текст хранится в UTF-8: кириллицу и раскладку даёт композитор (WP_EV_KEY.text).
#include <stdint.h>
#include <stddef.h>
#include "libui/ui.h"
#include "libui/libui_ll.h"

#define WIN_W 600
#define WIN_H 380

// Окно разворачивается, поэтому раскладка считается от текущего размера
static int g_ww = WIN_W, g_wh = WIN_H;

#define TOOLBAR_H 40
#define STATUS_H  24
#define GUTTER_W  44
#define LINE_H    18
#define TEXT_X    (GUTTER_W + 10)
#define TEXT_Y    (TOOLBAR_H + 6)

static const uint16_t icon_notepad[16] = {
    0b0000000000000000, 0b0001111111100000, 0b0001000000110000, 0b0001000000101000,
    0b0001000000111000, 0b0001011110100000, 0b0001000000100000, 0b0001011110100000,
    0b0001000000100000, 0b0001011110100000, 0b0001000000100000, 0b0001011110100000,
    0b0001000000100000, 0b0001111111100000, 0b0000000000000000, 0b0000000000000000
};

#define MAX_TEXT_LEN (64 * 1024)
static char s_text[MAX_TEXT_LEN];
static int  s_len = 0;
static int  s_cursor = 0;          // байтовая позиция (всегда на границе символа UTF-8)
static int  s_scroll = 0;          // первая видимая строка
static int  s_modified = 0;
static char s_path[128] = "/home/notes.txt";
static char s_status[64] = "Готово";

static void set_status(const char* s) { ov_strlcpy(s_status, s, sizeof(s_status)); }

// --- UTF-8 ---
static int is_cont(char c) { return (((uint8_t)c) & 0xC0) == 0x80; }
static int next_cp(int i) { if (i < s_len) i++; while (i < s_len && is_cont(s_text[i])) i++; return i; }
static int prev_cp(int i) { if (i > 0) i--; while (i > 0 && is_cont(s_text[i])) i--; return i; }

static int line_start(int i) { while (i > 0 && s_text[i - 1] != '\n') i--; return i; }
static int line_end(int i)   { while (i < s_len && s_text[i] != '\n') i++; return i; }

// Колонка (в символах) позиции i в её строке
static int column_of(int i) {
    int c = 0;
    for (int p = line_start(i); p < i; p = next_cp(p)) c++;
    return c;
}
// Позиция в строке, начинающейся с ls, на колонке col (или конец строки)
static int pos_at_column(int ls, int col) {
    int p = ls, le = line_end(ls);
    while (col-- > 0 && p < le) p = next_cp(p);
    return p;
}
static int line_index_of(int pos) {
    int n = 0;
    for (int i = 0; i < pos; i++) if (s_text[i] == '\n') n++;
    return n;
}
static int line_start_by_index(int idx) {
    int i = 0;
    while (idx > 0 && i < s_len) { if (s_text[i] == '\n') idx--; i++; }
    return i;
}

// --- Правка ---
static void insert_bytes(const char* b, int n) {
    if (s_len + n >= MAX_TEXT_LEN) return;
    for (int i = s_len - 1; i >= s_cursor; i--) s_text[i + n] = s_text[i];
    for (int i = 0; i < n; i++) s_text[s_cursor + i] = b[i];
    s_cursor += n;
    s_len += n;
    s_text[s_len] = '\0';
    s_modified = 1;
}

static void delete_range(int a, int b) {
    if (b <= a) return;
    int n = b - a;
    for (int i = a; i + n <= s_len; i++) s_text[i] = s_text[i + n];
    s_len -= n;
    s_cursor = a;
    s_modified = 1;
}

// --- Файл ---
static void load_file(const char* path) {
    int fd = ov_open(path, OV_O_RDONLY);
    if (fd < 0) { set_status("Новый файл"); return; }
    int64_t rd = ov_read(fd, s_text, MAX_TEXT_LEN - 1);
    ov_close(fd);
    if (rd < 0) { set_status("Ошибка чтения"); return; }
    s_len = (int)rd;
    s_text[s_len] = '\0';
    s_cursor = 0;
    s_scroll = 0;
    s_modified = 0;
    set_status("Открыт");
}

static void save_file(void) {
    int fd = ov_open(s_path, OV_O_WRONLY | OV_O_CREAT);
    if (fd < 0) { set_status("Нет доступа к файлу"); return; }
    int64_t wr = ov_write(fd, s_text, (uint64_t)s_len);
    ov_close(fd);
    if (wr == s_len) { s_modified = 0; set_status("Сохранено"); }
    else set_status("Ошибка записи");
}

static void handle_keys(ui_win_t* w) {
    int ctrl = ui_mods(w) & WP_MOD_CTRL;
    if (ctrl && ui_key_pressed(w, 0x1F)) { save_file(); return; }   // Ctrl+S

    if (ui_key_pressed(w, WP_KEY_LEFT))  s_cursor = prev_cp(s_cursor);
    if (ui_key_pressed(w, WP_KEY_RIGHT)) s_cursor = next_cp(s_cursor);
    if (ui_key_pressed(w, WP_KEY_HOME))  s_cursor = line_start(s_cursor);
    if (ui_key_pressed(w, WP_KEY_END))   s_cursor = line_end(s_cursor);
    if (ui_key_pressed(w, WP_KEY_UP)) {
        int ls = line_start(s_cursor);
        if (ls > 0) s_cursor = pos_at_column(line_start(ls - 1), column_of(s_cursor));
    }
    if (ui_key_pressed(w, WP_KEY_DOWN)) {
        int le = line_end(s_cursor);
        if (le < s_len) s_cursor = pos_at_column(le + 1, column_of(s_cursor));
    }
    if (ui_key_pressed(w, WP_KEY_BACKSPACE) && s_cursor > 0) delete_range(prev_cp(s_cursor), s_cursor);
    if (ui_key_pressed(w, WP_KEY_DELETE) && s_cursor < s_len) delete_range(s_cursor, next_cp(s_cursor));
    if (ui_key_pressed(w, WP_KEY_ENTER)) insert_bytes("\n", 1);
    if (ui_key_pressed(w, WP_KEY_TAB)) insert_bytes("    ", 4);

    const char* t = ui_typed(w);
    if (t[0] && !ctrl) insert_bytes(t, ov_strlen(t));
}

int main(void) {
    if (libui_bind() < 0) ov_exit(1);     // тулкит берётся из /lib/libui.ll
    char arg[128];
    if (ov_get_arg(arg, sizeof(arg)) > 0 && arg[0]) {
        ov_strlcpy(s_path, arg, sizeof(s_path));
        load_file(s_path);
    }

    ui_app_id("notepad");       // следующий файл откроется в этом же окне

    ui_win_t* w = ui_open("Notepad — Текстовый редактор", WIN_W, WIN_H, icon_notepad, WP_FLAG_RESIZABLE);
    if (!w) ov_exit(1);

    int visible = (g_wh - TEXT_Y - STATUS_H) / LINE_H;

    while (ui_begin(w, UI_WAIT)) {
        g_ww = ui_width(w);                            // окно могли развернуть
        g_wh = ui_height(w);
        visible = (g_wh - TEXT_Y - STATUS_H) / LINE_H;

        const char* req = ui_open_request(w);          // файл от overFiles
        if (req[0]) {
            ov_strlcpy(s_path, req, sizeof(s_path));
            load_file(s_path);
        }
        int cursor_before = s_cursor;

        // Панель: сохранить, открыть, путь
        ui_fill(w, 0, 0, g_ww, TOOLBAR_H, UI_PANEL);
        ui_hline(w, 0, TOOLBAR_H, g_ww, UI_BORDER);
        int save = ui_button_accent(w, 8, 7, 100, 26, "Сохранить");
        int open = ui_button(w, 114, 7, 90, 26, "Открыть");
        // Поле пути: Enter — открыть файл
        if (ui_textbox(w, 212, 7, g_ww - 220, s_path, sizeof(s_path))) open = 1;
        if (save) save_file();
        if (open) load_file(s_path);

        // Редактор получает клавиши, если не печатают в поле пути
        int editor_click = ui_mouse_clicked(w) && ui_hit(w, 0, TOOLBAR_H, g_ww, g_wh - TOOLBAR_H - STATUS_H);
        static int editor_focus = 1;
        if (ui_mouse_clicked(w)) editor_focus = editor_click;
        if (editor_focus) handle_keys(w);

        // Клик по тексту — курсор туда
        if (editor_click) {
            int line = s_scroll + (ui_mouse_y(w) - TEXT_Y) / LINE_H;
            int col = (ui_mouse_x(w) - TEXT_X + UI_MONO_W / 2) / UI_MONO_W;
            if (col < 0) col = 0;
            int total = line_index_of(s_len);
            if (line > total) line = total;
            if (line < 0) line = 0;
            s_cursor = pos_at_column(line_start_by_index(line), col);
        }

        // Прокрутка: колесо, и курсор всегда на экране
        s_scroll += ui_scroll(w) * 3;
        int cur_line = line_index_of(s_cursor);
        if (s_cursor != cursor_before) {
            if (cur_line < s_scroll) s_scroll = cur_line;
            if (cur_line >= s_scroll + visible) s_scroll = cur_line - visible + 1;
        }
        int total_lines = line_index_of(s_len) + 1;
        if (s_scroll > total_lines - 1) s_scroll = total_lines - 1;
        if (s_scroll < 0) s_scroll = 0;

        // Текст
        ui_fill(w, 0, TOOLBAR_H + 1, g_ww, g_wh - TOOLBAR_H - 1 - STATUS_H, UI_BG_DARK);
        ui_fill(w, 0, TOOLBAR_H + 1, GUTTER_W, g_wh - TOOLBAR_H - 1 - STATUS_H, UI_BG);
        int p = line_start_by_index(s_scroll);
        for (int row = 0; row < visible; row++) {
            int ln = s_scroll + row;
            if (ln >= total_lines) break;
            int y = TEXT_Y + row * LINE_H;
            char num[12];
            ui_itoa(ln + 1, num);
            ui_text(w, GUTTER_W - 8 - ui_text_width(num), y, num, ln == cur_line ? UI_TEXT : UI_TEXT_MUTED);

            int le = line_end(p);
            int x = TEXT_X;
            for (int i = p; i <= le; i = (i < le) ? next_cp(i) : le + 1) {
                if (i == s_cursor && editor_focus) ui_fill(w, x, y, 2, 16, UI_ACCENT);
                if (i == le) break;
                if (x < g_ww - 8) {
                    char ch[5];
                    int n = next_cp(i) - i;
                    for (int k = 0; k < n && k < 4; k++) ch[k] = s_text[i + k];
                    ch[n < 4 ? n : 4] = '\0';
                    ui_text_mono(w, x, y, ch, UI_TEXT);
                }
                x += UI_MONO_W;
            }
            p = le + 1;
        }

        // Статус
        int sy = g_wh - STATUS_H;
        ui_fill(w, 0, sy, g_ww, STATUS_H, UI_PANEL);
        ui_hline(w, 0, sy, g_ww, UI_BORDER);
        ui_label_muted(w, 10, sy + 4, s_status);
        char info[48], n1[12], n2[12];
        ui_itoa(cur_line + 1, n1);
        ui_itoa(column_of(s_cursor) + 1, n2);
        int q = 0;
        info[0] = '\0';
        const char* parts[] = { s_modified ? "* " : "", "Стр ", n1, ", стб ", n2 };
        for (int k = 0; k < 5; k++) for (const char* c = parts[k]; *c; c++) info[q++] = *c;
        info[q] = '\0';
        ui_text(w, g_ww - 10 - ui_text_width(info), sy + 4, info, UI_TEXT);

        ui_end(w);
    }

    ui_close(w);
    ov_exit(0);
    return 0;
}
