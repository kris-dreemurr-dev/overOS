#include "gfx.h"
#include "libgfx/bmp.h"
#include "libgfx/png.h"
#include "syscalls.h"
#include "icons.h"
#include "cursors.h"
#include "libgfx/uifont.h"
#include "libgfx/svg.h"
#include "prgres.h"
#include "theme.h"
#include "taskbar.h"

uint32_t* g_backbuffer = NULL;
uint32_t  g_screen_w = 0;
uint32_t  g_screen_h = 0;
uint32_t  g_buf_bytes = 0;
int       g_fb_fd = -1;

void gfx_init(int fb_fd) {
    g_fb_fd = fb_fd;
    fb_var_info_t fb_info = {0};
    int res = sys_ioctl(g_fb_fd, 0x4600, &fb_info);

    if (res < 0 || fb_info.width == 0 || fb_info.height == 0) {
        g_screen_w = 1600;
        g_screen_h = 900;
    } else {
        g_screen_w = fb_info.width;
        g_screen_h = fb_info.height;
    }

    g_buf_bytes = g_screen_w * g_screen_h * 4;
    g_backbuffer = (uint32_t*)sys_sbrk((int64_t)g_buf_bytes);

    // Шрифт интерфейса читается из /sys/fonts/ui.ttf. Не нашёлся — рисуем
    // растровым 8x16: интерфейс останется читаемым, просто «консольным».
    uif_init();
}

void gfx_draw_rect(int rx, int ry, int rw, int rh, uint32_t color) {
    if (!g_backbuffer) return;

    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { ry += ry; ry = 0; }
    if (rx + rw > (int)g_screen_w) rw = g_screen_w - rx;
    if (ry + rh > (int)g_screen_h) rh = g_screen_h - ry;
    if (rw <= 0 || rh <= 0) return;

    uint64_t pair_color = ((uint64_t)color << 32) | color;

    for (int y = 0; y < rh; y++) {
        uint32_t* row = &g_backbuffer[(ry + y) * g_screen_w + rx];
        int qwords = rw / 2;
        uint64_t* row64 = (uint64_t*)row;
        for (int q = 0; q < qwords; q++) {
            row64[q] = pair_color;
        }
        if (rw & 1) {
            row[rw - 1] = color;
        }
    }
}

// Один глиф шрифта: y — верх строки, базовая линия на уровне uif_baseline()
static void gfx_glyph(int x, int y, const ttf_glyph_t* g, int px, uint32_t color) {
    const uint8_t* cov = uif_cov(g);
    int gx = x + g->bx, gy = y + uif_baseline(px) - g->by;
    for (int r = 0; r < g->h; r++) {
        int py = gy + r;
        if (py < 0 || py >= (int)g_screen_h) continue;
        uint32_t* row = &g_backbuffer[py * g_screen_w];
        for (int c = 0; c < g->w; c++) {
            uint32_t a = cov[r * g->w + c];
            int pxx = gx + c;
            if (!a || pxx < 0 || pxx >= (int)g_screen_w) continue;
            row[pxx] = uif_blend(row[pxx], color, a);
        }
    }
}

// Запасной путь: шрифт не загрузился — рисуем прежним растровым 8x16
static void gfx_mono_glyph(int x, int y, uint32_t cp, uint32_t color) {
    const uint8_t* rows = uif_mono_rows(cp);
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < 0 || py >= (int)g_screen_h) continue;
        uint8_t bits = rows[r];
        for (int c = 0; c < 8; c++) {
            int pxx = x + c;
            if ((bits & (0x80 >> c)) && pxx >= 0 && pxx < (int)g_screen_w)
                g_backbuffer[py * g_screen_w + pxx] = color;
        }
    }
}

static void gfx_string_px(int x, int y, const char* str, uint32_t color, int px) {
    const uint8_t* s = (const uint8_t*)str;
    while (*s) {
        uint32_t cp = uif_utf8_next(&s);
        if (!uif_ok()) { gfx_mono_glyph(x, y, cp, color); x += UIF_MONO_W; continue; }
        const ttf_glyph_t* g = uif_glyph(cp, px);
        if (!g) continue;
        gfx_glyph(x, y, g, px, color);
        x += g->adv;
    }
}

// Отдельный символ CP866 (цифры и прочее, что приходит байтами, а не строкой)
void gfx_draw_char(int x, int y, uint8_t ch, uint32_t color) {
    uint32_t cp = ch;
    if (ch >= 0x80 && ch <= 0xAF) cp = 0x410 + (ch - 0x80);
    else if (ch >= 0xE0 && ch <= 0xEF) cp = 0x440 + (ch - 0xE0);
    else if (ch == 0xF0) cp = 0x401;
    else if (ch == 0xF1) cp = 0x451;
    if (!uif_ok()) { gfx_mono_glyph(x, y, cp, color); return; }
    const ttf_glyph_t* g = uif_glyph(cp, UIF_PX);
    if (g) gfx_glyph(x, y, g, UIF_PX, color);
}

void gfx_draw_string(int x, int y, const char* str, uint32_t color) {
    gfx_string_px(x, y, str, color, UIF_PX);
}

// Ширина строки: шрифт переменной ширины, считать символы больше нельзя
int gfx_text_width(const char* str) { return uif_text_width(str); }

void gfx_draw_dec(int x, int y, int val, uint32_t color) {
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    if (val == 0) buf[i--] = '0';
    while (val > 0 && i >= 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    gfx_draw_string(x, y, &buf[i + 1], color);
}

void gfx_draw_dec2(int x, int y, int val, uint32_t color) {
    gfx_draw_char(x, y, (uint8_t)('0' + ((val / 10) % 10)), color);
    gfx_draw_char(x + 8, y, (uint8_t)('0' + (val % 10)), color);
}

// =============================================================================
// Значки: системные (тема) -- .svg из /sys/icons, значки программ -- ресурс
// ICON внутри самого .prg (tools/elf2prg.py --icon). Имя, начинающееся с '/',
// -- это путь к программе: "/desprogs/ovfiles.prg". Растеризует система сама,
// каждый значок один раз на размер, и в кэше он лежит как ARGB.
// Монохромные значки рисуются с currentColor = белый, а цвет задаётся при
// выводе (умножением), поэтому один файл годится и для обычного, и для
// подсвеченного, и для приглушённого состояния. Цветные (флешка в
// уведомлении) рисуются с белым оттенком -- то есть как есть.
// =============================================================================
#define ICON_SLOTS    64
#define ICON_NAME_MAX 96
#define ICON_MAX_PX   (48 * 48)
#define ICON_FILE_MAX 8192

typedef struct {
    char name[ICON_NAME_MAX];
    int  size;
    int  ok;
    uint32_t* px;
} icon_slot_t;

static icon_slot_t g_icons[ICON_SLOTS];
static uint32_t    g_icon_arena[ICON_SLOTS * ICON_MAX_PX];
static int         g_icon_count = 0;

static int icon_name_eq(const char* a, const char* b) {
    for (int i = 0; i < ICON_NAME_MAX - 1; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

// Значок нужного размера: индекс в кэше, -1 — значка нет
int gfx_icon(const char* name, int size) {
    if (!name || !name[0] || size <= 0 || size * size > ICON_MAX_PX) return -1;
    for (int i = 0; i < g_icon_count; i++)
        if (g_icons[i].size == size && icon_name_eq(g_icons[i].name, name))
            return g_icons[i].ok ? i : -1;
    if (g_icon_count >= ICON_SLOTS) return -1;

    icon_slot_t* sl = &g_icons[g_icon_count];
    int n = 0;
    while (name[n] && n < ICON_NAME_MAX - 1) { sl->name[n] = name[n]; n++; }
    sl->name[n] = '\0';
    sl->size = size;
    sl->ok = 0;
    sl->px = &g_icon_arena[g_icon_count * ICON_MAX_PX];
    g_icon_count++;

    static uint8_t file[ICON_FILE_MAX];
    int total = 0;
    if (name[0] == '/') {
        // Значок программы -- из её исполняемого файла
        total = prg_resource(name, "ICON", file, ICON_FILE_MAX);
    } else {
        char path[64];
        int p = 0;
        const char* dir = "/sys/icons/";
        while (*dir) path[p++] = *dir++;
        for (int i = 0; sl->name[i] && p < 58; i++) path[p++] = sl->name[i];
        const char* ext = ".svg";
        while (*ext) path[p++] = *ext++;
        path[p] = '\0';
        int fd = sys_open(path, 0);
        if (fd < 0) return -1;
        while (total < ICON_FILE_MAX) {
            int r = sys_read(fd, (char*)file + total, ICON_FILE_MAX - total);
            if (r <= 0) break;
            total += r;
        }
        sys_close(fd);
    }
    if (total <= 0) return -1;
    if (svg_render(file, (uint32_t)total, sl->px, size, size, 0xFFFFFF) != 0) return -1;
    sl->ok = 1;
    return g_icon_count - 1;
}

// Вывести значок из кэша. tint — цвет (монохромные значки им окрашиваются).
void gfx_draw_icon(int x, int y, int idx, uint32_t tint) {
    if (idx < 0 || idx >= g_icon_count || !g_icons[idx].ok) return;
    const icon_slot_t* sl = &g_icons[idx];
    int size = sl->size;
    for (int r = 0; r < size; r++) {
        int py = y + r;
        if (py < 0 || py >= (int)g_screen_h) continue;
        uint32_t* row = &g_backbuffer[py * g_screen_w];
        for (int c = 0; c < size; c++) {
            uint32_t p = sl->px[r * size + c];
            uint32_t a = p >> 24;
            int px = x + c;
            if (!a || px < 0 || px >= (int)g_screen_w) continue;
            uint32_t cr = ((p >> 16) & 0xFF) * ((tint >> 16) & 0xFF) / 255;
            uint32_t cg = ((p >> 8) & 0xFF) * ((tint >> 8) & 0xFF) / 255;
            uint32_t cb = (p & 0xFF) * (tint & 0xFF) / 255;
            row[px] = uif_blend(row[px], (cr << 16) | (cg << 8) | cb, a);
        }
    }
}

// Удобная обёртка: загрузить (при первом вызове) и сразу нарисовать
void gfx_icon_draw(int x, int y, const char* name, int size, uint32_t tint) {
    gfx_draw_icon(x, y, gfx_icon(name, size), tint);
}

void gfx_draw_mono_icon(int x, int y, const uint16_t* mask, uint32_t fg, uint32_t alt_col, int has_alt) {
    for (int r = 0; r < 16; r++) {
        for (int c = 0; c < 16; c++) {
            if (mask[r] & (1 << (15 - c))) {
                uint32_t col = (has_alt && c >= 7) ? alt_col : fg;
                int px = x + c, py = y + r;
                if (px >= 0 && px < (int)g_screen_w && py >= 0 && py < (int)g_screen_h) {
                    g_backbuffer[py * g_screen_w + px] = col;
                }
            }
        }
    }
}

// --- Программный курсор -----------------------------------------------------
// Движение мыши — не повод перерисовывать обои, окна и панель: под курсором
// сохраняется кусок заднего буфера, и на следующем кадре он возвращается на
// место. Раньше каждый шаг мыши стоил полной отрисовки сцены, и композитор
// съедал почти половину процессора просто потому, что курсор ехал.
#define CUR_BOX 24
static uint32_t s_cur_bak[CUR_BOX * CUR_BOX];
static int s_cur_have = 0, s_cur_bx = 0, s_cur_by = 0, s_cur_bw = 0, s_cur_bh = 0;

// Сцену перерисовали — сохранённый кусок больше не соответствует буферу
void gfx_cursor_forget(void) { s_cur_have = 0; }

void gfx_cursor_restore(void) {
    if (!s_cur_have || !g_backbuffer) { s_cur_have = 0; return; }
    for (int r = 0; r < s_cur_bh; r++) {
        uint32_t* d = &g_backbuffer[(uint32_t)(s_cur_by + r) * g_screen_w + s_cur_bx];
        const uint32_t* b = &s_cur_bak[r * CUR_BOX];
        for (int c = 0; c < s_cur_bw; c++) d[c] = b[c];
    }
    s_cur_have = 0;
}

// Строки, затронутые курсором (для частичного вывода кадра)
void gfx_cursor_rows(int* y0, int* y1) {
    if (!s_cur_have) { *y0 = 0; *y1 = 0; return; }
    *y0 = s_cur_by;
    *y1 = s_cur_by + s_cur_bh;
}

void gfx_draw_cursor(int mx, int my, int shape) {
    if (shape == GFX_CUR_HIDDEN) { s_cur_have = 0; return; }
    if (shape < 0 || shape >= WP_CUR_COUNT) shape = WP_CUR_ARROW;
    const cursor_shape_t* c = &g_cursors[shape];

    // Сохраняем то, что под курсором (с запасом на горячую точку)
    if (g_backbuffer) {
        int bx = mx - c->hot_x, by = my - c->hot_y;
        if (bx < 0) bx = 0;
        if (by < 0) by = 0;
        int bw = CUR_W, bh = CUR_H;
        if (bx + bw > (int)g_screen_w) bw = (int)g_screen_w - bx;
        if (by + bh > (int)g_screen_h) bh = (int)g_screen_h - by;
        if (bw > 0 && bh > 0) {
            s_cur_bx = bx; s_cur_by = by; s_cur_bw = bw; s_cur_bh = bh;
            for (int r = 0; r < bh; r++) {
                const uint32_t* src = &g_backbuffer[(uint32_t)(by + r) * g_screen_w + bx];
                uint32_t* dst = &s_cur_bak[r * CUR_BOX];
                for (int cx = 0; cx < bw; cx++) dst[cx] = src[cx];
            }
            s_cur_have = 1;
        }
    }

    for (int y = 0; y < CUR_H; y++) {
        const char* row = c->rows[y];
        for (int x = 0; x < CUR_W; x++) {
            char ch = row[x];
            if (ch != '#' && ch != '.') continue;
            int px = mx - c->hot_x + x, py = my - c->hot_y + y;
            if (px < 0 || px >= (int)g_screen_w || py < 0 || py >= (int)g_screen_h) continue;
            g_backbuffer[py * g_screen_w + px] = (ch == '#') ? 0x000000 : 0xFFFFFF;
        }
    }
}

void gfx_present(void) {
    if (g_fb_fd >= 0) {
        sys_lseek(g_fb_fd, 0, 0);
        sys_write(g_fb_fd, g_backbuffer, g_buf_bytes);
    }
}

// Вывести только строки [y0, y1): ядро пишет в LFB по смещению, а сравнение с
// теневой копией идёт лишь по этим строкам. Для едущего курсора это два десятка
// строк вместо всего экрана.
void gfx_present_rows(int y0, int y1) {
    if (g_fb_fd < 0 || !g_backbuffer) return;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)g_screen_h) y1 = (int)g_screen_h;
    if (y1 <= y0) return;
    if (y0 == 0 && y1 == (int)g_screen_h) { gfx_present(); return; }

    uint32_t row = g_screen_w * 4;
    sys_lseek(g_fb_fd, (long)y0 * row, 0);
    sys_write(g_fb_fd, (const uint8_t*)g_backbuffer + (uint64_t)y0 * row,
              (uint32_t)(y1 - y0) * row);
    sys_lseek(g_fb_fd, 0, 0);
}

void gfx_blit(int x, int y, int w, int h, const uint32_t* src) {
    if (!g_backbuffer || !src) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w > (int)g_screen_w) ? (int)g_screen_w : x + w;
    if (x1 <= x0) return;
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= (int)g_screen_h) continue;
        const uint32_t* s = src + (uint32_t)r * (uint32_t)w + (x0 - x);
        uint32_t* d = &g_backbuffer[py * g_screen_w + x0];
        for (int c = 0; c < x1 - x0; c++) d[c] = s[c];
    }
}

uint32_t* g_wallpaper_buf = NULL;
int       g_has_wallpaper = 0;

void gfx_load_wallpaper(const char* filepath) {
    g_has_wallpaper = 0;
    if (!filepath || !filepath[0]) return;

    int fd = sys_open(filepath, 0);
    if (fd < 0) {
        show_error_dialog("Обои: файл не найден");
        return;
    }

    // Буфер под файл. Разбор форматов -- общий (src/lib/bmp.h и png.h), своего
    // декодера BMP у рабочего стола больше нет: и кода меньше, и обои можно
    // ставить в PNG.
    #define MAX_WALLPAPER_FILE_SIZE (4 * 1024 * 1024)
    static uint8_t* s_file_data = NULL;
    if (!s_file_data) {
        s_file_data = (uint8_t*)sys_sbrk((int64_t)MAX_WALLPAPER_FILE_SIZE);
        if (s_file_data == (void*)(uintptr_t)-1) {
            show_error_dialog("Обои: не удалось выделить память под чтение файла");
            sys_close(fd);
            return;
        }
    }

    int file_bytes = sys_read(fd, s_file_data, MAX_WALLPAPER_FILE_SIZE);
    sys_close(fd);
    if (file_bytes < 32) {
        show_error_dialog("Обои: файл слишком мал или повреждён");
        return;
    }

    // Размеры читаем из заголовка, чтобы выделить ровно столько, сколько надо
    int img_w = 0, img_h = 0, is_png = 0;
    if (s_file_data[0] == 0x89 && s_file_data[1] == 'P' && s_file_data[2] == 'N' && s_file_data[3] == 'G') {
        is_png = 1;
        img_w = (int)(((uint32_t)s_file_data[16] << 24) | ((uint32_t)s_file_data[17] << 16) |
                      ((uint32_t)s_file_data[18] << 8) | s_file_data[19]);
        img_h = (int)(((uint32_t)s_file_data[20] << 24) | ((uint32_t)s_file_data[21] << 16) |
                      ((uint32_t)s_file_data[22] << 8) | s_file_data[23]);
    } else if (s_file_data[0] == 'B' && s_file_data[1] == 'M') {
        const bmp_info_hdr_t* ih = (const bmp_info_hdr_t*)(s_file_data + 14);
        img_w = ih->width;
        img_h = ih->height < 0 ? -ih->height : ih->height;
    } else {
        show_error_dialog("Обои: поддерживаются только BMP и PNG");
        return;
    }
    if (img_w <= 0 || img_h <= 0 || (uint64_t)img_w * img_h > 4000000ULL) {
        show_error_dialog("Обои: неподдерживаемый размер картинки");
        return;
    }

    static uint32_t* s_argb = NULL;
    static uint32_t  s_argb_px = 0;
    uint32_t need_px = (uint32_t)img_w * (uint32_t)img_h;
    if (s_argb_px < need_px) {
        s_argb = (uint32_t*)sys_sbrk((int64_t)(need_px * 4));
        if (s_argb == (void*)(uintptr_t)-1) { s_argb = NULL; s_argb_px = 0;
            show_error_dialog("Обои: нет памяти под картинку"); return; }
        s_argb_px = need_px;
    }

    if (is_png) {
        static uint8_t* s_scratch = NULL;
        static uint32_t s_scratch_cap = 0;
        uint32_t need = (uint32_t)img_h * (1 + (uint32_t)img_w * 4) + 64;
        if (s_scratch_cap < need) {
            s_scratch = (uint8_t*)sys_sbrk((int64_t)need);
            if (s_scratch == (void*)(uintptr_t)-1) { s_scratch = NULL; s_scratch_cap = 0;
                show_error_dialog("Обои: нет памяти под распаковку PNG"); return; }
            s_scratch_cap = need;
        }
        png_info_t pi;
        if (png_decode(s_file_data, (uint32_t)file_bytes, s_argb, s_argb_px,
                       s_scratch, s_scratch_cap, &pi) != PNG_OK) {
            show_error_dialog("Обои: PNG не распакован");
            return;
        }
        img_w = pi.w; img_h = pi.h;
    } else {
        bmp_info_t bi;
        if (bmp_decode(s_file_data, (uint32_t)file_bytes, s_argb, s_argb_px, &bi) != BMP_OK) {
            show_error_dialog("Обои: BMP не разобран");
            return;
        }
        img_w = bi.w; img_h = bi.h;
    }

    // Холст экрана
    if (!g_wallpaper_buf) {
        g_wallpaper_buf = (uint32_t*)sys_sbrk((int64_t)g_buf_bytes);
        if (g_wallpaper_buf == (void*)(uintptr_t)-1) {
            show_error_dialog("Обои: нет памяти под буфер экрана");
            g_wallpaper_buf = NULL;
            return;
        }
    }
    for (uint32_t i = 0; i < g_screen_w * g_screen_h; i++) g_wallpaper_buf[i] = COLOR_DESKTOP_BG;

    // Масштабирование под экран (ближайший сосед, шаг 16.16)
    int max_h = (int)g_screen_h - TASKBAR_HEIGHT;
    uint32_t x_step = ((uint32_t)img_w << 16) / g_screen_w;
    uint32_t y_step = ((uint32_t)img_h << 16) / (uint32_t)max_h;
    for (int dy = 0; dy < max_h; dy++) {
        int sy = (int)(((uint32_t)dy * y_step) >> 16);
        if (sy >= img_h) sy = img_h - 1;
        uint32_t* dst_row = &g_wallpaper_buf[dy * g_screen_w];
        const uint32_t* src_row = &s_argb[(uint32_t)sy * (uint32_t)img_w];
        for (int dx = 0; dx < (int)g_screen_w; dx++) {
            int sx = (int)(((uint32_t)dx * x_step) >> 16);
            if (sx >= img_w) sx = img_w - 1;
            dst_row[dx] = src_row[sx] | 0xFF000000u;
        }
    }
    g_has_wallpaper = 1;
}

void gfx_draw_wallpaper(void) {
    if (g_has_wallpaper && g_wallpaper_buf) {
        int max_y = (int)g_screen_h - TASKBAR_HEIGHT;
        for (int y = 0; y < max_y; y++) {
            uint32_t* dst = &g_backbuffer[y * g_screen_w];
            const uint32_t* src = &g_wallpaper_buf[y * g_screen_w];
            int qwords = g_screen_w / 2;
            uint64_t* dst64 = (uint64_t*)dst;
            const uint64_t* src64 = (const uint64_t*)src;
            for (int q = 0; q < qwords; q++) {
                dst64[q] = src64[q];
            }
            if (g_screen_w & 1) dst[g_screen_w - 1] = src[g_screen_w - 1];
        }
    } else {
        // Fallback: стандартный темный фон рабочего стола
        gfx_draw_rect(0, 0, g_screen_w, (int)g_screen_h - TASKBAR_HEIGHT, COLOR_DESKTOP_BG);
    }
}
// Символ, увеличенный в scale раз (часы и аватар на экране входа)
// Крупный текст — тот же шрифт, просто другой кегль: растеризатор посчитает
// глифы нужного размера и положит их в кэш рядом с обычными
void gfx_draw_char_scaled(int x, int y, uint8_t ch, uint32_t color, int scale) {
    if (scale < 2) { gfx_draw_char(x, y, ch, color); return; }
    char one[2] = { (char)ch, '\0' };
    gfx_string_px(x, y, one, color, UIF_PX_LG);
}

void gfx_draw_string_scaled(int x, int y, const char* str, uint32_t color, int scale) {
    gfx_string_px(x, y, str, color, (scale < 2) ? UIF_PX : UIF_PX_LG);
}

int gfx_text_width_lg(const char* str) { return uifl_text_width(str); }

// Строка любого кегля (заголовок уведомления крупнее текста под ним)
void gfx_draw_string_px(int x, int y, const char* str, uint32_t color, int px) {
    gfx_string_px(x, y, str, color, px);
}
int gfx_text_width_px(const char* str, int px) { return uif_width_px(str, px); }

// Прямоугольник со скруглёнными углами: углы заданы маской corners (бит 0 —
// левый верхний, 1 — правый верхний, 2 — левый нижний, 3 — правый нижний).
// Нужен для шапок окон и панели: без скруглений интерфейс выглядит «квадратным»,
// а растягивать картинки ради этого дорого — считаем отступ по окружности.
void gfx_round_rect(int x, int y, int w, int h, int r, int corners, uint32_t color) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int row = 0; row < h; row++) {
        int inset = 0;
        int top = (row < r), bot = (row >= h - r);
        if (top || bot) {
            int dy = top ? (r - 1 - row) : (row - (h - r));
            int d = r * r - (dy + 1) * (dy + 1);
            int k = 0;
            while ((k + 1) * (k + 1) <= d) k++;        // целочисленный sqrt
            inset = r - k;
        }
        int left  = inset, right = inset;
        if (top)  { if (!(corners & 1)) left = 0; if (!(corners & 2)) right = 0; }
        if (bot)  { if (!(corners & 4)) left = 0; if (!(corners & 8)) right = 0; }
        gfx_draw_rect(x + left, y + row, w - left - right, 1, color);
    }
}

// Кадр клиента со скруглёнными углами: пиксели углов не трогаем вовсе, и там
// остаётся то, что уже нарисовано (обои или окно ниже) — поэтому скругление
// работает и поверх других окон, без прозрачности и без лишнего буфера.
void gfx_blit_round(int x, int y, int w, int h, const uint32_t* src, int r, int corners) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= (int)g_screen_h) continue;
        int inset = 0;
        int top = (row < r), bot = (row >= h - r);
        if (top || bot) {
            int dy = top ? (r - 1 - row) : (row - (h - r));
            int d = r * r - (dy + 1) * (dy + 1);
            int k = 0;
            while ((k + 1) * (k + 1) <= d) k++;
            inset = r - k;
        }
        int left = inset, right = inset;
        if (top) { if (!(corners & 1)) left = 0; if (!(corners & 2)) right = 0; }
        if (bot) { if (!(corners & 4)) left = 0; if (!(corners & 8)) right = 0; }
        int x0 = x + left, x1 = x + w - right;
        if (x0 < 0) x0 = 0;
        if (x1 > (int)g_screen_w) x1 = (int)g_screen_w;
        const uint32_t* srow = &src[(uint32_t)row * (uint32_t)w];
        uint32_t* drow = &g_backbuffer[py * g_screen_w];
        for (int px = x0; px < x1; px++) drow[px] = srow[px - x];
    }
}

// Отрезок (кнопки окна рисуются линиями, а не картинками)
void gfx_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps <= 0) { gfx_draw_rect(x0, y0, 1, 1, color); return; }
    for (int i = 0; i <= steps; i++) {
        gfx_draw_rect(x0 + dx * i / steps, y0 + dy * i / steps, 1, 1, color);
    }
}

// Затемнение области: каждый канал умножается на num/256
void gfx_darken(int rx, int ry, int rw, int rh, int num) {
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > (int)g_screen_w) rw = (int)g_screen_w - rx;
    if (ry + rh > (int)g_screen_h) rh = (int)g_screen_h - ry;
    for (int y = ry; y < ry + rh; y++) {
        uint32_t* row = &g_backbuffer[y * g_screen_w];
        for (int x = rx; x < rx + rw; x++) {
            uint32_t p = row[x];
            uint32_t r = (((p >> 16) & 0xFF) * (uint32_t)num) >> 8;
            uint32_t g = (((p >> 8) & 0xFF) * (uint32_t)num) >> 8;
            uint32_t b = ((p & 0xFF) * (uint32_t)num) >> 8;
            row[x] = (r << 16) | (g << 8) | b;
        }
    }
}

// Закрашенный круг (аватар)
void gfx_fill_circle(int cx, int cy, int rad, uint32_t color) {
    for (int dy = -rad; dy <= rad; dy++) {
        int span = 0;
        while ((span + 1) * (span + 1) + dy * dy <= rad * rad) span++;
        gfx_draw_rect(cx - span, cy + dy, span * 2 + 1, 1, color);
    }
}

// =============================================================================
// Скруглённые углы со сглаживанием и мягкая тень.
//
// gfx_round_rect считает отступ по окружности в целых пикселях, и на карточке
// уведомления ступеньки видны. Здесь край считается с точностью до 1/256
// пикселя, а крайний пиксель подмешивается к фону по доле закрытия -- та же
// идея, что у растеризатора шрифта.
// =============================================================================
static uint32_t gfx_isqrt64(uint64_t v) {
    uint64_t x = 0, bit = 1ULL << 42;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= x + bit) { v -= x + bit; x = (x >> 1) + bit; }
        else x >>= 1;
        bit >>= 2;
    }
    return (uint32_t)x;
}

// Отступ края строки от края прямоугольника в 1/256 пикселя
static int gfx_inset_fp(int row, int h, int r) {
    int dy_fp = 0;
    if (row < r) dy_fp = (r - row) * 256 - 128;
    else if (row >= h - r) dy_fp = (row - (h - r)) * 256 + 128;
    if (dy_fp <= 0) return 0;
    int64_t rr = (int64_t)r * 256; rr *= rr;
    int64_t dd = (int64_t)dy_fp * dy_fp;
    int dx = (dd >= rr) ? 0 : (int)gfx_isqrt64((uint64_t)(rr - dd));
    int ins = r * 256 - dx;
    return ins < 0 ? 0 : ins;
}

// Целый отступ края (для колец тени, где доли не нужны)
static int gfx_inset(int row, int h, int r) {
    int dy;
    if (row < r) dy = r - row;
    else if (row >= h - r) dy = row - (h - r) + 1;
    else return 0;
    int d = r * r - dy * dy;
    if (d < 0) d = 0;
    int k = 0;
    while ((k + 1) * (k + 1) <= d) k++;
    return r - k;
}

static void gfx_blend_px(int px, int py, uint32_t color, uint32_t a) {
    if (!a || px < 0 || py < 0 || px >= (int)g_screen_w || py >= (int)g_screen_h) return;
    uint32_t* p = &g_backbuffer[(uint32_t)py * g_screen_w + px];
    *p = uif_blend(*p, color, a);
}

void gfx_round_rect_aa(int x, int y, int w, int h, int r, uint32_t color) {
    if (!g_backbuffer || w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= (int)g_screen_h) continue;
        int ins = gfx_inset_fp(row, h, r);
        int edge = ins >> 8, frac = ins & 255;
        int start = frac ? edge + 1 : edge;
        if (w - 2 * start > 0) gfx_draw_rect(x + start, py, w - 2 * start, 1, color);
        if (frac) {
            uint32_t a = (uint32_t)(255 - frac);
            gfx_blend_px(x + edge, py, color, a);
            gfx_blend_px(x + w - 1 - edge, py, color, a);
        }
    }
}

// Мягкая тень вокруг скруглённой карточки: кольцами наружу, каждое следующее
// слабее. Внутрь не лезем -- там всё равно будет сама карточка, а без этого
// тень пришлось бы считать по всей площади.
void gfx_shadow_round(int x, int y, int w, int h, int r, int spread, int alpha0) {
    if (!g_backbuffer || spread <= 0) return;
    for (int i = spread; i >= 1; i--) {
        int a = alpha0 * (spread - i + 1) / spread;
        int num = 256 - (a > 255 ? 255 : a);
        int X0 = x - i, Y0 = y - i, W = w + 2 * i, H = h + 2 * i, R = r + i;
        int X1 = x - (i - 1), Y1 = y - (i - 1), W1 = w + 2 * (i - 1), H1 = h + 2 * (i - 1), R1 = r + i - 1;
        for (int row = 0; row < H; row++) {
            int py = Y0 + row;
            if (py < 0 || py >= (int)g_screen_h) continue;
            int ins = gfx_inset(row, H, R);
            int l = X0 + ins, rt = X0 + W - ins;
            if (py >= Y1 && py < Y1 + H1) {
                int ii = gfx_inset(py - Y1, H1, R1);
                int li = X1 + ii, ri = X1 + W1 - ii;
                if (li > l) gfx_darken(l, py, li - l, 1, num);
                if (rt > ri) gfx_darken(ri, py, rt - ri, 1, num);
            } else {
                gfx_darken(l, py, rt - l, 1, num);
            }
        }
    }
}
