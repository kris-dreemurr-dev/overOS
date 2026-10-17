// overFiles — файловый менеджер overOS на тулките libui (src/lib/ui.h)
#include <stdint.h>
#include <stddef.h>
#include "libui/ui.h"
#include "libui/libui_ll.h"
#include "ext.h"

#define WIN_W 620
#define WIN_H 420

#define HEADER_H  40
#define STATUS_H  24
#define SIDEBAR_W 150
#define CELL_W    104
#define CELL_H    78
#define GRID_X    (SIDEBAR_W + 12)
#define GRID_Y    (HEADER_H + 10)
// Окно разворачивается (WP_FLAG_RESIZABLE), поэтому сетка значков считается от
// текущего размера окна, а не от констант
static int g_ww = WIN_W, g_wh = WIN_H;
#define GRID_COLS grid_cols()
#define GRID_ROWS grid_rows()
static int grid_cols(void) { int c = (g_ww - GRID_X - 8) / CELL_W; return c < 1 ? 1 : c; }
static int grid_rows(void) { int r = (g_wh - GRID_Y - STATUS_H - 4) / CELL_H; return r < 1 ? 1 : r; }

// ext.h проверяет заголовки .prg через эти функции
int  sys_open(const char* fn, int flags) { return ov_open(fn, flags); }
int  sys_read(int fd, void* buf, int sz) { return (int)ov_read(fd, buf, (uint64_t)sz); }
void sys_close(int fd)                   { ov_close(fd); }

static const uint16_t icon_folder_win[16] = {
    0b0000000000000000, 0b0111110000000000, 0b0100011111111110, 0b0100000000000010,
    0b0111111111111110, 0b0100000000000010, 0b0100000000000010, 0b0100000000000010,
    0b0100000000000010, 0b0100000000000010, 0b0100000000000010, 0b0111111111111110,
    0b0000000000000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
};

#define COLOR_FOLDER     0xB83248
#define COLOR_FOLDER_DK  0x8A2234
#define COLOR_SELECT_BG  0x3A2027

#define MAX_ITEMS 128
static ov_dirent_t s_files[MAX_ITEMS];
static uint8_t     s_kind[MAX_ITEMS];    // вид каждой записи, считается один раз при чтении каталога
static int         s_count = 0;
static int         s_folders = 0;
static int         s_sel = -1;
static int         s_scroll_row = 0;
static char        s_cwd[128] = "/";
static char        s_home[64] = "/";

// Подключённые накопители (как «Устройства» в панели мест Dolphin)
#define MAX_MOUNTS 6
static ov_mount_t  s_mounts[MAX_MOUNTS];
static int         s_nmounts = 0;

// Создание файла/папки "по месту"
static int  s_creating = 0;
static int  s_creating_dir = 0;
static char s_new_name[64];

// Доступ запрещён / режим администратора (как в Dolphin)
static int  s_orig_uid;
static int  s_admin = 0;
static int  s_denied = 0;
static int  s_denied_can_unlock = 0;
static char s_denied_path[128];
static const char* s_denied_msg = "";
static int  s_denied_retry = 0;          // после разблокировки зайти в s_denied_path
static char s_pw[64];

static const char* item_name(int i) { return s_files[i].clean_name[0] ? s_files[i].clean_name : s_files[i].name; }

static void path_join(const char* dir, const char* name, char* out, int max) {
    int p = 0;
    for (int i = 0; dir[i] && p < max - 1; i++) out[p++] = dir[i];
    if (p > 0 && out[p - 1] != '/' && p < max - 1) out[p++] = '/';
    for (int i = 0; name[i] && p < max - 1; i++) out[p++] = name[i];
    out[p] = '\0';
}

static void pw_wipe(void) {
    volatile char* v = s_pw;
    for (int i = 0; i < (int)sizeof(s_pw); i++) v[i] = 0;
}

static void show_denied(const char* path, const char* msg, int retry) {
    ov_strlcpy(s_denied_path, path, sizeof(s_denied_path));
    s_denied_msg = msg;
    s_denied = 1;
    s_denied_retry = retry;
    s_denied_can_unlock = !s_admin && ov_is_admin(s_orig_uid);
    pw_wipe();
}

static void reload_mounts(void) {
    ov_mount_t all[MAX_MOUNTS + 1];
    int n = ov_mounts(all, MAX_MOUNTS + 1);
    s_nmounts = 0;
    for (int i = 0; i < n && s_nmounts < MAX_MOUNTS; i++)
        if (all[i].flags & OV_MOUNT_REMOVABLE) s_mounts[s_nmounts++] = all[i];
}

// Путь внутри path (или сам path)
static int path_under(const char* p, const char* base) {
    int i = 0;
    while (base[i] && (p[i] | 0x20) == (base[i] | 0x20)) i++;
    return !base[i] && (p[i] == '\0' || p[i] == '/');
}

static int reload(void) {
    reload_mounts();
    // Ядро заполняет не все поля записи (например, длинное имя у файлов с коротким
    // именем 8.3) — чистим буфер, иначе в подписях виден мусор от прошлого каталога
    ov_memset(s_files, 0, sizeof(s_files));
    int n = ov_listdir(s_cwd, s_files, MAX_ITEMS);
    if (n < 0) { s_count = 0; s_folders = 0; return -1; }
    // Папки — первыми, как в любом файловом менеджере
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (s_files[i].attr & OV_ATTR_DIR) {
            ov_dirent_t t = s_files[k]; s_files[k] = s_files[i]; s_files[i] = t;
            k++;
        }
    }
    // "." и ".." не показываем
    s_count = 0;
    s_folders = 0;
    for (int i = 0; i < n; i++) {
        const char* nm = s_files[i].name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
        s_files[s_count] = s_files[i];
        if (s_files[s_count].attr & OV_ATTR_DIR) s_folders++;
        s_count++;
    }
    // Имена из FAT приходят с хвостовыми пробелами (формат 8.3) — обрезаем,
    // иначе они видны в подписи под значком и портят собираемый путь
    for (int i = 0; i < s_count; i++) {
        char* fields[2] = { s_files[i].name, s_files[i].clean_name };
        for (int f = 0; f < 2; f++) {
            char* t = fields[f];
            int len = 0;
            while (len < 31 && (uint8_t)t[len] >= 0x20) len++;
            while (len > 0 && t[len - 1] == ' ') len--;
            t[len] = '\0';
        }
    }
    // Вид файла определяем здесь, а не при отрисовке: для .prg он читает заголовок
    // с диска, и делать это для каждой ячейки в каждом кадре нельзя
    for (int i = 0; i < s_count; i++) {
        char full[160];
        path_join(s_cwd, item_name(i), full, sizeof(full));
        s_kind[i] = (uint8_t)ext_get_kind(full, item_name(i), s_files[i].attr);
    }
    if (s_sel >= s_count) s_sel = -1;
    return 0;
}

static void go_to(const char* path) {
    char prev[128];
    ov_strlcpy(prev, s_cwd, sizeof(prev));
    ov_strlcpy(s_cwd, path, sizeof(s_cwd));
    if (reload() < 0) {
        ov_strlcpy(s_cwd, prev, sizeof(s_cwd));
        reload();
        show_denied(path, "Нет прав на просмотр этой папки.", 1);
        return;
    }
    s_sel = -1;
    s_scroll_row = 0;
    s_creating = 0;
}

static void go_up(void) {
    if (s_cwd[0] == '/' && s_cwd[1] == '\0') return;
    char up[128];
    ov_strlcpy(up, s_cwd, sizeof(up));
    int last = 0;
    for (int i = 0; up[i]; i++) if (up[i] == '/') last = i;
    if (last == 0) { up[0] = '/'; up[1] = '\0'; } else up[last] = '\0';
    go_to(up);
}

// Открыть файл в программе. В режиме администратора программа запускается с
// теми же правами, что и сам проводник (как «Открыть от имени root» в Dolphin):
// иначе блокнот, открытый из разблокированной папки, тут же писал «Нет доступа
// к файлу». ov_spawn с uidgid = -1 наследует права вызывающего, а после ov_sudo
// это root. Передачу уже открытому окну (app_id) в режиме администратора не
// делаем: то окно работает под пользователем и прав всё равно не получит.
static void launch_as(const char* app_id, const char* app, const char* arg) {
    if (!s_admin && app_id && arg && arg[0] && ui_hand_over(app_id, arg)) return;
    if (ov_spawn(app, arg, (uint64_t)-1) < 0) show_denied(app, "Не удалось запустить программу.", 0);
}

static void launch(const char* app, const char* arg) { launch_as(0, app, arg); }

static void open_item(int i) {
    if (i < 0 || i >= s_count) return;
    char full[160];
    path_join(s_cwd, item_name(i), full, sizeof(full));
    switch (ext_get_action(full, item_name(i), s_files[i].attr)) {
        case FILE_ACT_DIR:      go_to(full); break;
        case FILE_ACT_EXEC_PRG: launch(full, ""); break;
        case FILE_ACT_IMAGE:    launch_as("ovimage",  "/desprogs/ovimage.prg", full); break;
        case FILE_ACT_AUDIO:    launch_as("ovplayer", "/desprogs/ovplayer.prg", full); break;
        case FILE_ACT_TEXT:     launch_as("notepad",  "/desprogs/notepad.prg", full); break;
        default:                show_denied(full, "Этот файл нечем открыть.", 0); break;
    }
}

static void commit_create(void) {
    s_creating = 0;
    if (!s_new_name[0]) return;
    char full[160];
    path_join(s_cwd, s_new_name, full, sizeof(full));
    int r = s_creating_dir ? ov_mkdir(full) : ov_create(full);
    if (r <= 0) show_denied(s_cwd, "Нет прав на создание здесь.", 0);
    reload();
}

static void draw_folder(ui_win_t* w, int x, int y, int sel) {
    ui_fill(w, x + 2, y, 12, 4, sel ? COLOR_FOLDER : COLOR_FOLDER_DK);
    ui_fill(w, x + 2, y + 4, 28, 18, COLOR_FOLDER);
    ui_fill(w, x + 5, y + 3, 22, 5, 0xEAEAEA);
    ui_fill(w, x, y + 7, 32, 18, sel ? 0xD63D56 : 0xA52A42);
    ui_fill(w, x, y + 7, 32, 1, 0xE84B66);
    ui_fill(w, x, y + 24, 32, 1, 0x731B2B);
}

// Значки по виду файла: рисуется лист бумаги, на нём пиктограмма и цветная
// полоса снизу. Вид файла берётся из кэша s_kind, потому что ext_get_kind для
// .prg открывает файл, а в кадре таких ячеек десятки.
static const uint16_t icon_kind[FILE_KIND_COUNT][16] = {
    { 0 },                                              // FILE_KIND_DIR — у папок свой значок
    {   // IMAGE: рамка, солнце и гора
        0b0000000000000000, 0b0000000000000000, 0b0001111111111000, 0b0001000000001000,
        0b0001000011001000, 0b0001000011001000, 0b0001000000001000, 0b0001000110001000,
        0b0001001111001000, 0b0001011111101000, 0b0001111111111000, 0b0001111111111000,
        0b0001111111111000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // AUDIO: нота
        0b0000000000000000, 0b0000000111111000, 0b0000000111111000, 0b0000000110001000,
        0b0000000110001000, 0b0000000110001000, 0b0000000110001000, 0b0000000110001000,
        0b0000001110001000, 0b0000011110001000, 0b0000111110011000, 0b0000111100111000,
        0b0000111001110000, 0b0000011111100000, 0b0000001111000000, 0b0000000000000000
    },
    {   // VIDEO: кадр плёнки с треугольником воспроизведения
        0b0000000000000000, 0b0000000000000000, 0b0011111111111100, 0b0010110110110100,
        0b0010000000000100, 0b0010001100000100, 0b0010001111000100, 0b0010001111110100,
        0b0010001111110100, 0b0010001111000100, 0b0010001100000100, 0b0010000000000100,
        0b0010110110110100, 0b0011111111111100, 0b0000000000000000, 0b0000000000000000
    },
    {   // TEXT: строки текста
        0b0000000000000000, 0b0000000000000000, 0b0001111111111000, 0b0000000000000000,
        0b0001111111111000, 0b0000000000000000, 0b0001111111111000, 0b0000000000000000,
        0b0001111111110000, 0b0000000000000000, 0b0001111111111000, 0b0000000000000000,
        0b0001111110000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // CODE: угловые скобки
        0b0000000000000000, 0b0000000000000000, 0b0000010000100000, 0b0000100000010000,
        0b0001000000001000, 0b0010000000000100, 0b0100000000000010, 0b0010000000000100,
        0b0001000000001000, 0b0000100000010000, 0b0000010000100000, 0b0000000000000000,
        0b0000000000000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // CONFIG: три движка настроек
        0b0000000000000000, 0b0000000000000000, 0b0001111111111000, 0b0000001100000000,
        0b0000001100000000, 0b0000000000000000, 0b0001111111111000, 0b0000000001100000,
        0b0000000001100000, 0b0000000000000000, 0b0001111111111000, 0b0000011000000000,
        0b0000011000000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // ARCHIVE: ящик с лентой
        0b0000000000000000, 0b0000000000000000, 0b0001111111111000, 0b0001100110011000,
        0b0001111111111000, 0b0001000110001000, 0b0001000110001000, 0b0001000000001000,
        0b0001000110001000, 0b0001000110001000, 0b0001000000001000, 0b0001111111111000,
        0b0000000000000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // EXEC: окно программы
        0b0000000000000000, 0b0000000000000000, 0b0001111111111000, 0b0001111111111000,
        0b0001000000001000, 0b0001000000001000, 0b0001000000001000, 0b0001000000001000,
        0b0001000000001000, 0b0001000000001000, 0b0001111111111000, 0b0000000000000000,
        0b0000000000000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // MODULE: микросхема с выводами
        0b0000000000000000, 0b0000100100100000, 0b0001111111111000, 0b0011000000001100,
        0b0001000000001000, 0b0011011111101100, 0b0001010000101000, 0b0001010000101000,
        0b0011011111101100, 0b0001000000001000, 0b0011000000001100, 0b0001111111111000,
        0b0000100100100000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // BINARY: нули и единицы
        0b0000000000000000, 0b0000000000000000, 0b0001100011100000, 0b0011110100010000,
        0b0010010100010000, 0b0010010100010000, 0b0010010100010000, 0b0010010100010000,
        0b0010010100010000, 0b0011110011100000, 0b0001100000000000, 0b0000000000000000,
        0b0000000000000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // FONT: буква
        0b0000000000000000, 0b0000000000000000, 0b0000011110000000, 0b0000111111000000,
        0b0001100001100000, 0b0001100001100000, 0b0001100001100000, 0b0001111111100000,
        0b0001111111100000, 0b0001100001100000, 0b0001100001100000, 0b0001100001100000,
        0b0000000000000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    {   // LIBRARY: кирпичная кладка — .ll собирается из нескольких исходников
        0b0000000000000000, 0b0000000000000000, 0b0001111111111000, 0b0001001001001000,
        0b0001001001001000, 0b0001111111111000, 0b0001100100100100, 0b0001100100100100,
        0b0001111111111000, 0b0001001001001000, 0b0001001001001000, 0b0001111111111000,
        0b0000000000000000, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
    },
    { 0 }                                               // FILE_KIND_OTHER — только лист бумаги
};

static const uint32_t color_kind[FILE_KIND_COUNT] = {
    0x6E7681,   // DIR (не используется)
    0x1976D2,   // IMAGE
    0x7B1FA2,   // AUDIO
    0xE64A19,   // VIDEO
    0x455A64,   // TEXT
    0x2E7D32,   // CODE
    0xF57F17,   // CONFIG
    0x5D4037,   // ARCHIVE
    0x0D47A1,   // EXEC
    0xC62828,   // MODULE
    0x37474F,   // BINARY
    0xAD1457,   // FONT
    0x00695C,   // LIBRARY
    0x6E7681    // OTHER
};

static void draw_file(ui_win_t* w, int x, int y, int kind) {
    if (kind < 0 || kind >= FILE_KIND_COUNT) kind = FILE_KIND_OTHER;
    uint32_t col = color_kind[kind];

    ui_fill(w, x + 2, y, 20, 26, 0xDCDCDC);
    ui_fill(w, x + 16, y, 6, 6, 0xB0B0B0);              // загнутый уголок
    ui_fill(w, x + 2, y + 23, 20, 3, col);              // цветная полоса вида файла

    if (kind == FILE_KIND_OTHER || kind == FILE_KIND_DIR) {
        ui_fill(w, x + 5, y + 8, 14, 2, 0x6E7681);      // безликий файл — просто строки
        ui_fill(w, x + 5, y + 13, 14, 2, 0x6E7681);
        ui_fill(w, x + 5, y + 18, 9, 2, 0x6E7681);
        return;
    }
    ui_icon16(w, x + 4, y + 5, icon_kind[kind], col);
}

// Подпись под значком: не шире ячейки
static void cell_label(ui_win_t* w, int cx, int y, const char* name, uint32_t color) {
    char buf[64];
    int max_chars = (CELL_W - 8) / UI_FONT_W, chars = 0, p = 0;
    for (const char* s = name; *s && p < (int)sizeof(buf) - 5; ) {
        int n = 1;
        while ((((uint8_t)s[n]) & 0xC0) == 0x80) n++;
        if (chars == max_chars - 1 && s[n]) { buf[p++] = '.'; break; }
        for (int k = 0; k < n; k++) buf[p++] = s[k];
        s += n;
        chars++;
    }
    buf[p] = '\0';
    ui_text_center(w, cx, y, CELL_W, buf, color);
}

static void draw_denied_dialog(ui_win_t* w) {
    // Затемняем окно под диалогом
    uint32_t* px = ui_pixels(w);
    for (int i = 0; i < g_ww * g_wh; i++) px[i] = (px[i] >> 1) & 0x7F7F7F;

    int dw = 420, dh = s_denied_can_unlock ? 196 : 140;
    int dx = (g_ww - dw) / 2, dy = (g_wh - dh) / 2;
    ui_fill(w, dx, dy, dw, dh, UI_BG);
    ui_frame(w, dx, dy, dw, dh, UI_DANGER);
    ui_fill(w, dx, dy, dw, 28, UI_DANGER);
    ui_text(w, dx + 12, dy + 6, "Доступ запрещён", UI_TEXT);
    ui_label(w, dx + 14, dy + 40, s_denied_msg);
    ui_text(w, dx + 14, dy + 60, s_denied_path, COLOR_FOLDER);

    if (s_denied_can_unlock) {
        ui_label_muted(w, dx + 14, dy + 86, "Пароль администратора (ваш пароль):");
        ui_focus_next_textbox(w);    // единственное поле модального диалога — ввод сразу в него
        int enter = ui_password(w, dx + 14, dy + 108, dw - 28, s_pw, sizeof(s_pw));
        int cancel = ui_button(w, dx + dw - 250, dy + dh - 42, 100, 28, "Отмена") || ui_key_pressed(w, WP_KEY_ESC);
        int unlock = ui_button_accent(w, dx + dw - 140, dy + dh - 42, 126, 28, "Разблокировать") || enter;
        if (cancel) { pw_wipe(); s_denied = 0; }
        else if (unlock) {
            int ok = (ov_sudo(s_pw) == 0);           // при неудаче ядро держит паузу ~1 с
            pw_wipe();
            if (!ok) {
                s_denied_msg = "Неверный пароль (проверьте раскладку).";
            } else {
                s_admin = 1;
                s_denied = 0;
                if (s_denied_retry) go_to(s_denied_path); else reload();
            }
        }
    } else {
        ui_label_muted(w, dx + 14, dy + 84, "Обратитесь к администратору.");
        if (ui_button_accent(w, dx + dw - 110, dy + dh - 42, 96, 28, "OK") ||
            ui_key_pressed(w, WP_KEY_ENTER) || ui_key_pressed(w, WP_KEY_ESC))
            s_denied = 0;
    }
}

int main(void) {
    if (libui_bind() < 0) ov_exit(1);     // тулкит из /lib/libui.ll
    s_orig_uid = ov_getuid();
    ov_user_t me;
    if (ov_getpwuid(s_orig_uid, &me)) ov_strlcpy(s_home, me.home, sizeof(s_home));

    char arg[128];
    if (ov_get_arg(arg, sizeof(arg)) > 0 && arg[0]) ov_strlcpy(s_cwd, arg, sizeof(s_cwd));
    if (reload() < 0) { ov_strlcpy(s_cwd, "/", sizeof(s_cwd)); reload(); }

    ui_app_id("ovfiles");       // панель задач узнаёт по классу уже запущенную программу
    ui_win_t* w = ui_open("overFiles — Файлы", WIN_W, WIN_H, icon_folder_win,
                           WP_FLAG_FILE_BROWSER | WP_FLAG_RESIZABLE);
    if (!w) ov_exit(1);

    uint64_t last_refresh = ov_ms();

    // Раз в секунду перечитываем каталог: файлы могли появиться из других программ
    while (ui_begin(w, 1000)) {
        // Окно могли развернуть — сетка и панели считаются от текущего размера
        g_ww = ui_width(w);
        g_wh = ui_height(w);

        if (!s_creating && !s_denied && ov_ms() - last_refresh >= 1000) {
            last_refresh = ov_ms();
            reload();
        }

        // Путь от композитора: "Открыть" в уведомлении о флешке, или файл
        // открыли, пока мы уже работали (единственный экземпляр)
        const char* req = ui_open_request(w);
        if (req && req[0] && !s_denied) go_to(req);

        // Флешку вынули, пока мы в ней -- уходим домой, а не в пустоту
        if (s_cwd[0] && path_under(s_cwd, "/media") && s_cwd[6] == '/' && !s_denied) {
            int alive = 0;
            for (int m = 0; m < s_nmounts; m++) if (path_under(s_cwd, s_mounts[m].path)) alive = 1;
            if (!alive) go_to(s_home);
        }

        // Команды из контекстного меню композитора
        int cmd = ui_command(w);
        if ((cmd == WP_CMD_CREATE_FOLDER || cmd == WP_CMD_CREATE_FILE) && !s_denied) {
            s_creating = 1;
            s_creating_dir = (cmd == WP_CMD_CREATE_FOLDER);
            ov_strlcpy(s_new_name, s_creating_dir ? "new" : "file.txt", sizeof(s_new_name));
        }

        // Клавиатура (если не открыт диалог и не вводится имя)
        if (!s_denied && !s_creating) {
            if (ui_key_pressed(w, WP_KEY_BACKSPACE)) go_up();
            if (ui_key_pressed(w, WP_KEY_ENTER) && s_sel >= 0) open_item(s_sel);
            if (ui_key_pressed(w, WP_KEY_RIGHT) && s_sel < s_count - 1) s_sel++;
            if (ui_key_pressed(w, WP_KEY_LEFT) && s_sel > 0) s_sel--;
            if (ui_key_pressed(w, WP_KEY_DOWN)) s_sel = (s_sel < 0) ? 0 : (s_sel + GRID_COLS < s_count ? s_sel + GRID_COLS : s_sel);
            if (ui_key_pressed(w, WP_KEY_UP) && s_sel >= GRID_COLS) s_sel -= GRID_COLS;
        }

        ui_clear(w);

        // --- Шапка: назад, путь ---
        ui_fill(w, 0, 0, g_ww, HEADER_H, UI_PANEL);
        ui_hline(w, 0, HEADER_H, g_ww, UI_BORDER);
        if (ui_button(w, 8, 7, 30, 26, "<") && !s_denied) go_up();
        ui_fill(w, 46, 7, g_ww - 54, 26, UI_BG_DARK);
        ui_frame(w, 46, 7, g_ww - 54, 26, UI_BORDER);
        ui_label_muted(w, 54, 12, "overOS >");
        ui_text(w, 54 + ui_text_width("overOS > "), 12, s_cwd, UI_TEXT);

        // --- Боковая панель: места ---
        ui_fill(w, 0, HEADER_H + 1, SIDEBAR_W, g_wh - HEADER_H - 1 - STATUS_H, UI_BG_DARK);
        ui_label_muted(w, 12, HEADER_H + 10, "Места");
        if (ui_button(w, 6, HEADER_H + 32, SIDEBAR_W - 12, 26, "Домашняя папка") && !s_denied) go_to(s_home);
        if (ui_button(w, 6, HEADER_H + 62, SIDEBAR_W - 12, 26, "Корень  /") && !s_denied) go_to("/");

        // --- Устройства: флешки в /media, щелчок -- открыть, значок справа -- извлечь
        if (s_nmounts) {
            int dy = HEADER_H + 100;
            ui_label_muted(w, 12, dy, "Устройства");
            dy += 22;
            for (int m = 0; m < s_nmounts; m++, dy += 30) {
                const ov_mount_t* mt = &s_mounts[m];
                int here = path_under(s_cwd, mt->path);
                int bw = SIDEBAR_W - 12;
                if (here) ui_fill(w, 6, dy, bw, 26, COLOR_SELECT_BG);
                else if (ui_hit(w, 6, dy, bw, 26)) ui_fill(w, 6, dy, bw, 26, UI_HOVER);
                ui_icon(w, 10, dy + 3, 20, "drive-removable", 0xFFFFFF);
                char lab[24];
                ov_strlcpy(lab, mt->label[0] ? mt->label : mt->name, sizeof(lab));
                ui_text(w, 34, dy + 5, lab, UI_TEXT);
                int ex = 6 + bw - 24;
                int ehot = ui_hit(w, ex, dy + 3, 20, 20);
                if (ehot) ui_fill(w, ex, dy + 3, 20, 20, UI_BORDER);
                ui_icon(w, ex + 2, dy + 5, 16, "media-eject", ehot ? UI_TEXT : 0x8F98A0);
                if (ui_mouse_clicked(w) && !s_denied) {
                    if (ehot) {
                        char name[24];
                        ov_strlcpy(name, mt->name, sizeof(name));
                        if (here) go_to(s_home);         // уходим с флешки, прежде чем её снять
                        ov_eject(name);
                        reload_mounts();
                        break;
                    } else if (ui_hit(w, 6, dy, bw, 26)) {
                        go_to(mt->path);
                    }
                }
            }
        }

        // --- Сетка ---
        int n = s_count + (s_creating ? 1 : 0);
        int total_rows = (n + GRID_COLS - 1) / GRID_COLS;
        if (!s_denied && ui_hit(w, SIDEBAR_W, HEADER_H, g_ww - SIDEBAR_W, g_wh - HEADER_H - STATUS_H))
            s_scroll_row += ui_scroll(w);
        if (s_sel >= 0) {
            int r = s_sel / GRID_COLS;
            if (r < s_scroll_row) s_scroll_row = r;
            if (r >= s_scroll_row + GRID_ROWS) s_scroll_row = r - GRID_ROWS + 1;
        }
        if (s_creating) s_scroll_row = (total_rows > GRID_ROWS) ? total_rows - GRID_ROWS : 0;
        if (s_scroll_row > total_rows - GRID_ROWS) s_scroll_row = total_rows - GRID_ROWS;
        if (s_scroll_row < 0) s_scroll_row = 0;

        int clicked_item = 0;
        for (int i = s_scroll_row * GRID_COLS; i < n; i++) {
            int row = i / GRID_COLS - s_scroll_row, col = i % GRID_COLS;
            if (row >= GRID_ROWS) break;
            int cx = GRID_X + col * CELL_W, cy = GRID_Y + row * CELL_H;

            if (s_creating && i == s_count) {
                ui_fill(w, cx, cy, CELL_W - 6, CELL_H - 6, COLOR_SELECT_BG);
                if (s_creating_dir) draw_folder(w, cx + (CELL_W - 38) / 2, cy + 6, 1);
                else draw_file(w, cx + (CELL_W - 28) / 2, cy + 6, FILE_KIND_OTHER);
                ui_focus_next_textbox(w);    // имя вводится сразу, без клика по полю
                if (ui_textbox(w, cx - 4, cy + 40, CELL_W + 2, s_new_name, sizeof(s_new_name))) commit_create();
                else if (ui_key_pressed(w, WP_KEY_ESC)) s_creating = 0;
                clicked_item |= ui_hit(w, cx - 4, cy + 40, CELL_W + 2, 26);
                continue;
            }

            int sel = (i == s_sel);
            int hot = !s_denied && ui_hit(w, cx, cy, CELL_W - 6, CELL_H - 6);
            if (sel) { ui_fill(w, cx, cy, CELL_W - 6, CELL_H - 6, COLOR_SELECT_BG); ui_frame(w, cx, cy, CELL_W - 6, CELL_H - 6, COLOR_FOLDER); }
            else if (hot) ui_fill(w, cx, cy, CELL_W - 6, CELL_H - 6, UI_HOVER);
            if (s_files[i].attr & OV_ATTR_DIR) draw_folder(w, cx + (CELL_W - 38) / 2, cy + 6, sel);
            else {
                // У программы свой значок -- вшит в её .prg (ресурс ICON), как у .exe
                char full[160];
                int drawn = 0;
                if (s_kind[i] == FILE_KIND_EXEC) {
                    path_join(s_cwd, item_name(i), full, sizeof(full));
                    drawn = ui_icon(w, cx + (CELL_W - 38) / 2 - 2, cy + 2, 36, full, 0xDDE3EA);
                }
                if (!drawn) draw_file(w, cx + (CELL_W - 28) / 2, cy + 6, s_kind[i]);
            }
            cell_label(w, cx - 3, cy + 44, item_name(i), sel ? UI_TEXT : 0xD0D4D8);

            if (hot && ui_mouse_clicked(w)) {
                clicked_item = 1;
                if (s_sel == i) open_item(i);     // повторный клик / двойной клик — открыть
                else s_sel = i;
            }
        }
        // Клик в пустое место снимает выделение; клик мимо поля имени — создаём
        if (!s_denied && ui_mouse_clicked(w) && !clicked_item &&
            ui_hit(w, SIDEBAR_W, HEADER_H, g_ww - SIDEBAR_W, g_wh - HEADER_H - STATUS_H)) {
            if (s_creating) commit_create();
            s_sel = -1;
        }

        // --- Статус ---
        int sy = g_wh - STATUS_H;
        if (s_admin) {
            ui_fill(w, SIDEBAR_W, sy - 20, g_ww - SIDEBAR_W, 20, 0x7A1F2B);
            ui_text(w, SIDEBAR_W + 10, sy - 18, "Режим администратора: будьте осторожны", UI_TEXT);
        }
        ui_fill(w, 0, sy, g_ww, STATUS_H, UI_PANEL);
        ui_hline(w, 0, sy, g_ww, UI_BORDER);
        char status[128];
        if (s_sel >= 0) {
            char full[160];
            path_join(s_cwd, item_name(s_sel), full, sizeof(full));
            ext_get_info_string(full, item_name(s_sel), s_files[s_sel].attr, s_files[s_sel].size, status, sizeof(status));
        } else {
            char a[12], b[12];
            ui_itoa(s_folders, a);
            ui_itoa(s_count - s_folders, b);
            int p = 0;
            const char* parts[] = { "Папок: ", a, ", файлов: ", b };
            for (int k = 0; k < 4; k++) for (const char* c = parts[k]; *c; c++) status[p++] = *c;
            status[p] = '\0';
        }
        ui_label_muted(w, 10, sy + 4, status);

        if (s_denied) draw_denied_dialog(w);

        ui_end(w);
    }

    ui_close(w);
    ov_exit(0);
    return 0;
}
