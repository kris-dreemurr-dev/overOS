#ifndef PNG_H
#define PNG_H
// png.h — декодер PNG для программ overOS (Ring 3), без libc и без плавающей точки.
//
// Реализовано целиком своё: распаковка DEFLATE (RFC 1951) с фиксированными и
// динамическими таблицами Хаффмана, разбор контейнера PNG (RFC 2083), снятие
// фильтров строк и перевод любого поддержанного формата в готовый ARGB.
//
// Почему только целая арифметика: декодер писался до того, как switch.asm стал
// сохранять состояние SSE/FPU (fxsave/fxrstor), и менять его смысла нет —
// в целых числах он точнее и быстрее.
//
// Поддержано: типы цвета 0 (серый), 2 (RGB), 3 (палитра), 4 (серый+альфа),
// 6 (RGBA); глубина 1, 2, 4, 8 и 16 бит (у 16 берётся старший байт);
// прозрачность палитры через tRNS; IDAT, разбитый на несколько чанков.
// Не поддержано: чересстрочный Adam7 (PNG_ERR_INTERLACE) и цветовой ключ
// tRNS для типов 0 и 2 — такие файлы рисуются без прозрачности.
//
// Память декодер не выделяет: вызывающий даёт буфер под результат (ARGB) и
// рабочий буфер под распакованные строки (>= h * (1 + ширина_строки_в_байтах)).

#include <stdint.h>

#define PNG_OK              0
#define PNG_ERR_SIG        -1   // не PNG
#define PNG_ERR_TRUNC      -2   // файл обрывается
#define PNG_ERR_IHDR       -3   // нет или битый IHDR
#define PNG_ERR_FORMAT     -4   // неподдержанный тип цвета или глубина
#define PNG_ERR_INTERLACE  -5   // чересстрочный Adam7
#define PNG_ERR_NO_IDAT    -6   // нет сжатых данных
#define PNG_ERR_SCRATCH    -7   // мал рабочий буфер
#define PNG_ERR_OUTPUT     -8   // мал буфер результата
#define PNG_ERR_DEFLATE    -9   // ошибка распаковки
#define PNG_ERR_FILTER    -10   // неизвестный фильтр строки

typedef struct {
    int w, h;
    int bit_depth;
    int color_type;
    int has_alpha;      // в файле есть прозрачность (альфа-канал или tRNS палитры)
} png_info_t;

// ============================================================================
// Чтение сжатого потока: байты лежат в одном или нескольких чанках IDAT
// ============================================================================
typedef struct {
    const uint8_t* file;
    uint32_t len;
    uint32_t pos;        // позиция внутри текущего IDAT
    uint32_t end;        // конец данных текущего IDAT
    int      done;       // IDAT закончились
} png_idat_t;

static inline uint32_t png_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline int png_is(const uint8_t* p, const char* tag) {
    return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1] &&
           p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3];
}

// Перейти к следующему чанку IDAT начиная со смещения from (смещение заголовка чанка)
static inline void png_idat_seek(png_idat_t* s, uint32_t from) {
    uint32_t off = from;
    while (off + 8 <= s->len) {
        uint32_t clen = png_be32(s->file + off);
        const uint8_t* type = s->file + off + 4;
        if (off + 12 + clen > s->len) break;            // чанк не влезает в файл
        if (png_is(type, "IDAT")) {
            s->pos = off + 8;
            s->end = off + 8 + clen;
            if (clen) return;                            // нашли непустой IDAT
            off = s->end + 4;                            // пустой IDAT — ищем дальше
            continue;
        }
        if (png_is(type, "IEND")) break;
        off += 12 + clen;
    }
    s->done = 1;
    s->pos = s->end = 0;
}

// ============================================================================
// Распаковка DEFLATE
// ============================================================================
typedef struct {
    uint16_t counts[16];     // сколько кодов каждой длины
    uint16_t symbols[288];   // символы, отсортированные по длине кода
} png_huff_t;

typedef struct {
    png_idat_t in;
    uint32_t   bitbuf;
    int        bitcnt;
    uint8_t*   dst;
    uint32_t   dst_cap;
    uint32_t   dst_pos;
    int        err;
} png_inf_t;

static const uint16_t png_len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t png_len_ext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t png_dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t png_dist_ext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
// Порядок длин кодов в заголовке динамического блока (RFC 1951, 3.2.7)
static const uint8_t png_clc_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

// Таблицы держим в BSS, а не на стеке: кадр функции остаётся маленьким
static png_huff_t png_lt, png_dt, png_clt;
static uint8_t    png_lengths[288 + 32];

static inline int png_byte(png_inf_t* d) {
    while (d->in.pos >= d->in.end) {
        if (d->in.done) { d->err = 1; return 0; }
        png_idat_seek(&d->in, d->in.end ? d->in.end + 4 : 8);
        if (d->in.done) { d->err = 1; return 0; }
    }
    return d->in.file[d->in.pos++];
}

static inline int png_bit(png_inf_t* d) {
    if (!d->bitcnt) {
        d->bitbuf = (uint32_t)png_byte(d);
        d->bitcnt = 8;
    }
    int b = (int)(d->bitbuf & 1);
    d->bitbuf >>= 1;
    d->bitcnt--;
    return b;
}

static inline uint32_t png_bits(png_inf_t* d, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)png_bit(d) << i;
    return v;
}

static inline void png_huff_build(png_huff_t* t, const uint8_t* lengths, int num) {
    uint16_t offs[16];
    for (int i = 0; i < 16; i++) t->counts[i] = 0;
    for (int i = 0; i < num; i++) t->counts[lengths[i]]++;
    t->counts[0] = 0;
    uint16_t sum = 0;
    for (int i = 0; i < 16; i++) { offs[i] = sum; sum = (uint16_t)(sum + t->counts[i]); }
    for (int i = 0; i < num; i++) if (lengths[i]) t->symbols[offs[lengths[i]]++] = (uint16_t)i;
}

// Канонический Хаффман: идём от коротких кодов к длинным, пока код не попадёт в диапазон
static inline int png_huff_decode(png_inf_t* d, const png_huff_t* t) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= png_bit(d);
        int count = t->counts[len];
        if (code - first < count) return t->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
        if (d->err) return -1;
    }
    return -1;
}

static inline int png_block_stored(png_inf_t* d) {
    d->bitcnt = 0;                                   // выравнивание на границу байта
    int l1 = png_byte(d), l2 = png_byte(d);
    png_byte(d); png_byte(d);                        // NLEN не проверяем
    uint32_t n = (uint32_t)l1 | ((uint32_t)l2 << 8);
    if (d->dst_pos + n > d->dst_cap) return -1;
    for (uint32_t i = 0; i < n; i++) d->dst[d->dst_pos++] = (uint8_t)png_byte(d);
    return d->err ? -1 : 0;
}

static inline int png_block_huff(png_inf_t* d, const png_huff_t* lt, const png_huff_t* dt) {
    for (;;) {
        int sym = png_huff_decode(d, lt);
        if (sym < 0 || d->err) return -1;
        if (sym < 256) {
            if (d->dst_pos >= d->dst_cap) return -1;
            d->dst[d->dst_pos++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) return 0;                    // конец блока
        sym -= 257;
        if (sym >= 29) return -1;
        uint32_t len = png_len_base[sym] + png_bits(d, png_len_ext[sym]);
        int ds = png_huff_decode(d, dt);
        if (ds < 0 || ds >= 30) return -1;
        uint32_t dist = png_dist_base[ds] + png_bits(d, png_dist_ext[ds]);
        if (dist == 0 || dist > d->dst_pos) return -1;
        if (d->dst_pos + len > d->dst_cap) return -1;
        // Ссылка назад в уже распакованные байты: длина может перекрывать источник,
        // поэтому копируем строго по одному байту
        uint32_t from = d->dst_pos - dist;
        for (uint32_t i = 0; i < len; i++) d->dst[d->dst_pos++] = d->dst[from + i];
        if (d->err) return -1;
    }
}

static inline int png_block_dynamic(png_inf_t* d) {
    int hlit  = (int)png_bits(d, 5) + 257;
    int hdist = (int)png_bits(d, 5) + 1;
    int hclen = (int)png_bits(d, 4) + 4;
    if (hlit > 288 || hdist > 32) return -1;

    uint8_t clc[19];
    for (int i = 0; i < 19; i++) clc[i] = 0;
    for (int i = 0; i < hclen; i++) clc[png_clc_order[i]] = (uint8_t)png_bits(d, 3);
    png_huff_build(&png_clt, clc, 19);

    int n = 0;
    while (n < hlit + hdist) {
        int sym = png_huff_decode(d, &png_clt);
        if (sym < 0 || d->err) return -1;
        if (sym < 16) {
            png_lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {                      // повтор предыдущей длины
            if (n == 0) return -1;
            uint8_t prev = png_lengths[n - 1];
            int rep = 3 + (int)png_bits(d, 2);
            while (rep-- && n < hlit + hdist) png_lengths[n++] = prev;
        } else if (sym == 17) {                      // серия нулей, короткая
            int rep = 3 + (int)png_bits(d, 3);
            while (rep-- && n < hlit + hdist) png_lengths[n++] = 0;
        } else {                                     // серия нулей, длинная
            int rep = 11 + (int)png_bits(d, 7);
            while (rep-- && n < hlit + hdist) png_lengths[n++] = 0;
        }
    }

    png_huff_build(&png_lt, png_lengths, hlit);
    png_huff_build(&png_dt, png_lengths + hlit, hdist);
    return png_block_huff(d, &png_lt, &png_dt);
}

static inline int png_block_fixed(png_inf_t* d) {
    uint8_t ll[288], dl[30];
    for (int i = 0;   i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    for (int i = 0;   i < 30;  i++) dl[i] = 5;
    png_huff_build(&png_lt, ll, 288);
    png_huff_build(&png_dt, dl, 30);
    return png_block_huff(d, &png_lt, &png_dt);
}

static inline int png_inflate(png_inf_t* d) {
    int last;
    do {
        last = png_bit(d);
        int type = (int)png_bits(d, 2);
        int r;
        if (type == 0)      r = png_block_stored(d);
        else if (type == 1) r = png_block_fixed(d);
        else if (type == 2) r = png_block_dynamic(d);
        else                return -1;
        if (r < 0 || d->err) return -1;
    } while (!last);
    return 0;
}

// ============================================================================
// Фильтры строк (RFC 2083, 6) и разбор контейнера
// ============================================================================
static inline int png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

// Один сэмпл из строки с учётом глубины (у 16 бит берём старший байт)
static inline uint32_t png_sample(const uint8_t* row, int idx, int depth) {
    if (depth == 8)  return row[idx];
    if (depth == 16) return row[idx * 2];
    int per = 8 / depth;                             // сэмплов в байте: 8, 4 или 2
    uint32_t b = row[idx / per];
    int shift = (per - 1 - (idx % per)) * depth;
    return (b >> shift) & ((1u << depth) - 1u);
}

static inline uint32_t png_scale(uint32_t v, int depth) {
    if (depth >= 8) return v & 0xFF;
    uint32_t maxv = (1u << depth) - 1u;              // 1 бит -> 0/255, 4 бита -> 0..255
    return (v * 255u) / maxv;
}

static uint32_t png_pal[256];                        // палитра PLTE в виде 0x00RRGGBB
static uint8_t  png_pal_a[256];                      // прозрачность из tRNS

static inline int png_decode(const uint8_t* file, uint32_t len,
                             uint32_t* out, uint32_t out_cap,
                             uint8_t* scratch, uint32_t scratch_cap,
                             png_info_t* info) {
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if (!file || len < 8 || !out || !scratch || !info) return PNG_ERR_SIG;
    for (int i = 0; i < 8; i++) if (file[i] != sig[i]) return PNG_ERR_SIG;

    int w = 0, h = 0, depth = 0, ct = 0;
    int have_ihdr = 0, have_idat = 0, pal_n = 0, trns_n = 0;
    uint32_t first_idat = 0;

    for (int i = 0; i < 256; i++) { png_pal[i] = 0; png_pal_a[i] = 255; }

    // Обход чанков: собираем IHDR, PLTE, tRNS и находим первый IDAT
    uint32_t off = 8;
    while (off + 12 <= len) {
        uint32_t clen = png_be32(file + off);
        const uint8_t* type = file + off + 4;
        const uint8_t* data = file + off + 8;
        if (off + 12 + clen > len) return PNG_ERR_TRUNC;

        if (png_is(type, "IHDR")) {
            if (clen < 13) return PNG_ERR_IHDR;
            w     = (int)png_be32(data);
            h     = (int)png_be32(data + 4);
            depth = data[8];
            ct    = data[9];
            if (data[10] != 0 || data[11] != 0) return PNG_ERR_FORMAT;   // метод сжатия и фильтрации
            if (data[12] != 0) return PNG_ERR_INTERLACE;
            have_ihdr = 1;
        } else if (png_is(type, "PLTE")) {
            pal_n = (int)(clen / 3);
            if (pal_n > 256) pal_n = 256;
            for (int i = 0; i < pal_n; i++)
                png_pal[i] = ((uint32_t)data[i*3] << 16) | ((uint32_t)data[i*3+1] << 8) | data[i*3+2];
        } else if (png_is(type, "tRNS")) {
            trns_n = (int)clen;
            if (trns_n > 256) trns_n = 256;
            // Для палитры tRNS — это байт альфы на индекс; для типов 0 и 2 это
            // цветовой ключ, который мы не поддерживаем (рисуем без прозрачности)
            for (int i = 0; i < trns_n; i++) png_pal_a[i] = data[i];
        } else if (png_is(type, "IDAT")) {
            if (!have_idat) { first_idat = off; have_idat = 1; }
        } else if (png_is(type, "IEND")) {
            break;
        }
        off += 12 + clen;
    }

    if (!have_ihdr) return PNG_ERR_IHDR;
    if (!have_idat) return PNG_ERR_NO_IDAT;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return PNG_ERR_IHDR;

    int channels;
    switch (ct) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return PNG_ERR_FORMAT;
    }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return PNG_ERR_FORMAT;
    if (ct != 0 && ct != 3 && depth < 8) return PNG_ERR_FORMAT;          // по стандарту так и есть
    if (ct == 3 && depth == 16) return PNG_ERR_FORMAT;

    uint32_t bpl = ((uint32_t)w * (uint32_t)channels * (uint32_t)depth + 7) / 8;
    uint32_t stride = bpl + 1;                                           // + байт фильтра строки
    if (stride * (uint32_t)h > scratch_cap) return PNG_ERR_SCRATCH;
    if ((uint32_t)w * (uint32_t)h > out_cap) return PNG_ERR_OUTPUT;

    // Распаковка IDAT в рабочий буфер
    png_inf_t d;
    d.in.file = file; d.in.len = len; d.in.pos = 0; d.in.end = 0; d.in.done = 0;
    png_idat_seek(&d.in, first_idat);
    if (d.in.done) return PNG_ERR_NO_IDAT;
    d.bitbuf = 0; d.bitcnt = 0;
    d.dst = scratch; d.dst_cap = stride * (uint32_t)h; d.dst_pos = 0; d.err = 0;

    int cmf = png_byte(&d), flg = png_byte(&d);                          // заголовок zlib
    if (d.err || (cmf & 0x0F) != 8 || (flg & 0x20)) return PNG_ERR_DEFLATE;
    if (png_inflate(&d) < 0) return PNG_ERR_DEFLATE;
    if (d.dst_pos < stride * (uint32_t)h) return PNG_ERR_DEFLATE;        // строк меньше, чем заявлено

    // Снятие фильтров: каждая строка восстанавливается по предыдущей, на месте
    int fbpp = ((int)channels * depth + 7) / 8;
    if (fbpp < 1) fbpp = 1;
    for (int y = 0; y < h; y++) {
        uint8_t* row = scratch + (uint32_t)y * stride;
        int ft = row[0];
        uint8_t* cur = row + 1;
        const uint8_t* up = (y > 0) ? (scratch + (uint32_t)(y - 1) * stride + 1) : 0;
        switch (ft) {
            case 0: break;
            case 1:
                for (uint32_t x = (uint32_t)fbpp; x < bpl; x++) cur[x] = (uint8_t)(cur[x] + cur[x - fbpp]);
                break;
            case 2:
                if (up) for (uint32_t x = 0; x < bpl; x++) cur[x] = (uint8_t)(cur[x] + up[x]);
                break;
            case 3:
                for (uint32_t x = 0; x < bpl; x++) {
                    int a = (x >= (uint32_t)fbpp) ? cur[x - fbpp] : 0;
                    int b = up ? up[x] : 0;
                    cur[x] = (uint8_t)(cur[x] + ((a + b) >> 1));
                }
                break;
            case 4:
                for (uint32_t x = 0; x < bpl; x++) {
                    int a = (x >= (uint32_t)fbpp) ? cur[x - fbpp] : 0;
                    int b = up ? up[x] : 0;
                    int c = (up && x >= (uint32_t)fbpp) ? up[x - fbpp] : 0;
                    cur[x] = (uint8_t)(cur[x] + png_paeth(a, b, c));
                }
                break;
            default: return PNG_ERR_FILTER;
        }
    }

    // Перевод в ARGB
    for (int y = 0; y < h; y++) {
        const uint8_t* row = scratch + (uint32_t)y * stride + 1;
        uint32_t* dst = out + (uint32_t)y * (uint32_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t r, g, b, a = 255;
            switch (ct) {
                case 0: {
                    r = g = b = png_scale(png_sample(row, x, depth), depth);
                    break;
                }
                case 2: {
                    int i = x * 3;
                    r = png_sample(row, i + 0, depth);
                    g = png_sample(row, i + 1, depth);
                    b = png_sample(row, i + 2, depth);
                    break;
                }
                case 3: {
                    uint32_t idx = png_sample(row, x, depth);
                    if (idx > 255) idx = 255;
                    r = (png_pal[idx] >> 16) & 0xFF;
                    g = (png_pal[idx] >> 8) & 0xFF;
                    b = png_pal[idx] & 0xFF;
                    a = png_pal_a[idx];
                    break;
                }
                case 4: {
                    int i = x * 2;
                    r = g = b = png_scale(png_sample(row, i + 0, depth), depth);
                    a = png_sample(row, i + 1, depth) & 0xFF;
                    break;
                }
                default: {
                    int i = x * 4;
                    r = png_sample(row, i + 0, depth);
                    g = png_sample(row, i + 1, depth);
                    b = png_sample(row, i + 2, depth);
                    a = png_sample(row, i + 3, depth);
                    break;
                }
            }
            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    info->w = w;
    info->h = h;
    info->bit_depth = depth;
    info->color_type = ct;
    info->has_alpha = (ct == 4 || ct == 6 || (ct == 3 && trns_n > 0)) ? 1 : 0;
    return PNG_OK;
}

#endif // PNG_H
