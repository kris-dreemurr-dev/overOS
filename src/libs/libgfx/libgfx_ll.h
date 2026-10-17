#ifndef LIBGFX_LL_H
#define LIBGFX_LL_H
// libgfx_ll.h — подключение графики из /lib/libgfx.ll: PNG, BMP, WAV-разбор,
// шрифты TrueType и значки SVG.
//
// Программа включает нужные заголовки ради типов (png.h, bmp.h, ttf.h,
// svg.h), затем этот файл и зовёт libgfx_bind() в начале main. Вызовы остаются
// прежними: имена подменяются макросами на поля таблицы указателей.

#include "ovos.h"

#define LIBGFX_PATH "/lib/libgfx.ll"

typedef struct {
    int lib;
    // Типы приходят из png.h/bmp.h, которые включает сама программа,
    // поэтому структуры здесь объявлены как void*
    int (*png_decode)(const uint8_t* file, uint32_t len, uint32_t* argb, uint32_t max_px,
                      uint8_t* scratch, uint32_t scratch_len, void* info);
    int (*bmp_decode)(const uint8_t* file, uint32_t len, uint32_t* argb, uint32_t max_px, void* info);
    int (*svg_render)(const uint8_t* data, uint32_t len, uint32_t* out, int w, int h, uint32_t tint);
    int (*ttf_open)(const uint8_t* data, uint32_t len, void* face);
    const void* (*ttf_glyph)(void* face, uint32_t cp, int px);
    const uint8_t* (*ttf_bitmap)(const void* glyph);
    int (*ttf_text_width)(void* face, const char* utf8, int px);
} libgfx_api_t;

static libgfx_api_t g_gfx;

// 0 — библиотека загружена, -1 — нет файла, -2 — не нашлось символа
static inline int libgfx_bind(void) {
    for (unsigned i = 0; i < sizeof(g_gfx); i++) ((char*)&g_gfx)[i] = 0;
    int h = ov_lib_open(LIBGFX_PATH);
    if (h < 0) return -1;
    g_gfx.lib = h;
    OV_BIND(h, g_gfx.png_decode, "gfx_png_decode");
    OV_BIND(h, g_gfx.bmp_decode, "gfx_bmp_decode");
    OV_BIND(h, g_gfx.svg_render, "svg_render");
    OV_BIND(h, g_gfx.ttf_open, "ttf_open");
    OV_BIND(h, g_gfx.ttf_glyph, "ttf_glyph");
    OV_BIND(h, g_gfx.ttf_bitmap, "ttf_bitmap");
    OV_BIND(h, g_gfx.ttf_text_width, "ttf_text_width");
    const void* const* p = (const void* const*)&g_gfx;
    for (unsigned i = 1; i < sizeof(g_gfx) / sizeof(void*); i++) if (!p[i]) return -2;
    return 0;
}

// Имена вызовов остаются прежними
#define png_decode(f, l, a, m, s, sl, i) g_gfx.png_decode((f), (l), (a), (m), (s), (sl), (i))
#define bmp_decode(f, l, a, m, i)        g_gfx.bmp_decode((f), (l), (a), (m), (i))
#define svg_render(d, l, o, w, h, t)     g_gfx.svg_render((d), (l), (o), (w), (h), (t))

#endif
