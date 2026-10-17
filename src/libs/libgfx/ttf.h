#ifndef TTF_H
#define TTF_H
// ttf.h — разбор и растеризация шрифтов TrueType (.ttf) в overOS.
//
// Шрифт читается как есть, прямо из файла: таблицы cmap/glyf/loca/hmtx
// разбираются на месте, контуры глифов переводятся в кривые, кривые — в
// многоугольники, многоугольники закрашиваются по правилу ненулевого обхода со
// сглаживанием (супервыборка 4x4). Готовые карты покрытия складываются в кэш,
// поэтому каждый глиф считается один раз на размер.
//
// Никакого FreeType и libc: вся арифметика своя, память — статическая арена.
// Пользователь кладёт любой .ttf в /sys/fonts, и интерфейс рисуется им.

#include <stdint.h>

#define TTF_CACHE_SLOTS 512            // сколько разных глифов помним
#define TTF_CACHE_BYTES (512 * 1024)   // арена под карты покрытия

typedef struct {
    const uint8_t* data;
    uint32_t len;

    uint32_t glyf, loca, cmap, hmtx, hhea, head, maxp, os2;
    uint32_t glyf_len, loca_len;

    int upem;              // единиц на em (обычно 1000 или 2048)
    int loc_long;          // формат loca: 0 — короткий, 1 — длинный
    int num_glyphs;
    int num_hmetrics;
    int ascent, descent;   // в единицах шрифта
    int cap_height;

    uint32_t cmap_sub;     // выбранная подтаблица cmap
    int      cmap_fmt;     // 4 или 12
} ttf_face_t;

typedef struct {
    uint32_t key;          // (код << 8) | размер — чем помечен слот кэша
    int16_t  w, h;         // размер карты покрытия
    int16_t  bx, by;       // смещение слева от пера и верх карты над базовой линией
    int16_t  adv;          // шаг пера
    uint32_t off;          // смещение карты в арене
} ttf_glyph_t;

// Разобрать шрифт. data должен жить, пока шрифтом пользуются. 0 — порядок.
int ttf_open(const uint8_t* data, uint32_t len, ttf_face_t* f);

// Глиф нужного размера (в пикселях по em). 0 — такого символа нет.
const ttf_glyph_t* ttf_glyph(ttf_face_t* f, uint32_t cp, int px);

// Карта покрытия глифа (байт прозрачности на пиксель)
const uint8_t* ttf_bitmap(const ttf_glyph_t* g);

// Метрики строки и кегля
int ttf_text_width(ttf_face_t* f, const char* utf8, int px);
int ttf_ascent(const ttf_face_t* f, int px);     // от верха строки до базовой линии
int ttf_line_height(const ttf_face_t* f, int px);
int ttf_baseline(const ttf_face_t* f, int px);   // базовая линия для «прописные от y+2»

// Очередной символ строки UTF-8 (s сдвигается)
uint32_t ttf_utf8_next(const uint8_t** s);

#endif
