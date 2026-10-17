// overImages — просмотр изображений BMP и PNG на тулките libui (src/lib/ui.h)
//
// Оба формата приводятся при загрузке к одному виду — массиву ARGB s_argb —
// поэтому отрисовка знает только про масштаб и прозрачность, а не про формат.
#include <stdint.h>
#include <stddef.h>
#include "libui/ui.h"
#include "libgfx/png.h"
#include "libgfx/bmp.h"
#include "libgfx/libgfx_ll.h"
#include "libui/libui_ll.h"

#define WIN_W 560
#define WIN_H 400

// Окно разворачивается: раскладка и масштаб считаются от текущего размера
static int g_ww = WIN_W, g_wh = WIN_H;
#define TOOLBAR_H 40
#define STATUS_H  24

// Предел по числу пикселей (1920x1200). Буферы статические: кучи у программы нет,
// а образ .prg всё равно отображается целиком при запуске.
#define PX_MAX (1920 * 1200)

// Иконка изображения 16x16
static const uint16_t icon_image[16] = {
    0b0000000000000000, 0b0111111111111110, 0b0100000000000010, 0b0101100000000010,
    0b0101100000100010, 0b0100000001110010, 0b0100000011111010, 0b0100001111111010,
    0b0100111111111010, 0b0101111111111010, 0b0111111111111010, 0b0100000000000010,
    0b0111111111111110, 0b0000000000000000, 0b0000000000000000, 0b0000000000000000
};

static uint8_t  s_file_data[1024 * 1024 * 8];   // сам файл
static uint32_t s_argb[PX_MAX];                 // распакованное изображение
static uint8_t  s_scratch[PX_MAX * 5];          // рабочий буфер декодера PNG
static char     s_filepath[128] = "/sys/logo.bmp";

static int  s_img_w = 0, s_img_h = 0;
static int  s_img_loaded = 0;
static int  s_has_alpha = 0;
static int  s_err = 0;                          // код ошибки загрузки
static char s_fmt[40] = "";                     // строка формата для статусной строки

static void fmt_set(const char* a, const char* b, int bits) {
    int p = 0;
    for (const char* s = a; *s && p < 30; s++) s_fmt[p++] = *s;
    for (const char* s = b; *s && p < 36; s++) s_fmt[p++] = *s;
    if (bits > 0) {
        char num[8];
        ui_itoa(bits, num);
        s_fmt[p++] = ' ';
        for (int i = 0; num[i] && p < 38; i++) s_fmt[p++] = num[i];
        const char* tail = " бит";
        for (int i = 0; tail[i] && p < 39; i++) s_fmt[p++] = tail[i];
    }
    s_fmt[p] = '\0';
}


// ============================================================================
// Загрузка: формат определяем по подписи файла, а не по расширению
// ============================================================================
#define LOAD_ERR_OPEN    -30
#define LOAD_ERR_SHORT   -31
#define LOAD_ERR_UNKNOWN -32

static int load_image(const char* path) {
    s_img_loaded = 0;
    s_has_alpha = 0;
    s_err = 0;
    s_fmt[0] = '\0';

    int fd = ov_open(path, OV_O_RDONLY);
    if (fd < 0) { s_err = LOAD_ERR_OPEN; return LOAD_ERR_OPEN; }
    int bytes = (int)ov_read(fd, s_file_data, sizeof(s_file_data));
    ov_close(fd);
    if (bytes < 16) { s_err = LOAD_ERR_SHORT; return LOAD_ERR_SHORT; }

    // PNG
    if (s_file_data[0] == 0x89 && s_file_data[1] == 'P' &&
        s_file_data[2] == 'N'  && s_file_data[3] == 'G') {
        png_info_t info;
        int r = png_decode(s_file_data, (uint32_t)bytes, s_argb, PX_MAX,
                           s_scratch, sizeof(s_scratch), &info);
        if (r != PNG_OK) { s_err = r; return r; }
        s_img_w = info.w;
        s_img_h = info.h;
        s_has_alpha = info.has_alpha;
        static const char* kinds[7] = { " серый", "", " RGB", " палитра", " серый+альфа", "", " RGBA" };
        fmt_set("PNG", (info.color_type <= 6) ? kinds[info.color_type] : "", info.bit_depth);
        s_img_loaded = 1;
        return 0;
    }

    // BMP
    if (s_file_data[0] == 'B' && s_file_data[1] == 'M') {
        bmp_info_t bi;
        int r = bmp_decode(s_file_data, (uint32_t)bytes, s_argb, PX_MAX, &bi);
        if (r != BMP_OK) { s_err = r; return r; }
        s_img_w = bi.w;
        s_img_h = bi.h;
        s_has_alpha = 0;
        fmt_set(bi.rle ? "BMP RLE8" : "BMP", "", bi.bit_depth);
        s_img_loaded = 1;
        return 0;
    }

    s_err = LOAD_ERR_UNKNOWN;
    return LOAD_ERR_UNKNOWN;
}

static const char* err_text(void) {
    switch (s_err) {
        case LOAD_ERR_OPEN:      return "Файл не открывается";
        case LOAD_ERR_SHORT:     return "Файл слишком мал или пуст";
        case LOAD_ERR_UNKNOWN:   return "Неизвестный формат: нужен BMP или PNG";
        case PNG_ERR_INTERLACE:  return "PNG с чересстрочной развёрткой (Adam7) не поддержан";
        case PNG_ERR_FORMAT:     return "PNG: неподдержанный тип цвета или глубина";
        case PNG_ERR_DEFLATE:    return "PNG: ошибка распаковки данных";
        case PNG_ERR_OUTPUT:     return "Изображение больше 1920x1200";
        case PNG_ERR_SCRATCH:    return "Изображение не влезает в буфер декодера";
        case PNG_ERR_TRUNC:      return "Файл обрывается на середине";
        case BMP_ERR_SMALL:      return "BMP: файл короче заголовка";
        case BMP_ERR_COMPR:
        case BMP_ERR_DEPTH:      return "BMP: неподдержанное сжатие или глубина";
        case BMP_ERR_SIZE:       return "BMP: неверные размеры";
        case BMP_ERR_TOOBIG:     return "BMP больше 1920x1200";
        default:                 return "Не удалось открыть изображение";
    }
}

// ============================================================================
// Отрисовка: масштаб 16.16 и смешивание с шахматкой по альфа-каналу
// ============================================================================
static void draw_canvas(ui_win_t* w, int fit) {
    uint32_t* fb = ui_pixels(w);
    int cx0 = 0, cy0 = TOOLBAR_H + 1;
    int cw = g_ww, ch = g_wh - TOOLBAR_H - 1 - STATUS_H;

    int sq = 16;
    for (int y = cy0; y < cy0 + ch; y += sq) {
        for (int x = cx0; x < cx0 + cw; x += sq) {
            uint32_t col = (((x / sq) + (y / sq)) & 1) ? 0x202428 : 0x16181B;
            ui_fill(w, x, y, sq, (y + sq > cy0 + ch) ? cy0 + ch - y : sq, col);
        }
    }

    if (!s_img_loaded) return;

    uint32_t scale = 65536;                       // 1.0 в формате 16.16
    if (fit && (s_img_w > cw || s_img_h > ch)) {
        uint32_t sx = ((uint32_t)s_img_w * 65536) / cw;
        uint32_t sy = ((uint32_t)s_img_h * 65536) / ch;
        scale = (sx > sy) ? sx : sy;
    }

    int disp_w = (int)(((uint64_t)s_img_w * 65536) / scale);
    int disp_h = (int)(((uint64_t)s_img_h * 65536) / scale);
    if (disp_w < 1) disp_w = 1;
    if (disp_h < 1) disp_h = 1;

    int dst_x = cx0 + (cw - disp_w) / 2;
    int dst_y = cy0 + (ch - disp_h) / 2;

    for (int dy = 0; dy < disp_h; dy++) {
        int py = dst_y + dy;
        if (py < cy0 || py >= cy0 + ch) continue;

        int sy = (int)(((uint64_t)dy * scale) >> 16);
        if (sy >= s_img_h) sy = s_img_h - 1;

        const uint32_t* src_row = &s_argb[(uint32_t)sy * s_img_w];
        uint32_t* fb_row = &fb[py * g_ww];

        for (int dx = 0; dx < disp_w; dx++) {
            int px = dst_x + dx;
            if (px < cx0 || px >= cx0 + cw) continue;

            int sx = (int)(((uint64_t)dx * scale) >> 16);
            if (sx >= s_img_w) sx = s_img_w - 1;

            uint32_t c = src_row[sx];
            if (!s_has_alpha) { fb_row[px] = 0xFF000000 | (c & 0x00FFFFFF); continue; }

            uint32_t a = c >> 24;
            if (a == 255) { fb_row[px] = 0xFF000000 | (c & 0x00FFFFFF); continue; }
            if (a == 0) continue;                 // полностью прозрачно: оставляем шахматку

            uint32_t bg = fb_row[px];
            uint32_t r = (((c >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * (255 - a)) / 255;
            uint32_t g = (((c >> 8)  & 0xFF) * a + ((bg >> 8)  & 0xFF) * (255 - a)) / 255;
            uint32_t b = ((c & 0xFF)         * a + (bg & 0xFF)         * (255 - a)) / 255;
            fb_row[px] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    ui_frame(w, dst_x - 1, dst_y - 1, disp_w + 2, disp_h + 2, UI_BORDER);
}

int main(void) {
    if (libgfx_bind() < 0 || libui_bind() < 0) ov_exit(1);   // разборщики и тулкит из /lib
    char arg[128];
    if (ov_get_arg(arg, sizeof(arg)) > 0 && arg[0]) ov_strlcpy(s_filepath, arg, sizeof(s_filepath));
    load_image(s_filepath);

    ui_app_id("ovimage");       // следующая картинка откроется в этом же окне

    ui_win_t* w = ui_open("overImages — Просмотр изображений", WIN_W, WIN_H, icon_image, WP_FLAG_RESIZABLE);
    if (!w) ov_exit(1);

    int fit = 1;
    while (ui_begin(w, UI_WAIT)) {
        g_ww = ui_width(w);                            // окно могли развернуть
        g_wh = ui_height(w);

        const char* req = ui_open_request(w);          // файл от overFiles
        if (req[0]) {
            ov_strlcpy(s_filepath, req, sizeof(s_filepath));
            load_image(s_filepath);
            fit = 1;
        }
        if (ui_key_pressed(w, WP_KEY_ESC)) break;

        draw_canvas(w, fit);

        ui_fill(w, 0, 0, g_ww, TOOLBAR_H, UI_PANEL);
        ui_hline(w, 0, TOOLBAR_H, g_ww, UI_BORDER);
        ui_icon16(w, 12, 12, icon_image, UI_ACCENT);
        ui_label(w, 38, 12, s_filepath);
        if (ui_button(w, g_ww - 150, 7, 140, 26, fit ? "Масштаб 100%" : "По размеру окна")) fit = !fit;

        int sy = g_wh - STATUS_H;
        ui_fill(w, 0, sy, g_ww, STATUS_H, UI_PANEL);
        ui_hline(w, 0, sy, g_ww, UI_BORDER);
        if (s_img_loaded) {
            char stat[80], num[12];
            int p = 0;
            ui_itoa(s_img_w, num); for (int i = 0; num[i]; i++) stat[p++] = num[i];
            stat[p++] = 'x';
            ui_itoa(s_img_h, num); for (int i = 0; num[i]; i++) stat[p++] = num[i];
            stat[p++] = ','; stat[p++] = ' ';
            for (int i = 0; s_fmt[i]; i++) stat[p++] = s_fmt[i];
            if (s_has_alpha) { const char* t = " + альфа"; for (int i = 0; t[i]; i++) stat[p++] = t[i]; }
            stat[p] = '\0';
            ui_label(w, 12, sy + 4, stat);
        } else {
            ui_text(w, 12, sy + 4, err_text(), UI_DANGER);
        }

        ui_end(w);
    }

    ui_close(w);
    ov_exit(0);
    return 0;
}
