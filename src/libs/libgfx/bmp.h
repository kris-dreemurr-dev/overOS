#ifndef BMP_H
#define BMP_H
// bmp.h — декодер BMP для программ overOS (Ring 3), без libc и без плавающей точки.
//
// Раньше этот разбор лежал прямо в overImages; теперь он стоит рядом с png.h,
// чтобы любая программа (просмотрщик, обои, установщик) читала картинки одним
// вызовом и получала готовый ARGB.
//
// Поддержано: 8 бит с палитрой (в том числе сжатие RLE8), 24 и 32 бита,
// строки снизу вверх и сверху вниз (отрицательная высота).
// Память декодер не выделяет: вызывающий даёт буфер под результат.

#include <stdint.h>

#define BMP_OK            0
#define BMP_ERR_SMALL   -20   // файл короче заголовка
#define BMP_ERR_COMPR   -21   // неподдержанное сжатие
#define BMP_ERR_DEPTH   -22   // неподдержанная глубина цвета
#define BMP_ERR_SIZE    -23   // нулевые или отрицательные размеры
#define BMP_ERR_TOOBIG  -24   // не влезает в буфер вызывающего

#pragma pack(push, 1)
typedef struct {
    uint16_t type;            // "BM"
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t off_bits;        // смещение пикселей от начала файла
} bmp_file_hdr_t;

typedef struct {
    uint32_t size;            // размер этого заголовка (за ним идёт палитра)
    int32_t  width;
    int32_t  height;          // < 0 — строки сверху вниз
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;     // 0 = BI_RGB, 1 = BI_RLE8, 3 = BI_BITFIELDS
    uint32_t size_image;
    int32_t  x_pels_per_meter;
    int32_t  y_pels_per_meter;
    uint32_t clr_used;
    uint32_t clr_important;
} bmp_info_hdr_t;
#pragma pack(pop)

typedef struct {
    int w, h;
    int bit_depth;            // 8, 24 или 32
    int rle;                  // 1 — файл был сжат RLE8
} bmp_info_t;

// Разбор файла в ARGB (альфа всегда 0xFF: прозрачности в этих вариантах BMP нет).
// argb должен вмещать max_px пикселей. Возврат: BMP_OK или BMP_ERR_*.
static inline int bmp_decode(const uint8_t* file, uint32_t len,
                             uint32_t* argb, uint32_t max_px, bmp_info_t* info) {
    if (!file || !argb || len < 54) return BMP_ERR_SMALL;

    const bmp_file_hdr_t* fh = (const bmp_file_hdr_t*)file;
    const bmp_info_hdr_t* ih = (const bmp_info_hdr_t*)(file + 14);

    if (ih->compression != 0 && ih->compression != 1 && ih->compression != 3) return BMP_ERR_COMPR;
    if (ih->bit_count != 8 && ih->bit_count != 24 && ih->bit_count != 32) return BMP_ERR_DEPTH;

    int w = ih->width;
    int top_down = ih->height < 0;
    int h = top_down ? -ih->height : ih->height;
    if (w <= 0 || h <= 0) return BMP_ERR_SIZE;
    if ((uint32_t)w * (uint32_t)h > max_px) return BMP_ERR_TOOBIG;

    int bpp = ih->bit_count;
    int indexed = (bpp == 8);
    uint32_t palette[256];
    if (indexed) {
        const uint8_t* pal_raw = file + 14 + ih->size;
        if (pal_raw + 256 * 4 > file + len) return BMP_ERR_SMALL;
        for (int i = 0; i < 256; i++) {
            palette[i] = ((uint32_t)pal_raw[i * 4 + 2] << 16) |
                         ((uint32_t)pal_raw[i * 4 + 1] << 8)  |
                         ((uint32_t)pal_raw[i * 4 + 0]);
        }
    }

    for (uint32_t i = 0; i < (uint32_t)w * (uint32_t)h; i++) argb[i] = 0xFF000000;

    if (info) { info->w = w; info->h = h; info->bit_depth = bpp; info->rle = 0; }

    // Сжатие RLE8: распаковываем прямо в argb через палитру
    if (ih->compression == 1 && indexed) {
        const uint8_t* p   = file + fh->off_bits;
        const uint8_t* end = file + len;
        int cur_x = 0;
        int bottom_up = (ih->height > 0);
        int cur_y = bottom_up ? (h - 1) : 0;

        while (p + 1 < end) {
            uint8_t count = *p++;
            uint8_t val   = *p++;
            if (count > 0) {
                for (int i = 0; i < count; i++) {
                    if (cur_x < w && cur_y >= 0 && cur_y < h)
                        argb[cur_y * w + cur_x] = 0xFF000000 | palette[val];
                    cur_x++;
                }
            } else if (val == 0) {            // конец строки
                cur_x = 0;
                cur_y += bottom_up ? -1 : 1;
            } else if (val == 1) {            // конец изображения
                break;
            } else if (val == 2) {            // смещение курсора
                if (p + 1 < end) {
                    int dx = *p++;
                    int dy = *p++;
                    cur_x += dx;
                    cur_y += bottom_up ? -dy : dy;
                }
            } else {                          // абсолютный блок индексов
                int cnt = val;
                for (int i = 0; i < cnt; i++) {
                    if (p >= end) break;
                    uint8_t idx = *p++;
                    if (cur_x < w && cur_y >= 0 && cur_y < h)
                        argb[cur_y * w + cur_x] = 0xFF000000 | palette[idx];
                    cur_x++;
                }
                if (cnt & 1) { if (p < end) p++; }   // выравнивание по слову
            }
        }
        if (info) info->rle = 1;
        return BMP_OK;
    }

    // Без сжатия: 8 бит с палитрой, 24 и 32 бита
    int bytes_per_pixel = bpp / 8;
    int row_stride = ((w * bytes_per_pixel + 3) & ~3);
    const uint8_t* pixels = file + fh->off_bits;

    for (int y = 0; y < h; y++) {
        int src_y = top_down ? y : (h - 1 - y);
        const uint8_t* row = pixels + (uint32_t)src_y * row_stride;
        if (row + (uint32_t)w * bytes_per_pixel > file + len) break;
        uint32_t* dst = &argb[(uint32_t)y * w];
        for (int x = 0; x < w; x++) {
            if (indexed) {
                dst[x] = 0xFF000000 | palette[row[x]];
            } else if (bytes_per_pixel == 3) {
                dst[x] = 0xFF000000 | ((uint32_t)row[x * 3 + 2] << 16) |
                         ((uint32_t)row[x * 3 + 1] << 8) | row[x * 3 + 0];
            } else {
                dst[x] = 0xFF000000 | (*(const uint32_t*)(row + x * 4) & 0x00FFFFFF);
            }
        }
    }
    return BMP_OK;
}

#endif
