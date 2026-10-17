#ifndef UIFONT_H
#define UIFONT_H
// uifont.h — шрифт графического интерфейса overOS.
//
// Шрифт настоящий, в формате .ttf: файл лежит в /sys/fonts/ui.ttf, разбирает и
// растеризует его src/lib/ttf.c прямо в системе (кэш глифов на размер). Никакой
// заготовки на этапе сборки больше нет — положите другой .ttf, и интерфейс
// будет нарисован им.
//
// Если файла нет или он битый, рисуем прежним растровым 8x16 (font_ru_8x16.h) —
// интерфейс остаётся читаемым, как в консоли.
//
// Здесь лежат только общие части: загрузка файла, выбор глифа, замена символов,
// которых нет в шрифте, и смешивание с фоном. Цикл по пикселям пишет каждый
// потребитель сам: у композитора это задний буфер экрана, у libui — буфер окна
// в разделяемой памяти.

#include <stdint.h>
#include "ovos.h"
#include "ttf.h"
#include "font_ru_8x16.h"

#define UIF_PATH      "/sys/fonts/ui.ttf"
#define UIF_FILE_MAX  (1536 * 1024)     // с запасом на обычные шрифты интерфейса
#define UIF_PX        14                // кегль интерфейса
#define UIF_PX_LG     30                // крупный (часы в окне входа)
#define UIF_MONO_W    8                 // ширина символа запасного растрового шрифта

static ttf_face_t uif_face;
static int        uif_ready = 0;
static uint8_t    uif_file[UIF_FILE_MAX];

// Загрузить шрифт. Вызывать один раз при старте; 1 — получилось.
static inline int uif_init(void) {
    if (uif_ready) return 1;
    int fd = ov_open(UIF_PATH, 0);
    if (fd < 0) return 0;
    int total = 0;
    while (total < (int)sizeof(uif_file)) {
        int n = ov_read(fd, (char*)uif_file + total, (int)sizeof(uif_file) - total);
        if (n <= 0) break;
        total += n;
    }
    ov_close(fd);
    if (total < 1024) return 0;
    uif_ready = (ttf_open(uif_file, (uint32_t)total, &uif_face) == 0);
    return uif_ready;
}

static inline int uif_ok(void) { return uif_ready; }

// Символы, которых в шрифте нет, подменяем похожими — строка не должна рассыпаться
static inline uint32_t uif_fallback(uint32_t cp) {
    if (cp == 0x00A0) return ' ';
    if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) return '-';
    if (cp == 0x00AB || cp == 0x00BB || cp == 0x201C || cp == 0x201D) return '"';
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x2022 || cp == 0x00B7) return '*';
    if (cp == 0x2026) return '.';
    return '?';
}

static inline const ttf_glyph_t* uif_glyph(uint32_t cp, int px) {
    if (!uif_ready) return 0;
    const ttf_glyph_t* g = ttf_glyph(&uif_face, cp, px);
    if (!g) g = ttf_glyph(&uif_face, uif_fallback(cp), px);
    return g;
}

static inline const uint8_t* uif_cov(const ttf_glyph_t* g) { return ttf_bitmap(g); }

// Базовая линия внутри строки и высота строки
static inline int uif_baseline(int px) { return uif_ready ? ttf_baseline(&uif_face, px) : 12; }
static inline int uif_line(int px)     { return uif_ready ? ttf_line_height(&uif_face, px) : 16; }

static inline uint32_t uif_utf8_next(const uint8_t** s) { return ttf_utf8_next(s); }

// Запасной растровый шрифт: UTF-8 -> индекс глифа CP866
static inline uint8_t uif_mono_index(uint32_t cp) {
    if (cp < 0x80) return (uint8_t)cp;
    if (cp >= 0x410 && cp <= 0x43F) return (uint8_t)(0x80 + (cp - 0x410));
    if (cp >= 0x440 && cp <= 0x44F) return (uint8_t)(0xE0 + (cp - 0x440));
    if (cp == 0x401) return 0x85;
    if (cp == 0x451) return 0xA5;
    if (cp == 0x2014 || cp == 0x2013) return '-';
    if (cp == 0x00A0) return ' ';
    return '?';
}

static inline const uint8_t* uif_mono_rows(uint32_t cp) {
    return &font_ru_8x16[(uint32_t)uif_mono_index(cp) * 16];
}

// Ширина строки в пикселях (шрифт переменной ширины — считать символы нельзя)
static inline int uif_width_px(const char* utf8, int px) {
    const uint8_t* s = (const uint8_t*)utf8;
    int wsum = 0;
    while (*s) {
        uint32_t cp = uif_utf8_next(&s);
        if (!uif_ready) { wsum += UIF_MONO_W; continue; }
        const ttf_glyph_t* g = uif_glyph(cp, px);
        if (g) wsum += g->adv;
    }
    return wsum;
}

static inline int uif_text_width(const char* utf8)  { return uif_width_px(utf8, UIF_PX); }
static inline int uifl_text_width(const char* utf8) { return uif_width_px(utf8, UIF_PX_LG); }

// Смешивание цвета с фоном по покрытию a (0..255)
static inline uint32_t uif_blend(uint32_t dst, uint32_t fg, uint32_t a) {
    if (a >= 255) return fg;
    uint32_t ia = 255 - a;
    uint32_t r = ((((fg >> 16) & 0xFF) * a) + (((dst >> 16) & 0xFF) * ia)) / 255;
    uint32_t g = ((((fg >> 8)  & 0xFF) * a) + (((dst >> 8)  & 0xFF) * ia)) / 255;
    uint32_t b = (((fg & 0xFF) * a) + ((dst & 0xFF) * ia)) / 255;
    return (r << 16) | (g << 8) | b;
}

#endif
