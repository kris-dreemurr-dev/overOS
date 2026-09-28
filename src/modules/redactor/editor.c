#include "redactor.h"

#define MAX_TABS         16
#define CODE_START_X     56

#define COLOR_BG           0x1E1E1E
#define COLOR_TOPBAR_BG    0x252526
#define COLOR_TAB_INACT    0x2D2D2D
#define COLOR_TAB_ACT      0x1E1E1E
#define COLOR_STATUS_BG    0x007ACC
#define COLOR_GUTTER_INACT 0x606060
#define COLOR_GUTTER_ACT   0xC6C6C6
#define COLOR_TEXT         0xD4D4D4
#define COLOR_CURSOR       0x00FF88

// VS Code Syntax Colors
#define VS_KEYWORD   0x569CD6
#define VS_TYPE      0x4EC9B0
#define VS_STRING    0xCE9178
#define VS_PREPROC   0xC586C0
#define VS_COMMENT   0x6A9955
#define VS_NUMBER    0xB5CEA8
#define VS_TEXT      0xD4D4D4

typedef struct {
    char name[16];
    char fat_name[12];
    char dir_path[64]; // Путь директории файла
    char text[EDITOR_MAX_LINES][EDITOR_MAX_COLS];
    int cursor_row;
    int cursor_col;
    int scroll_row;
    int is_modified;
    int file_type; // 0 = Text, 1 = .c, 2 = .h
} editor_tab_t;

static editor_tab_t tabs[MAX_TABS];
static int tab_count = 0;
static int active_tab = 0;

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int caps_lock = 0;
static int kbd_layout = 0;

static char status_msg[64] = "Ready";
char text_buf[EDITOR_MAX_LINES][EDITOR_MAX_COLS];

static void my_strcpy(char* dst, const char* src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static int my_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int my_strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void kputc_at(int x, int y, char c, uint32_t color) {
    char str[2];
    str[0] = c;
    str[1] = '\0';
    kputs_at(x, y, str, color);
}

static const char ascii_normal[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0, '*',   0, ' '
};

static const char ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
     0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',   0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0, '*',   0, ' '
};

static const uint8_t scancode_ru[128] = {
    0,   27,  '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '-',  '=', '\b',
  '\t', 0xA9, 0xE6, 0xE3, 0xAA, 0xA5, 0xAD, 0xA3, 0xE8, 0xE9, 0xA7, 0xE5, 0xEA, '\n',
     0, 0xE4, 0xEB, 0xA2, 0xA0, 0xAF, 0xE0, 0xAE, 0xAB, 0xA4, 0xA6, 0xED, 0xA5,    0,
  '\\', 0xEF, 0xE7, 0xE1, 0xAC, 0xA8, 0xE2, 0xEC, 0xA1, 0xEE,  '.',    0,  '*',    0,  ' '
};

static const uint8_t scancode_ru_shift[128] = {
    0,   27,  '!',  '"',  0xFC, ';',  '%',  ':',  '?',  '*',  '(',  ')',  '_',  '+', '\b',
  '\t', 0x89, 0x96, 0x93, 0x8A, 0x85, 0x8D, 0x83, 0x98, 0x99, 0x87, 0x95, 0x9A, '\n',
     0, 0x94, 0x9B, 0x82, 0x80, 0x8F, 0x90, 0x8E, 0x8B, 0x84, 0x86, 0x9D, 0x85,    0,
   '/', 0x9F, 0x97, 0x91, 0x8C, 0x88, 0x92, 0x9C, 0x81, 0x9E,  ',',    0,  '*',    0,  ' '
};

int line_length(int row) {
    if (row < 0 || row >= EDITOR_MAX_LINES || tab_count == 0) return 0;
    int len = 0;
    while (len < EDITOR_MAX_COLS - 1 && tabs[active_tab].text[row][len] != '\0') len++;
    return len;
}

static int get_visible_lines(void) {
    int usable_height = (int)screen_height - 28 - 26;
    int lines = usable_height / 18;
    if (lines < 5) lines = 5;
    if (lines > EDITOR_MAX_LINES) lines = EDITOR_MAX_LINES;
    return lines;
}

static int get_total_lines(void) {
    if (tab_count == 0) return 1;
    int total = 1;
    for (int r = EDITOR_MAX_LINES - 1; r >= 0; r--) {
        if (tabs[active_tab].text[r][0] != '\0') { total = r + 1; break; }
    }
    if (tabs[active_tab].cursor_row + 1 > total) total = tabs[active_tab].cursor_row + 1;
    return total;
}

static void update_scroll(void) {
    if (tab_count == 0) return;
    int vis = get_visible_lines();
    if (tabs[active_tab].cursor_row < tabs[active_tab].scroll_row) {
        tabs[active_tab].scroll_row = tabs[active_tab].cursor_row;
    }
    if (tabs[active_tab].cursor_row >= tabs[active_tab].scroll_row + vis) {
        tabs[active_tab].scroll_row = tabs[active_tab].cursor_row - vis + 1;
    }
}

static void sync_to_text_buf(void) {
    if (tab_count == 0) return;
    for (int r = 0; r < EDITOR_MAX_LINES; r++) {
        for (int c = 0; c < EDITOR_MAX_COLS; c++) {
            text_buf[r][c] = tabs[active_tab].text[r][c];
        }
    }
}

static int get_file_type(const char* filename) {
    int len = my_strlen(filename);
    if (len > 2) {
        char ext1 = filename[len - 2];
        char ext2 = filename[len - 1];
        if (ext1 == '.' || ext1 == '_') {
            if (ext2 == 'c' || ext2 == 'C') return 1;
            if (ext2 == 'h' || ext2 == 'H') return 2;
        }
    }
    return 0;
}

static void to_fat83(const char* in, char* out83) {
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    out83[11] = '\0';
    int i = 0;
    while (*in && *in != '.' && i < 8) {
        char c = *in++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[i++] = c;
    }
    while (*in && *in != '.') in++;
    if (*in == '.') in++;
    int j = 8;
    while (*in && j < 11) {
        char c = *in++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[j++] = c;
    }
}

static void navigate_to_dir(const char* path) {
    if (!fat16_is_mounted()) return;
    fat16_go_root();

    // Если корень — фиксируем "/" и выходим
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        my_strcpy(current_path, "/");
        return;
    }

    const char* p = path;
    if (*p == '/') p++;

    while (*p) {
        char part[16];
        int i = 0;
        while (*p && *p != '/' && i < 15) part[i++] = *p++;
        part[i] = '\0';
        if (*p == '/') p++;

        if (part[0] == '\0') continue;

        char n83[12];
        to_fat83(part, n83);

        if (!fat16_change_dir(n83)) {
            fat16_make_folder(n83);
            fat16_change_dir(n83);
        }
    }

    // Всегда синхронизируем системный указатель current_path с реальным путем
    my_strcpy(current_path, path);
}

static void split_path(const char* full_path, char* out_dir, char* out_file) {
    int len = my_strlen(full_path);
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (full_path[i] == '/') last_slash = i;
    }
    if (last_slash == -1) {
        my_strcpy(out_dir, current_path);
        my_strcpy(out_file, full_path);
    } else if (last_slash == 0) {
        my_strcpy(out_dir, "/");
        my_strcpy(out_file, full_path + 1);
    } else {
        int i;
        for (i = 0; i < last_slash && i < 63; i++) out_dir[i] = full_path[i];
        out_dir[i] = '\0';
        my_strcpy(out_file, full_path + last_slash + 1);
    }
}

static void load_buffer_to_tab(int t, const char* buf, int bytes) {
    for (int r = 0; r < EDITOR_MAX_LINES; r++) {
        for (int c = 0; c < EDITOR_MAX_COLS; c++) tabs[t].text[r][c] = '\0';
    }
    int row = 0, col = 0;
    uint8_t utf8_pfx = 0;
    for (int i = 0; i < bytes; i++) {
        uint8_t ch = (uint8_t)buf[i];
        if (ch == '\r') continue;
        if (ch == 0xD0 || ch == 0xD1) { utf8_pfx = ch; continue; }
        if (utf8_pfx) {
            if (utf8_pfx == 0xD0 && (ch >= 0x90 && ch <= 0xBF)) ch = ch - 0x90 + 0x80;
            else if (utf8_pfx == 0xD1 && (ch >= 0x80 && ch <= 0x8F)) ch = ch - 0x80 + 0xE0;
            else if (utf8_pfx == 0xD0 && ch == 0x81) ch = 0x85;
            else if (utf8_pfx == 0xD1 && ch == 0x91) ch = 0xA5;
            else ch = '?';
            utf8_pfx = 0;
        }
        if (ch == '\n') {
            tabs[t].text[row][col] = '\0';
            row++; col = 0;
            if (row >= EDITOR_MAX_LINES) break;
        } else {
            if (col < EDITOR_MAX_COLS - 1) tabs[t].text[row][col++] = (char)ch;
        }
    }
    tabs[t].cursor_row = 0; tabs[t].cursor_col = 0;
    tabs[t].scroll_row = 0; tabs[t].is_modified = 0;
}

static void load_single_file(const char* path) {
    char dir[64], file[32];
    split_path(path, dir, file);
    navigate_to_dir(dir);

    tab_count = 1;
    active_tab = 0;
    my_strcpy(tabs[0].dir_path, dir);

    int flen = 0;
    while (file[flen] && flen < 15) {
        char c = file[flen];
        if (c >= 'A' && c <= 'Z') c += 32;
        tabs[0].name[flen] = c;
        flen++;
    }
    tabs[0].name[flen] = '\0';
    tabs[0].file_type = get_file_type(tabs[0].name);
    to_fat83(file, tabs[0].fat_name);

    static char load_buf[EDITOR_MAX_LINES * EDITOR_MAX_COLS];
    int r = fat16_read_file(tabs[0].fat_name, load_buf, sizeof(load_buf) - 1);
    if (r > 0) {
        load_buf[r] = '\0';
        load_buffer_to_tab(0, load_buf, r);
    } else {
        load_buffer_to_tab(0, "", 0);
        tabs[0].is_modified = 1;
    }
}

static void create_project(const char* proj_name) {
    char proj_dir[64] = "/CODE/";
    int pidx = 6;
    for (int i = 0; proj_name[i] && pidx < 60; i++) {
        char c = proj_name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        proj_dir[pidx++] = c;
    }
    proj_dir[pidx] = '\0';

    navigate_to_dir(proj_dir);

    const char* template_c =
        "void main() {\n"
        "    print(\"devOS Project Started!\\n\");\n"
        "    int a = 20;\n"
        "    int b = 30;\n"
        "    print(\"Sum: \");\n"
        "    print_num(a + b);\n"
        "    print(\"\\n\");\n"
        "}\n";

    to_fat83("MAIN.C", tabs[0].fat_name);
    fat16_write_file(tabs[0].fat_name, template_c, my_strlen(template_c));

    tab_count = 1;
    active_tab = 0;
    my_strcpy(tabs[0].dir_path, proj_dir);
    my_strcpy(tabs[0].name, "main.c");
    tabs[0].file_type = 1;

    load_buffer_to_tab(0, template_c, my_strlen(template_c));
}

static void scan_and_load_dir(const char* dir_path) {
    if (!fat16_is_mounted()) fat16_mount(0);
    navigate_to_dir(dir_path);

    static fat16_file_info_api_t file_list[MAX_TABS];
    int raw_count = fat16_get_dir_files(file_list, MAX_TABS);

    tab_count = 0;
    static char load_buf[EDITOR_MAX_LINES * EDITOR_MAX_COLS];

    for (int t = 0; t < raw_count && tab_count < MAX_TABS; t++) {
        // Пропускаем системные записи каталогов FAT16: "." и ".."
        if (file_list[t].name83[0] == '.' || file_list[t].name83[0] == ' ' || file_list[t].name83[0] == '\0') {
            continue;
        }

        char base[9], ext[4];
        int b_len = 0;
        for (int i = 0; i < 8; i++) {
            char c = file_list[t].name83[i];
            if (c != ' ' && c != '\0') {
                if (c >= 'A' && c <= 'Z') c += 32;
                base[b_len++] = c;
            }
        }
        base[b_len] = '\0';

        int e_len = 0;
        for (int i = 8; i < 11; i++) {
            char c = file_list[t].name83[i];
            if (c != ' ' && c != '\0') {
                if (c >= 'A' && c <= 'Z') c += 32;
                ext[e_len++] = c;
            }
        }
        ext[e_len] = '\0';

        // Если это папка (нет расширения и это не корень проекта), не открываем её как текстовый файл
        if (e_len == 0 && my_strcmp(base, "main") != 0 && my_strcmp(base, "test") != 0) {
            // Это вложенная директория — пропускаем
            continue;
        }

        int cur_tab = tab_count++;
        my_strcpy(tabs[cur_tab].dir_path, dir_path);

        int idx = 0;
        for (int i = 0; base[i] && idx < 12; i++) tabs[cur_tab].name[idx++] = base[i];
        if (e_len > 0) {
            tabs[cur_tab].name[idx++] = '.';
            for (int i = 0; ext[i] && idx < 15; i++) tabs[cur_tab].name[idx++] = ext[i];
        }
        tabs[cur_tab].name[idx] = '\0';
        tabs[cur_tab].file_type = get_file_type(tabs[cur_tab].name);

        for (int f = 0; f < 11; f++) tabs[cur_tab].fat_name[f] = file_list[t].name83[f];
        tabs[cur_tab].fat_name[11] = '\0';

        int read_bytes = fat16_read_file(tabs[cur_tab].fat_name, load_buf, sizeof(load_buf) - 1);
        if (read_bytes > 0) {
            load_buf[read_bytes] = '\0';
            load_buffer_to_tab(cur_tab, load_buf, read_bytes);
        } else {
            load_buffer_to_tab(cur_tab, "", 0);
        }
    }

    // Если папка проекта пустая — создаем стандартный main.c
    if (tab_count == 0) {
        const char* template_c =
            "void main() {\n"
            "    print(\"Hello Project!\\n\");\n"
            "}\n";
        to_fat83("MAIN.C", tabs[0].fat_name);
        fat16_write_file(tabs[0].fat_name, template_c, my_strlen(template_c));

        tab_count = 1;
        my_strcpy(tabs[0].dir_path, dir_path);
        my_strcpy(tabs[0].name, "main.c");
        tabs[0].file_type = 1;
        load_buffer_to_tab(0, template_c, my_strlen(template_c));
    }

    active_tab = 0;
}

static void fs_save_active_tab(void) {
    if (tab_count == 0 || !fat16_is_mounted()) return;

    navigate_to_dir(tabs[active_tab].dir_path);

    static char save_buf[EDITOR_MAX_LINES * EDITOR_MAX_COLS];
    int total_bytes = 0;
    int total_lines = get_total_lines();

    for (int r = 0; r < total_lines; r++) {
        for (int c = 0; tabs[active_tab].text[r][c] != '\0'; c++) {
            save_buf[total_bytes++] = tabs[active_tab].text[r][c];
        }
        save_buf[total_bytes++] = '\n';
    }

    if (fat16_write_file(tabs[active_tab].fat_name, save_buf, total_bytes)) {
        tabs[active_tab].is_modified = 0;
        int i = 0; const char* msg = "Saved to ";
        while (msg[i]) { status_msg[i] = msg[i]; i++; }
        int d = 0;
        while (tabs[active_tab].dir_path[d] && i < 50) status_msg[i++] = tabs[active_tab].dir_path[d++];
        if (status_msg[i-1] != '/') status_msg[i++] = '/';
        int m = 0;
        while (tabs[active_tab].name[m] && i < 62) status_msg[i++] = tabs[active_tab].name[m++];
        status_msg[i] = '\0';
    }
}

void insert_char(char c) {
    if (tab_count == 0) return;
    int len = line_length(tabs[active_tab].cursor_row);
    if (len >= EDITOR_MAX_COLS - 2) return;
    for (int i = len; i >= tabs[active_tab].cursor_col; i--) {
        tabs[active_tab].text[tabs[active_tab].cursor_row][i + 1] = tabs[active_tab].text[tabs[active_tab].cursor_row][i];
    }
    tabs[active_tab].text[tabs[active_tab].cursor_row][tabs[active_tab].cursor_col] = c;
    tabs[active_tab].cursor_col++;
    tabs[active_tab].is_modified = 1;
}

static void handle_backspace(void) {
    if (tab_count == 0) return;
    int r = tabs[active_tab].cursor_row;
    int c = tabs[active_tab].cursor_col;
    int len = line_length(r);
    if (c > len) c = len;

    if (c > 0) {
        for (int i = c - 1; i < len; i++) {
            tabs[active_tab].text[r][i] = tabs[active_tab].text[r][i + 1];
        }
        tabs[active_tab].text[r][len - 1] = '\0';
        tabs[active_tab].cursor_col--;
        tabs[active_tab].is_modified = 1;
    } else if (r > 0) {
        int prev_len = line_length(r - 1);
        int curr_len = len;
        if (prev_len + curr_len < EDITOR_MAX_COLS - 1) {
            for (int i = 0; i <= curr_len; i++) {
                tabs[active_tab].text[r - 1][prev_len + i] = tabs[active_tab].text[r][i];
            }
            for (int row = r; row < EDITOR_MAX_LINES - 1; row++) {
                for (int col = 0; col < EDITOR_MAX_COLS; col++) {
                    tabs[active_tab].text[row][col] = tabs[active_tab].text[row + 1][col];
                }
            }
            for (int col = 0; col < EDITOR_MAX_COLS; col++) tabs[active_tab].text[EDITOR_MAX_LINES - 1][col] = '\0';
            tabs[active_tab].cursor_row--;
            tabs[active_tab].cursor_col = prev_len;
            tabs[active_tab].is_modified = 1;
        }
    }
}

static void handle_enter(void) {
    if (tab_count == 0) return;
    int r = tabs[active_tab].cursor_row;
    int c = tabs[active_tab].cursor_col;
    if (r >= EDITOR_MAX_LINES - 1) return;

    int indent = 0;
    while (indent < c && tabs[active_tab].text[r][indent] == ' ') indent++;

    for (int row = EDITOR_MAX_LINES - 1; row > r + 1; row--) {
        for (int col = 0; col < EDITOR_MAX_COLS; col++) {
            tabs[active_tab].text[row][col] = tabs[active_tab].text[row - 1][col];
        }
    }
    for (int col = 0; col < EDITOR_MAX_COLS; col++) tabs[active_tab].text[r + 1][col] = '\0';
    for (int i = 0; i < indent && i < EDITOR_MAX_COLS - 1; i++) tabs[active_tab].text[r + 1][i] = ' ';

    int curr_len = line_length(r);
    int dest_col = indent;
    for (int i = c; i < curr_len && dest_col < EDITOR_MAX_COLS - 1; i++) {
        tabs[active_tab].text[r + 1][dest_col++] = tabs[active_tab].text[r][i];
    }
    tabs[active_tab].text[r + 1][dest_col] = '\0';
    tabs[active_tab].text[r][c] = '\0';

    tabs[active_tab].cursor_row++;
    tabs[active_tab].cursor_col = indent;
    tabs[active_tab].is_modified = 1;
}

static void format_line_number(int n, char* out) {
    if (n < 10) {
        out[0] = ' '; out[1] = ' '; out[2] = '0' + n; out[3] = '\0';
    } else if (n < 100) {
        out[0] = ' '; out[1] = '0' + (n / 10); out[2] = '0' + (n % 10); out[3] = '\0';
    } else {
        out[0] = '0' + (n / 100); out[1] = '0' + ((n / 10) % 10); out[2] = '0' + (n % 10); out[3] = '\0';
    }
}

static char get_char_for_scancode(uint8_t sc) {
    if (sc >= 128) return 0;
    if (kbd_layout == 0) {
        char base = ascii_normal[sc];
        if (base >= 'a' && base <= 'z') {
            int make_upper = caps_lock ^ shift_pressed;
            return make_upper ? ascii_shift[sc] : base;
        }
        return shift_pressed ? ascii_shift[sc] : base;
    } else {
        int is_ru = (sc >= 0x10 && sc <= 0x1B) || (sc >= 0x1E && sc <= 0x28) || (sc >= 0x2C && sc <= 0x34) || (sc == 0x29);
        if (is_ru) {
            int make_upper = caps_lock ^ shift_pressed;
            return (char)(make_upper ? scancode_ru_shift[sc] : scancode_ru[sc]);
        }
        return (char)(shift_pressed ? scancode_ru_shift[sc] : scancode_ru[sc]);
    }
}

static int is_c_keyword(const char* word) {
    const char* kw[] = {
        "void", "int", "char", "short", "long", "unsigned", "signed",
        "struct", "union", "enum", "typedef", "return", "if", "else",
        "for", "while", "do", "switch", "case", "default", "break",
        "continue", "static", "const", "volatile", "extern", "sizeof", "asm", 0
    };
    for (int i = 0; kw[i]; i++) {
        if (my_strcmp(word, kw[i]) == 0) return 1;
    }
    return 0;
}

static int is_c_type(const char* word) {
    const char* tp[] = {
        "uint32_t", "uint16_t", "uint8_t", "uintptr_t", "int32_t", "int16_t", "int8_t", 0
    };
    for (int i = 0; tp[i]; i++) {
        if (my_strcmp(word, tp[i]) == 0) return 1;
    }
    return 0;
}

static int is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void draw_syntax_line(int start_x, int y, const char* line) {
    int x = start_x;
    int i = 0;
    int is_comment_line = 0;
    int is_preproc_line = 0;
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] == '/' && line[i+1] == '/') is_comment_line = 1;
    if (line[i] == '#') is_preproc_line = 1;
    i = 0;

    while (line[i] != '\0' && x < (int)screen_width - 16) {
        char c = line[i];

        if (is_comment_line || (c == '/' && line[i+1] == '/')) {
            while (line[i] != '\0' && x < (int)screen_width - 16) {
                kputc_at(x, y, line[i], VS_COMMENT);
                x += 8; i++;
            }
            break;
        }

        if (c == '"') {
            kputc_at(x, y, c, VS_STRING); x += 8; i++;
            while (line[i] != '\0' && line[i] != '"' && x < (int)screen_width - 16) {
                kputc_at(x, y, line[i], VS_STRING); x += 8; i++;
            }
            if (line[i] == '"') { kputc_at(x, y, '"', VS_STRING); x += 8; i++; }
            continue;
        }

        if (is_preproc_line && i == 0) {
            while (line[i] != '\0' && line[i] != '<' && line[i] != '"' && x < (int)screen_width - 16) {
                kputc_at(x, y, line[i], VS_PREPROC); x += 8; i++;
            }
            if (line[i] == '<' || line[i] == '"') {
                char quote_end = (line[i] == '<') ? '>' : '"';
                kputc_at(x, y, line[i], VS_PREPROC); x += 8; i++;
                while (line[i] != '\0' && line[i] != quote_end && x < (int)screen_width - 16) {
                    kputc_at(x, y, line[i], VS_STRING); x += 8; i++;
                }
                if (line[i] == quote_end) { kputc_at(x, y, quote_end, VS_PREPROC); x += 8; i++; }
            }
            is_preproc_line = 0;
            continue;
        }

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            char word[32];
            int wlen = 0;
            while (is_ident_char(line[i]) && wlen < 31) word[wlen++] = line[i++];
            word[wlen] = '\0';

            uint32_t color = VS_TEXT;
            if (is_c_keyword(word)) color = VS_KEYWORD;
            else if (is_c_type(word)) color = VS_TYPE;

            for (int w = 0; w < wlen; w++) {
                kputc_at(x, y, word[w], color);
                x += 8;
            }
            continue;
        }

        if (c >= '0' && c <= '9' && (i == 0 || !is_ident_char(line[i-1]))) {
            kputc_at(x, y, c, VS_NUMBER);
            x += 8; i++;
            continue;
        }

        kputc_at(x, y, c, COLOR_TEXT);
        x += 8; i++;
    }
}

static void draw_editor_ui(void) {
    clear_screen(COLOR_BG);

    // 1. Верхняя панель табов
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < (int)screen_width; x++) put_pixel(x, y, COLOR_TOPBAR_BG);
    }

    int tab_x = 8;
    for (int t = 0; t < tab_count; t++) {
        int is_act = (t == active_tab);
        int name_len = my_strlen(tabs[t].name);
        int tab_w = 32 + (name_len * 8) + 16;
        if (tab_w < 90) tab_w = 90;

        for (int y = 2; y < 24; y++) {
            for (int x = tab_x; x < tab_x + tab_w; x++) {
                put_pixel(x, y, is_act ? COLOR_TAB_ACT : COLOR_TAB_INACT);
            }
        }
        if (is_act) {
            for (int x = tab_x; x < tab_x + tab_w; x++) put_pixel(x, 2, COLOR_STATUS_BG);
        }

        if (tabs[t].file_type == 1) {
            kputs_at(tab_x + 6, 5, "C", 0x569CD6);
        } else if (tabs[t].file_type == 2) {
            kputs_at(tab_x + 6, 5, "C", 0xC586C0);
        } else {
            put_pixel(tab_x + 7, 9, 0x888888); put_pixel(tab_x + 8, 9, 0x888888); put_pixel(tab_x + 9, 9, 0x888888);
            put_pixel(tab_x + 7, 12, 0x888888); put_pixel(tab_x + 8, 12, 0x888888); put_pixel(tab_x + 9, 12, 0x888888);
            put_pixel(tab_x + 7, 15, 0x888888); put_pixel(tab_x + 8, 15, 0x888888); put_pixel(tab_x + 9, 15, 0x888888);
        }

        char title[20] = {0};
        int p = 0, m = 0;
        while (tabs[t].name[m] && p < 15) title[p++] = tabs[t].name[m++];
        if (tabs[t].is_modified) title[p++] = '*';
        title[p] = '\0';

        kputs_at(tab_x + 20, 5, title, is_act ? 0xFFFFFF : 0x888888);
        tab_x += tab_w + 4;
    }

    kputs_at(screen_width - 340, 5, "Lang: ", 0x888888);
    kputs_at(screen_width - 290, 5, kbd_layout ? "RU" : "EN", kbd_layout ? 0xFF6666 : 0x4EC9B0);
    if (caps_lock) kputs_at(screen_width - 260, 5, "[CAPS]", 0xDCDCAA);

    if (tab_count > 0) {
        if (tabs[active_tab].file_type == 1)      kputs_at(screen_width - 200, 5, "[C-Mode]", 0x569CD6);
        else if (tabs[active_tab].file_type == 2) kputs_at(screen_width - 200, 5, "[H-Mode]", 0xC586C0);
        else                                      kputs_at(screen_width - 200, 5, "[Text]", 0x888888);
    }

    update_scroll();
    int total_lines = get_total_lines();
    int visible_lines = get_visible_lines();

    // 2. Отрисовка кода
    for (int i = 0; i < visible_lines; i++) {
        int r = tabs[active_tab].scroll_row + i;
        if (r >= total_lines) break;

        int y = 28 + (i * 18);
        int is_cur = (r == tabs[active_tab].cursor_row);

        char num_buf[8];
        format_line_number(r + 1, num_buf);
        kputs_at(8, y, num_buf, is_cur ? COLOR_GUTTER_ACT : COLOR_GUTTER_INACT);

        if (tab_count > 0 && tabs[active_tab].file_type > 0) {
            draw_syntax_line(CODE_START_X, y, tabs[active_tab].text[r]);
        } else {
            kputs_at(CODE_START_X, y, tabs[active_tab].text[r], COLOR_TEXT);
        }
    }

    // 3. Курсор
    int cur_r = tabs[active_tab].cursor_row;
    int sc_r = tabs[active_tab].scroll_row;
    if (cur_r >= sc_r && cur_r < sc_r + visible_lines) {
        int cur_px = CODE_START_X + (tabs[active_tab].cursor_col * 8);
        int cur_py = 28 + ((cur_r - sc_r) * 18);
        for (int y = 0; y < 16; y++) {
            put_pixel(cur_px, cur_py + y, COLOR_CURSOR);
            put_pixel(cur_px + 1, cur_py + y, COLOR_CURSOR);
        }
    }

    // 4. Нижний статус-бар
    int bottom_y = (int)screen_height - 22;
    for (int y = bottom_y; y < (int)screen_height; y++) {
        for (int x = 0; x < (int)screen_width; x++) put_pixel(x, y, COLOR_STATUS_BG);
    }

    kputs_at(8, bottom_y + 3, "F1:<-Tab | F2:Tab-> | F5:Run | F6:Save | F8:.PRG | F9:.SYS | ESC", 0xFFFFFF);
    kputs_at(screen_width - 320, bottom_y + 3, "| ", 0xCCE6FF);
    kputs_at(screen_width - 304, bottom_y + 3, status_msg, 0xFFFF88);

    flush_buffer();
}

int get_tab_file_content(const char* filename, char* out_buf, int max_len) {
    for (int t = 0; t < tab_count; t++) {
        const char* a = filename;
        const char* b = tabs[t].name;
        int match = 1;
        while (*a && *b) {
            char ca = *a++, cb = *b++;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) { match = 0; break; }
        }
        if (*a || *b) match = 0;

        if (match) {
            int pos = 0;
            for (int r = 0; r < EDITOR_MAX_LINES; r++) {
                if (tabs[t].text[r][0] == '\0') {
                    int has_more = 0;
                    for (int chk = r + 1; chk < EDITOR_MAX_LINES; chk++) {
                        if (tabs[t].text[chk][0] != '\0') { has_more = 1; break; }
                    }
                    if (!has_more) break;
                }
                for (int c = 0; tabs[t].text[r][c] != '\0'; c++) {
                    if (pos < max_len - 2) out_buf[pos++] = tabs[t].text[r][c];
                }
                if (pos < max_len - 2) out_buf[pos++] = '\n';
            }
            out_buf[pos] = '\0';
            return 1;
        }
    }
    return 0;
}


static void prompt_new_file(void) {
    char name_buf[32] = {0};
    int pos = 0;
    int bottom_y = (int)screen_height - 22;

    while (1) {
        // Отрисовка строки запроса в нижнем статус-баре
        for (int y = bottom_y; y < (int)screen_height; y++) {
            for (int x = 0; x < (int)screen_width; x++) put_pixel(x, y, 0x1E1E1E);
        }
        kputs_at(8, bottom_y + 3, "New file name: ", 0x00FFFF);
        kputs_at(130, bottom_y + 3, name_buf, 0xFFFFFF);
        kputc_at(130 + (pos * 8), bottom_y + 3, '_', 0x55FF55);
        flush_buffer();

        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t sc = inb(0x60);
            
            // ФИКС МЫШИ: отбрасываем байты от тачпада/мыши
            if (status & 0x20) continue;
            // Игнорируем отпускания клавиш
            if (sc & 0x80) continue;

            // ESC — отмена
            if (sc == 0x01) break;

            // Enter — подтверждение создания
            if (sc == 0x1C) {
                if (pos > 0 && tab_count < 8) {
                    // Создаем новый таб
                    int t = tab_count++;
                    active_tab = t;
                    my_strcpy(tabs[t].name, name_buf);
                    tabs[t].file_type = (name_buf[my_strlen(name_buf) - 1] == 'h') ? 2 : 1;
                    tabs[t].is_modified = 1;
                    tabs[t].cursor_row = 0;
                    tabs[t].cursor_col = 0;
                    tabs[t].scroll_row = 0;
                    // Очищаем строки текста в новой вкладке
                    for (int r = 0; r < EDITOR_MAX_LINES; r++) {
                        tabs[t].text[r][0] = '\0';
                    }
                    sync_to_text_buf();
                }
                break;
            }

            // Backspace
            if (sc == 0x0E) {
                if (pos > 0) {
                    pos--;
                    name_buf[pos] = '\0';
                }
                continue;
            }

            // Преобразование скан-кода в символ
            char ch = get_char_for_scancode(sc);
            if (ch >= 32 && ch <= 126 && pos < 24) {
                name_buf[pos++] = ch;
                name_buf[pos] = '\0';
            }
        }
        sleep_ms(10);
    }
}

static void resolve_project_path(const char* arg, char* out_path) {
    // Если путь уже абсолютный (начинается с '/')
    if (arg[0] == '/') {
        my_strcpy(out_path, arg);
        return;
    }

    // Если указали просто имя проекта (например "cube"), разворачиваем в "/CODE/CUBE"
    out_path[0] = '/';
    out_path[1] = 'C';
    out_path[2] = 'O';
    out_path[3] = 'D';
    out_path[4] = 'E';
    out_path[5] = '/';
    int idx = 6;
    for (int i = 0; arg[i] && idx < 60; i++) {
        char c = arg[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out_path[idx++] = c;
    }
    out_path[idx] = '\0';
}

// Функция сборки текущего открытого .c файла в бинарный .SYS файл
static void build_sys_binary(void) {
    if (tab_count == 0 || tabs[active_tab].file_type != 1) {
        my_strcpy(status_msg, "Error: only .c files can be built to .SYS!");
        return;
    }

    // 1. Формируем дефолтное имя на основе текущего таба (без .c)
    char sys_name[9] = {0};
    int pos = 0;
    for (int i = 0; tabs[active_tab].name[i] && tabs[active_tab].name[i] != '.' && pos < 8; i++) {
        char c = tabs[active_tab].name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        // Разрешаем только латиницу, цифры, тире и подчеркивание для FAT 8.3
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            sys_name[pos++] = c;
        }
    }
    if (pos == 0) {
        sys_name[0] = 'M'; sys_name[1] = 'A'; sys_name[2] = 'I'; sys_name[3] = 'N';
        pos = 4;
    }
    sys_name[pos] = '\0';

    int bottom_y = (int)screen_height - 22;

    // 2. Интерактивный ввод имени (расширение .SYS зафиксировано)
    int confirmed = 0;
    while (1) {
        for (int y = bottom_y; y < (int)screen_height; y++) {
            for (int x = 0; x < (int)screen_width; x++) put_pixel(x, y, 0x1E1E1E);
        }

        kputs_at(8, bottom_y + 3, "Build as: [", 0x55FF55);
        kputs_at(104, bottom_y + 3, sys_name, 0xFFFF55);
        // Зафиксированное расширение и курсор
        kputc_at(104 + (pos * 8), bottom_y + 3, '_', 0xFFFFFF);
        kputs_at(104 + (pos * 8) + 10, bottom_y + 3, "].SYS  (Enter: Build | ESC: Cancel)", 0xAAAAAA);
        flush_buffer();

        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t sc = inb(0x60);
            
            // ФИКС МЫШИ: защищаем ввод имени бинарника от тачпада
            if (status & 0x20) continue;
            if (sc & 0x80) continue;

            // ESC — отменить сборку
            if (sc == 0x01) {
                my_strcpy(status_msg, "Build canceled");
                return;
            }

            // Enter — собираем с выбранным именем
            if (sc == 0x1C) {
                if (pos > 0) {
                    confirmed = 1;
                    break;
                }
            }

            // Backspace
            if (sc == 0x0E) {
                if (pos > 0) {
                    pos--;
                    sys_name[pos] = '\0';
                }
                continue;
            }

            char ch = get_char_for_scancode(sc);
            if (ch >= 'a' && ch <= 'z') ch -= 32; // Всегда в верхний регистр для FAT 8.3

            // Ограничение FAT 8.3: имя не более 8 символов
            if (pos < 8) {
                if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                    sys_name[pos++] = ch;
                    sys_name[pos] = '\0';
                }
            }
        }
        sleep_ms(10);
    }

    if (!confirmed) return;

    // 3. Компиляция проекта
    sync_to_text_buf();

    static char full_src[EDITOR_MAX_LINES * EDITOR_MAX_COLS];
    int p_idx = 0;
    for (int r = 0; r < EDITOR_MAX_LINES; r++) {
        for (int c = 0; text_buf[r][c] != '\0'; c++) {
            full_src[p_idx++] = text_buf[r][c];
        }
        full_src[p_idx++] = '\n';
    }
    full_src[p_idx] = '\0';

    // Вместо set_compiler_target_sys(1):
    set_compiler_target(TARGET_SYS);
    int ok = compile_source_to_x86(full_src);
    set_compiler_target(TARGET_JIT);

    if (!ok) {
        my_strcpy(status_msg, "Build Failed: Compile Error!");
        return;
    }

    int code_size = get_jit_code_size();

    // 4. Формирование .SYS бинарника
    static uint8_t sys_file_buffer[sizeof(sys_header_t) + sizeof(jit_buffer)];
    sys_header_t* hdr = (sys_header_t*)sys_file_buffer;

    hdr->magic[0] = SYS_MAGIC_0;
    hdr->magic[1] = SYS_MAGIC_1;
    hdr->magic[2] = SYS_MAGIC_2;
    hdr->magic[3] = SYS_MAGIC_3;
    hdr->entry_point = (driver_entry_t)(SYS_LOAD_BASE + sizeof(sys_header_t));

    for (int i = 0; i < code_size; i++) {
        sys_file_buffer[sizeof(sys_header_t) + i] = jit_buffer[i];
    }

    int total_sys_size = sizeof(sys_header_t) + code_size;

    // 5. Заполнение FAT 8.3 имени (ровно 11 байт: 8 байт имя + 3 байта расширение SYS)
    char out_name83[12];
    for (int i = 0; i < 11; i++) out_name83[i] = ' ';
    out_name83[11] = '\0';

    for (int i = 0; i < pos; i++) {
        out_name83[i] = sys_name[i];
    }
    out_name83[8]  = 'S';
    out_name83[9]  = 'Y';
    out_name83[10] = 'S';

    // 6. Запись в корень диска
    fat16_go_root();
    if (fat16_write_file(out_name83, (const char*)sys_file_buffer, total_sys_size)) {
        navigate_to_dir(tabs[active_tab].dir_path);

        char msg[64] = "Built: /";
        int m = 8;
        for (int i = 0; i < pos; i++) msg[m++] = sys_name[i];
        msg[m++] = '.'; msg[m++] = 'S'; msg[m++] = 'Y'; msg[m++] = 'S';
        msg[m] = '\0';
        my_strcpy(status_msg, msg);
    } else {
        navigate_to_dir(tabs[active_tab].dir_path);
        my_strcpy(status_msg, "Write to FAT16 failed!");
    }
}

// Функция сборки текущего открытого .c файла в изолированный .PRG файл (Ring 3)
static void build_prg_binary(void) {
    if (tab_count == 0 || tabs[active_tab].file_type != 1) {
        my_strcpy(status_msg, "Error: only .c files can be built to .PRG!");
        return;
    }

    // 1. Формируем имя по умолчанию (до 8 символов FAT 8.3)
    char prg_name[9] = {0};
    int pos = 0;
    for (int i = 0; tabs[active_tab].name[i] && tabs[active_tab].name[i] != '.' && pos < 8; i++) {
        char c = tabs[active_tab].name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            prg_name[pos++] = c;
        }
    }
    if (pos == 0) {
        prg_name[0] = 'T'; prg_name[1] = 'E'; prg_name[2] = 'S'; prg_name[3] = 'T';
        pos = 4;
    }
    prg_name[pos] = '\0';

    int bottom_y = (int)screen_height - 22;

    // 2. Интерактивный ввод имени бинарника
    int confirmed = 0;
    while (1) {
        for (int y = bottom_y; y < (int)screen_height; y++) {
            for (int x = 0; x < (int)screen_width; x++) put_pixel(x, y, 0x1E1E1E);
        }

        kputs_at(8, bottom_y + 3, "Build App: [", 0x55FFFF);
        kputs_at(110, bottom_y + 3, prg_name, 0xFFFF55);
        kputc_at(110 + (pos * 8), bottom_y + 3, '_', 0xFFFFFF);
        kputs_at(110 + (pos * 8) + 10, bottom_y + 3, "].PRG  (Enter: Build | ESC: Cancel)", 0xAAAAAA);
        flush_buffer();

        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t sc = inb(0x60);
            if (status & 0x20) continue;
            if (sc & 0x80) continue;

            if (sc == 0x01) {
                my_strcpy(status_msg, "Build canceled");
                return;
            }

            if (sc == 0x1C) {
                if (pos > 0) {
                    confirmed = 1;
                    break;
                }
            }

            if (sc == 0x0E) {
                if (pos > 0) {
                    pos--;
                    prg_name[pos] = '\0';
                }
                continue;
            }

            char ch = get_char_for_scancode(sc);
            if (ch >= 'a' && ch <= 'z') ch -= 32;

            if (pos < 8) {
                if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                    prg_name[pos++] = ch;
                    prg_name[pos] = '\0';
                }
            }
        }
        sleep_ms(10);
    }

    if (!confirmed) return;

    // 3. Компиляция для архитектуры Ring 3
    sync_to_text_buf();

    static char full_src[EDITOR_MAX_LINES * EDITOR_MAX_COLS];
    int p_idx = 0;
    for (int r = 0; r < EDITOR_MAX_LINES; r++) {
        for (int c = 0; text_buf[r][c] != '\0'; c++) {
            full_src[p_idx++] = text_buf[r][c];
        }
        full_src[p_idx++] = '\n';
    }
    full_src[p_idx] = '\0';

    set_compiler_target(TARGET_PRG);
    int ok = compile_source_to_x86(full_src);
    set_compiler_target(TARGET_JIT);

    if (!ok) {
        my_strcpy(status_msg, "Build Failed: Compile Error!");
        return;
    }

    int code_size = get_jit_code_size();

    // 4. Формирование плоского файла .PRG: сначала машинный код, затем пул строк со смещения 0x2000
    static uint8_t prg_file_buffer[0x2000 + 4096];
    for (int i = 0; i < (int)sizeof(prg_file_buffer); i++) prg_file_buffer[i] = 0;

    for (int i = 0; i < code_size; i++) {
        prg_file_buffer[i] = jit_buffer[i];
    }

    // Дописываем строковые литералы по смещению 0x2000
    extern char string_pool[4096];
    extern int string_pool_idx;
    for (int i = 0; i < string_pool_idx; i++) {
        prg_file_buffer[0x2000 + i] = (uint8_t)string_pool[i];
    }

    int total_prg_size = 0x2000 + string_pool_idx;

    // 5. Имя файла FAT 8.3: 8 символов + PRG
    char out_name83[12];
    for (int i = 0; i < 11; i++) out_name83[i] = ' ';
    out_name83[11] = '\0';

    for (int i = 0; i < pos; i++) {
        out_name83[i] = prg_name[i];
    }
    out_name83[8]  = 'P';
    out_name83[9]  = 'R';
    out_name83[10] = 'G';

    // 6. Запись файла в корень диска
    fat16_go_root();
    if (fat16_write_file(out_name83, (const char*)prg_file_buffer, total_prg_size)) {
        navigate_to_dir(tabs[active_tab].dir_path);

        char msg[64] = "Built: /";
        int m = 8;
        for (int i = 0; i < pos; i++) msg[m++] = prg_name[i];
        msg[m++] = '.'; msg[m++] = 'P'; msg[m++] = 'R'; msg[m++] = 'G';
        msg[m] = '\0';
        my_strcpy(status_msg, msg);
    } else {
        navigate_to_dir(tabs[active_tab].dir_path);
        my_strcpy(status_msg, "Write to FAT16 failed!");
    }
}

void run_editor(const char* args) {
    console_save_state();

    // 1. Точно запоминаем директорию, откуда запустили редактор
    char saved_path[64];
    my_strcpy(saved_path, current_path);

    active_tab = 0;
    kbd_layout = 0;
    shift_pressed = 0;
    ctrl_pressed = 0;
    caps_lock = 0;

    // 2. Разбор аргументов запуска
    if (args && (args[0] == 'n' && args[1] == 'e' && args[2] == 'w' && (args[3] == ' ' || args[3] == '\0'))) {
        const char* proj = args + 3;
        while (*proj == ' ') proj++;
        if (*proj != '\0') {
            create_project(proj);
            char pdir[64];
            resolve_project_path(proj, pdir);
            scan_and_load_dir(pdir);
        } else {
            scan_and_load_dir("/CODE");
        }
    } else if (args && args[0] != '\0') {
        int has_extension = 0;
        for (int i = 0; args[i]; i++) {
            if (args[i] == '.') { has_extension = 1; break; }
        }

        if (has_extension) {
            load_single_file(args);
        } else {
            char pdir[64];
            resolve_project_path(args, pdir);
            scan_and_load_dir(pdir);
        }
    } else {
        scan_and_load_dir("/CODE");
    }

    draw_editor_ui();

    int running = 1;
    static int ext_key = 0;

    while (running) {
        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t scancode = inb(0x60);
            if (status & 0x20) continue;

            if (scancode == 0xE0) { ext_key = 1; continue; }

            if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; continue; }
            if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; continue; }

            if (scancode == 0x1D) { ctrl_pressed = 1; continue; }
            if (scancode == 0x9D) { ctrl_pressed = 0; continue; }

            // CapsLock
            if (scancode == 0x3A) {
                caps_lock = !caps_lock;
                draw_editor_ui();
                continue;
            }

            // Переключение языка (Alt)
            if (scancode == 0x38) {
                kbd_layout = !kbd_layout;
                ext_key = 0;
                draw_editor_ui();
                continue;
            }

            if (scancode & 0x80) { ext_key = 0; continue; }

            // ESC: Выход из редактора
            if (scancode == 0x01) {
                running = 0;
                break;
            }

            // F1: Предыдущая вкладка
            if (scancode == 0x3B && tab_count > 1) {
                active_tab = (active_tab - 1 + tab_count) % tab_count;
                int i = 0; const char* msg = "Tab: ";
                while (msg[i]) { status_msg[i] = msg[i]; i++; }
                int m = 0;
                while (tabs[active_tab].name[m] && i < 60) status_msg[i++] = tabs[active_tab].name[m++];
                status_msg[i] = '\0';
                draw_editor_ui();
                continue;
            }

            // F2: Следующая вкладка
            if (scancode == 0x3C && tab_count > 1) {
                active_tab = (active_tab + 1) % tab_count;
                int i = 0; const char* msg = "Tab: ";
                while (msg[i]) { status_msg[i] = msg[i]; i++; }
                int m = 0;
                while (tabs[active_tab].name[m] && i < 60) status_msg[i++] = tabs[active_tab].name[m++];
                status_msg[i] = '\0';
                draw_editor_ui();
                continue;
            }

            // F3: Создать файл в текущем воркспейсе
            if (scancode == 0x3D) {
                prompt_new_file();
                draw_editor_ui();
                continue;
            }

            // F5: Компиляция и запуск через JIT
            if (scancode == 0x3F) {
                if (tab_count > 0 && tabs[active_tab].file_type == 1) {
                    sync_to_text_buf();
                    run_compiled_code();
                } else if (tab_count > 0 && tabs[active_tab].file_type == 2) {
                    int i = 0; const char* msg = "Headers (.h) cannot be executed!";
                    while (msg[i]) { status_msg[i] = msg[i]; i++; }
                    status_msg[i] = '\0';
                } else {
                    int i = 0; const char* msg = "Cannot run non-C file!";
                    while (msg[i]) { status_msg[i] = msg[i]; i++; }
                    status_msg[i] = '\0';
                }
                draw_editor_ui();
                continue;
            }

            // F6 / Ctrl+S: Сохранение
            if (scancode == 0x40 || (ctrl_pressed && scancode == 0x1F)) {
                fs_save_active_tab();
                draw_editor_ui();
                continue;
            }

            // F7: Пересканировать воркспейс
            if (scancode == 0x41) {
                const char* cur_dir = (tab_count > 0 && tabs[active_tab].dir_path[0]) ? tabs[active_tab].dir_path : "/CODE";
                scan_and_load_dir(cur_dir);
                draw_editor_ui();
                continue;
            }

            // F8: Скомпилировать и сохранить как .PRG файл (Ring 3 Sandbox)
            if (scancode == 0x42) {
                build_prg_binary();
                draw_editor_ui();
                continue;
            }

            // F9: Скомпилировать и сохранить как .SYS файл (Ring 0 Kernel Module)
            if (scancode == 0x43) {
                build_sys_binary();
                draw_editor_ui();
                continue;
            }

            // Tab (4 пробела)
            if (scancode == 0x0F) {
                int spaces = 4 - (tabs[active_tab].cursor_col % 4);
                for (int s = 0; s < spaces; s++) insert_char(' ');
            } else if (scancode == 0x0E) {
                handle_backspace();
            } else if (scancode == 0x1C) {
                handle_enter();
            } else if (scancode == 0x47) { // Home
                tabs[active_tab].cursor_col = 0;
            } else if (scancode == 0x4F) { // End
                tabs[active_tab].cursor_col = line_length(tabs[active_tab].cursor_row);
            } else if (scancode == 0x48) { // Вверх
                if (tabs[active_tab].cursor_row > 0) {
                    tabs[active_tab].cursor_row--;
                    int len = line_length(tabs[active_tab].cursor_row);
                    if (tabs[active_tab].cursor_col > len) tabs[active_tab].cursor_col = len;
                }
            } else if (scancode == 0x50) { // Вниз
                if (tabs[active_tab].cursor_row < EDITOR_MAX_LINES - 1) {
                    tabs[active_tab].cursor_row++;
                    int len = line_length(tabs[active_tab].cursor_row);
                    if (tabs[active_tab].cursor_col > len) tabs[active_tab].cursor_col = len;
                }
            } else if (scancode == 0x4B) { // Влево
                if (tabs[active_tab].cursor_col > 0) {
                    tabs[active_tab].cursor_col--;
                } else if (tabs[active_tab].cursor_row > 0) {
                    tabs[active_tab].cursor_row--;
                    tabs[active_tab].cursor_col = line_length(tabs[active_tab].cursor_row);
                }
            } else if (scancode == 0x4D) { // Вправо
                int len = line_length(tabs[active_tab].cursor_row);
                if (tabs[active_tab].cursor_col < len) {
                    tabs[active_tab].cursor_col++;
                } else if (tabs[active_tab].cursor_row < EDITOR_MAX_LINES - 1) {
                    tabs[active_tab].cursor_row++;
                    tabs[active_tab].cursor_col = 0;
                }
            } else {
                char ch = get_char_for_scancode(scancode);
                if (ch != 0) insert_char(ch);
            }

            ext_key = 0;
            draw_editor_ui();
        }
        sleep_ms(5);
    }

    // 3. Возвращаем FAT16 и указатель current_path точно на место старта
    navigate_to_dir(saved_path);
    console_restore_state();
}