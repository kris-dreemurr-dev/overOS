// libgfx.ll — графика overOS одной библиотекой: картинки, шрифты, значки.
//
// Внутри: разбор PNG и BMP, растеризатор шрифтов TrueType (src/lib/ttf.c) и
// растеризатор значков SVG (src/lib/svg.c). Раньше каждый из них вкомпилировался
// в программу; теперь код один на систему, а программа загружает его в runtime.
//
// Разборщики PNG и BMP написаны заголовочными (static inline), поэтому здесь
// для них есть обёртки: экспортируются только обычные функции.
//
// Звука здесь нет: всё, что связано со звуком и видео (включая WAV), умеет
// /lib/ffmpeg.ll — настоящий FFmpeg.

#include <stdint.h>
#include "png.h"
#include "bmp.h"

int gfx_png_decode(const uint8_t* file, uint32_t len, uint32_t* argb, uint32_t max_px,
                   uint8_t* scratch, uint32_t scratch_len, png_info_t* info) {
    return png_decode(file, len, argb, max_px, scratch, scratch_len, info);
}

int gfx_bmp_decode(const uint8_t* file, uint32_t len, uint32_t* argb, uint32_t max_px,
                   bmp_info_t* info) {
    return bmp_decode(file, len, argb, max_px, info);
}

// Версия библиотеки: программа может проверить, что загрузила ожидаемую
const char gfx_version[] = "overOS libgfx 1.0";
