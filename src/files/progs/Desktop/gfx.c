#include "gfx.h"
#include "syscalls.h"
#include "icons.h"
#include "font_8x8.h"
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

void gfx_draw_char(int x, int y, uint8_t ch, uint32_t color) {
    const uint8_t* glyph = &font_ru_8x16[(uint32_t)ch * 16];
    for (int r = 0; r < 16; r++) {
        uint8_t row = glyph[r];
        for (int c = 0; c < 8; c++) {
            if (row & (1 << (7 - c))) {
                int px = x + c, py = y + r;
                if (px >= 0 && px < (int)g_screen_w && py >= 0 && py < (int)g_screen_h) {
                    g_backbuffer[py * g_screen_w + px] = color;
                }
            }
        }
    }
}

void gfx_draw_string(int x, int y, const char* str, uint32_t color) {
    const uint8_t* s = (const uint8_t*)str;
    while (*s) {
        if (*s == 0xD0 && *(s + 1)) {
            uint8_t next = *(s + 1);
            if (next >= 0x90 && next <= 0xBF) gfx_draw_char(x, y, (uint8_t)(next - 0x90 + 0x80), color);
            else if (next == 0x81) gfx_draw_char(x, y, 0xF0, color);
            s += 2; x += 8;
        } else if (*s == 0xD1 && *(s + 1)) {
            uint8_t next = *(s + 1);
            if (next >= 0x80 && next <= 0x8F) gfx_draw_char(x, y, (uint8_t)(next - 0x80 + 0xE0), color);
            else if (next == 0x91) gfx_draw_char(x, y, 0xF1, color);
            s += 2; x += 8;
        } else {
            gfx_draw_char(x, y, *s, color);
            s++; x += 8;
        }
    }
}

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

void gfx_draw_cursor(int mx, int my) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 11; x++) {
            if (cursor_mask[y] & (1 << (15 - x))) {
                int px = mx + x, py = my + y;
                if (px >= 0 && px < (int)g_screen_w && py >= 0 && py < (int)g_screen_h) {
                    uint32_t col = (x == 0 || y == 0 || (cursor_mask[y] & (1 << (14 - x))) == 0) ? 0x000000 : 0xFFFFFF;
                    g_backbuffer[py * g_screen_w + px] = col;
                }
            }
        }
    }
}

void gfx_present(void) {
    if (g_fb_fd >= 0) {
        sys_write(g_fb_fd, g_backbuffer, g_buf_bytes);
    }
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

#pragma pack(push, 1)
typedef struct {
    uint16_t type;             // 0x4D42 ('BM')
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t off_bits;
} bmp_hdr_t;

typedef struct {
    uint32_t size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bit_count;        // 8, 24 или 32
    uint32_t compression;     // 0 = BI_RGB, 1 = BI_RLE8, 3 = BI_BITFIELDS
    uint32_t size_image;
    int32_t  x_pels;
    int32_t  y_pels;
    uint32_t clr_used;
    uint32_t clr_important;
} bmp_info_t;
#pragma pack(pop)

void gfx_load_wallpaper(const char* filepath) {
    g_has_wallpaper = 0;
    if (!filepath || !filepath[0]) return;

    int fd = sys_open(filepath, 0);
    if (fd < 0) {
        show_error_dialog("Обои: файл не найден");
        return;
    }

    // Буфер под сжатый файл в памяти (1 МБ с огромным запасом под RLE8)
    #define MAX_WALLPAPER_FILE_SIZE (1024 * 1024)
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

    if (file_bytes < (int)(sizeof(bmp_hdr_t) + sizeof(bmp_info_t))) {
        show_error_dialog("Обои: файл слишком мал или повреждён");
        return;
    }

    bmp_hdr_t*  hdr  = (bmp_hdr_t*)s_file_data;
    bmp_info_t* info = (bmp_info_t*)(s_file_data + 14);

    if (hdr->type != 0x4D42) {
        show_error_dialog("Обои: неверная сигнатура BMP");
        return;
    }

    if (info->compression != 0 && info->compression != 1 && info->compression != 3) {
        show_error_dialog("Обои: неподдерживаемое сжатие");
        return;
    }

    int img_w = info->width;
    int img_h = info->height < 0 ? -info->height : info->height;
    int is_bottom_up = (info->height > 0);

    // Палитра (для 8-битного RLE8)
    uint32_t palette[256];
    if (info->bit_count == 8) {
        const uint8_t* pal_raw = s_file_data + 14 + info->size;
        for (int i = 0; i < 256; i++) {
            palette[i] = ((uint32_t)pal_raw[i * 4 + 2] << 16) |
                         ((uint32_t)pal_raw[i * 4 + 1] << 8) |
                         (uint32_t)pal_raw[i * 4 + 0];
        }
    }

    // Выделяем буфер под холст экрана
    if (!g_wallpaper_buf) {
        g_wallpaper_buf = (uint32_t*)sys_sbrk((int64_t)g_buf_bytes);
        if (g_wallpaper_buf == (void*)(uintptr_t)-1) {
            show_error_dialog("Обои: нет памяти под буфер экрана");
            g_wallpaper_buf = NULL;
            return;
        }
    }

    // Заливаем базовым цветом темы
    for (uint32_t i = 0; i < g_screen_w * g_screen_h; i++) {
        g_wallpaper_buf[i] = COLOR_DESKTOP_BG;
    }

    const uint8_t* p   = s_file_data + hdr->off_bits;
    const uint8_t* end = s_file_data + file_bytes;

    int max_h = (int)g_screen_h - TASKBAR_HEIGHT;

    // -------------------------------------------------------------------------
    // РЕЖИМ RLE8: Сначала распаковываем в сырой массив 8-бит, затем скейлим
    // -------------------------------------------------------------------------
    if (info->compression == 1 && info->bit_count == 8) {
        // Выделяем память под промежуточный буфер сырых индексов (2 МБ для 1080p)
        static uint8_t* s_raw_indexed = NULL;
        static uint32_t s_raw_cap = 0;
        uint32_t needed = (uint32_t)img_w * (uint32_t)img_h;

        if (s_raw_cap < needed) {
            s_raw_indexed = (uint8_t*)sys_sbrk((int64_t)needed);
            if (s_raw_indexed == (void*)(uintptr_t)-1) {
                show_error_dialog("Обои: не удалось выделить память под распаковку");
                s_raw_indexed = NULL;
                s_raw_cap = 0;
                return;
            }
            s_raw_cap = needed;
        }

        // 1. Распаковка RLE8 в линейный массив [img_w * img_h]
        int cur_x = 0;
        int cur_y = is_bottom_up ? (img_h - 1) : 0;

        while (p + 1 < end) {
            uint8_t count = *p++;
            uint8_t val   = *p++;

            if (count > 0) {
                for (int i = 0; i < count; i++) {
                    if (cur_x < img_w && cur_y >= 0 && cur_y < img_h) {
                        s_raw_indexed[cur_y * img_w + cur_x] = val;
                    }
                    cur_x++;
                }
            } else {
                if (val == 0) { // Конец строки
                    cur_x = 0;
                    cur_y += is_bottom_up ? -1 : 1;
                } else if (val == 1) { // Конец изображения
                    break;
                } else if (val == 2) { // Смещение
                    if (p + 1 < end) {
                        cur_x += *p++;
                        cur_y += is_bottom_up ? -(*p++) : (*p++);
                    }
                } else { // Абсолютный блок пикселей
                    int abs_cnt = val;
                    for (int i = 0; i < abs_cnt; i++) {
                        if (p >= end) break;
                        uint8_t idx = *p++;
                        if (cur_x < img_w && cur_y >= 0 && cur_y < img_h) {
                            s_raw_indexed[cur_y * img_w + cur_x] = idx;
                        }
                        cur_x++;
                    }
                    if (abs_cnt & 1) { if (p < end) p++; }
                }
            }
        }

        // 2. Быстрое масштабирование (DDA Fixed-point 16.16) под размер экрана
        uint32_t x_step = ((uint32_t)img_w << 16) / g_screen_w;
        uint32_t y_step = ((uint32_t)img_h << 16) / max_h;
        uint32_t curr_y = 0;

        for (int dy = 0; dy < max_h; dy++) {
            int sy = curr_y >> 16;
            if (sy >= img_h) sy = img_h - 1;
            uint32_t* dst_row = &g_wallpaper_buf[dy * g_screen_w];
            const uint8_t* src_row = &s_raw_indexed[sy * img_w];

            uint32_t curr_x = 0;
            for (int dx = 0; dx < (int)g_screen_w; dx++) {
                int sx = curr_x >> 16;
                if (sx >= img_w) sx = img_w - 1;
                dst_row[dx] = palette[src_row[sx]];
                curr_x += x_step;
            }
            curr_y += y_step;
        }
    }
    // -------------------------------------------------------------------------
    // РЕЖИМ НЕСЖАТОГО BMP (24 / 32 bpp): Прямое масштабирование из буфера файла
    // -------------------------------------------------------------------------
    else {
        int bpp_bytes = info->bit_count / 8;
        int stride = ((img_w * bpp_bytes + 3) / 4) * 4;

        uint32_t x_step = ((uint32_t)img_w << 16) / g_screen_w;
        uint32_t y_step = ((uint32_t)img_h << 16) / max_h;
        uint32_t curr_y = 0;

        for (int dy = 0; dy < max_h; dy++) {
            int sy = curr_y >> 16;
            if (sy >= img_h) sy = img_h - 1;
            int file_y = is_bottom_up ? (img_h - 1 - sy) : sy;
            const uint8_t* row = p + file_y * stride;
            uint32_t* dst_row = &g_wallpaper_buf[dy * g_screen_w];

            uint32_t curr_x = 0;
            for (int dx = 0; dx < (int)g_screen_w; dx++) {
                int sx = curr_x >> 16;
                if (sx >= img_w) sx = img_w - 1;

                if (bpp_bytes == 1) {
                    dst_row[dx] = palette[row[sx]];
                } else if (bpp_bytes >= 3) {
                    uint8_t b = row[sx * bpp_bytes + 0];
                    uint8_t g = row[sx * bpp_bytes + 1];
                    uint8_t r = row[sx * bpp_bytes + 2];
                    dst_row[dx] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                }
                curr_x += x_step;
            }
            curr_y += y_step;
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