// ttf.c — шрифты TrueType в overOS: разбор таблиц, контуры глифов, растеризация.
//
// Порядок работы такой же, как у любого растеризатора шрифтов:
//   cmap  — код символа в номер глифа;
//   loca  — номер глифа в смещение внутри glyf;
//   glyf  — контуры: точки с признаком «на кривой», квадратичные кривые Безье;
//   hmtx  — шаг пера.
// Контуры масштабируются в пиксели, кривые разбиваются на отрезки, а затем
// многоугольники закрашиваются построчно по правилу ненулевого обхода (так же
// считает сам TrueType). Сглаживание — супервыборка 4x4: доля закрытых
// подпикселей и есть прозрачность.
//
// Всё держится на статических массивах: в системе нет libc, а библиотека должна
// одинаково работать и в композиторе, и внутри /lib/libui.ll.

#include "ttf.h"

#define SS        4                  // подвыборок по каждой оси
#define MAX_PTS   2048               // точек в контуре глифа (после разбиения кривых)
#define MAX_CONT  64
#define MAX_EDGES 4096
#define MAX_GLYPH_W 512
#define MAX_GLYPH_H 512

// --- чтение полей big-endian -------------------------------------------------
static uint32_t rd16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static int32_t  rds16(const uint8_t* p) { int32_t v = (int32_t)rd16(p); return v >= 0x8000 ? v - 0x10000 : v; }
static uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t tag_of(const char* s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8) | (uint8_t)s[3];
}

// --- кэш глифов --------------------------------------------------------------
static ttf_glyph_t g_cache[TTF_CACHE_SLOTS];
static uint8_t     g_arena[TTF_CACHE_BYTES];
static uint32_t    g_arena_used = 0;
static int         g_cache_used = 0;

const uint8_t* ttf_bitmap(const ttf_glyph_t* g) { return &g_arena[g->off]; }

uint32_t ttf_utf8_next(const uint8_t** s) {
    const uint8_t* p = *s;
    uint32_t c = *p++;
    if (c >= 0xF0)      { c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (c >= 0xE0) { c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (c >= 0xC0) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
    *s = p;
    return c;
}

// --- таблицы -----------------------------------------------------------------
static uint32_t find_table(const uint8_t* d, uint32_t len, uint32_t tag, uint32_t* out_len) {
    if (len < 12) return 0;
    uint32_t ver = rd32(d);
    uint32_t base = 0;
    if (ver == 0x74746366) {                 // 'ttcf' — коллекция, берём первый шрифт
        if (len < 16) return 0;
        base = rd32(d + 12);
        if (base + 12 > len) return 0;
    }
    uint32_t n = rd16(d + base + 4);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* rec = d + base + 12 + i * 16;
        if ((uint32_t)(rec + 16 - d) > len) return 0;
        if (rd32(rec) == tag) {
            uint32_t off = rd32(rec + 8), tlen = rd32(rec + 12);
            if (off >= len) return 0;
            if (out_len) *out_len = (off + tlen <= len) ? tlen : (len - off);
            return off;
        }
    }
    return 0;
}

// Подтаблица cmap: нужен Unicode. Формат 4 (BMP) или 12 (полный диапазон).
static void pick_cmap(ttf_face_t* f) {
    const uint8_t* c = f->data + f->cmap;
    uint32_t n = rd16(c + 2);
    uint32_t best = 0;
    int best_fmt = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* rec = c + 4 + i * 8;
        uint32_t plat = rd16(rec), enc = rd16(rec + 2), off = rd32(rec + 4);
        uint32_t sub = f->cmap + off;
        if (sub + 4 > f->len) continue;
        uint32_t fmt = rd16(f->data + sub);
        int unicode = (plat == 0) || (plat == 3 && (enc == 1 || enc == 10));
        if (!unicode) continue;
        if (fmt == 12) { best = sub; best_fmt = 12; break; }       // полный Unicode — лучше
        if (fmt == 4 && best_fmt != 12) { best = sub; best_fmt = 4; }
    }
    f->cmap_sub = best;
    f->cmap_fmt = best_fmt;
}

int ttf_open(const uint8_t* data, uint32_t len, ttf_face_t* f) {
    if (!data || len < 1024 || !f) return -1;
    for (uint32_t i = 0; i < sizeof(*f); i++) ((uint8_t*)f)[i] = 0;
    f->data = data;
    f->len = len;

    uint32_t ver = rd32(data);
    if (ver != 0x00010000 && ver != 0x74727565 && ver != 0x74746366) return -2;   // не TrueType

    f->head = find_table(data, len, tag_of("head"), 0);
    f->maxp = find_table(data, len, tag_of("maxp"), 0);
    f->hhea = find_table(data, len, tag_of("hhea"), 0);
    f->hmtx = find_table(data, len, tag_of("hmtx"), 0);
    f->loca = find_table(data, len, tag_of("loca"), &f->loca_len);
    f->glyf = find_table(data, len, tag_of("glyf"), &f->glyf_len);
    f->cmap = find_table(data, len, tag_of("cmap"), 0);
    f->os2  = find_table(data, len, tag_of("OS/2"), 0);
    if (!f->head || !f->maxp || !f->hhea || !f->hmtx || !f->loca || !f->glyf || !f->cmap) return -3;

    f->upem = (int)rd16(data + f->head + 18);
    if (f->upem <= 0) return -4;
    f->loc_long = (int)rds16(data + f->head + 50);
    f->num_glyphs = (int)rd16(data + f->maxp + 4);
    f->ascent = rds16(data + f->hhea + 4);
    f->descent = -rds16(data + f->hhea + 6);
    f->num_hmetrics = (int)rd16(data + f->hhea + 34);
    f->cap_height = 0;
    if (f->os2 && rd16(data + f->os2) >= 2) f->cap_height = rds16(data + f->os2 + 88);
    if (f->cap_height <= 0) f->cap_height = (f->upem * 7) / 10;

    pick_cmap(f);
    if (!f->cmap_sub) return -5;

    // Кэш глифов общий на процесс: при смене шрифта его надо сбросить
    g_arena_used = 0;
    g_cache_used = 0;
    for (int i = 0; i < TTF_CACHE_SLOTS; i++) g_cache[i].key = 0;
    return 0;
}

static int glyph_index(const ttf_face_t* f, uint32_t cp) {
    const uint8_t* t = f->data + f->cmap_sub;
    if (f->cmap_fmt == 4) {
        if (cp > 0xFFFF) return 0;
        uint32_t segX2 = rd16(t + 6);
        const uint8_t* ends = t + 14;
        const uint8_t* starts = ends + segX2 + 2;
        const uint8_t* deltas = starts + segX2;
        const uint8_t* ranges = deltas + segX2;
        for (uint32_t i = 0; i < segX2; i += 2) {
            if (cp <= rd16(ends + i)) {
                uint32_t start = rd16(starts + i);
                if (cp < start) return 0;
                uint32_t ro = rd16(ranges + i);
                if (!ro) return (int)((cp + rd16(deltas + i)) & 0xFFFF);
                const uint8_t* p = ranges + i + ro + (cp - start) * 2;
                if ((uint32_t)(p - f->data) + 2 > f->len) return 0;
                uint32_t g = rd16(p);
                return g ? (int)((g + rd16(deltas + i)) & 0xFFFF) : 0;
            }
        }
        return 0;
    }
    if (f->cmap_fmt == 12) {
        uint32_t n = rd32(t + 12);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* g = t + 16 + i * 12;
            uint32_t s = rd32(g), e = rd32(g + 4);
            if (cp < s) return 0;
            if (cp <= e) return (int)(rd32(g + 8) + (cp - s));
        }
    }
    return 0;
}

static uint32_t glyph_offset(const ttf_face_t* f, int gi, uint32_t* out_len) {
    if (gi < 0 || gi >= f->num_glyphs) return 0;
    uint32_t a, b;
    if (f->loc_long) {
        a = rd32(f->data + f->loca + gi * 4);
        b = rd32(f->data + f->loca + gi * 4 + 4);
    } else {
        a = rd16(f->data + f->loca + gi * 2) * 2;
        b = rd16(f->data + f->loca + gi * 2 + 2) * 2;
    }
    if (b <= a) return 0;                     // пустой глиф (пробел)
    *out_len = b - a;
    return f->glyf + a;
}

static int glyph_advance(const ttf_face_t* f, int gi) {
    if (gi >= f->num_hmetrics) gi = f->num_hmetrics - 1;
    if (gi < 0) return 0;
    return (int)rd16(f->data + f->hmtx + gi * 4);
}

// --- контуры -----------------------------------------------------------------
typedef struct {
    float x, y;
    uint8_t on;      // точка лежит на кривой
    uint8_t last;    // последняя точка контура
} ttf_pt_t;

static ttf_pt_t s_pts[MAX_PTS];
static int      s_npts;

// Разбор простого глифа: точки добавляются в s_pts с учётом преобразования
static int load_simple(const ttf_face_t* f, uint32_t off, uint32_t len,
                       float a, float b, float c, float d, float e, float fdx) {
    if (len < 10) return 0;
    const uint8_t* g = f->data + off;
    int nc = rds16(g);
    if (nc <= 0 || nc > MAX_CONT) return 0;
    const uint8_t* ends = g + 10;
    int npt = (int)rd16(ends + (nc - 1) * 2) + 1;
    if (npt <= 0 || npt > MAX_PTS - s_npts) return 0;
    uint32_t ins = rd16(ends + nc * 2);
    const uint8_t* p = ends + nc * 2 + 2 + ins;

    // Флаги (с повторами)
    static uint8_t flags[MAX_PTS];
    for (int i = 0; i < npt; ) {
        if ((uint32_t)(p - f->data) >= f->len) return 0;
        uint8_t fl = *p++;
        flags[i++] = fl;
        if (fl & 8) {
            int rep = *p++;
            while (rep-- > 0 && i < npt) flags[i++] = fl;
        }
    }
    // Координаты: дельты, размер зависит от флагов
    static int32_t xs[MAX_PTS], ys[MAX_PTS];
    int32_t v = 0;
    for (int i = 0; i < npt; i++) {
        uint8_t fl = flags[i];
        if (fl & 2) { int32_t dx = *p++; v += (fl & 16) ? dx : -dx; }
        else if (!(fl & 16)) { v += rds16(p); p += 2; }
        xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < npt; i++) {
        uint8_t fl = flags[i];
        if (fl & 4) { int32_t dy = *p++; v += (fl & 32) ? dy : -dy; }
        else if (!(fl & 32)) { v += rds16(p); p += 2; }
        ys[i] = v;
    }

    int start = 0;
    for (int ci = 0; ci < nc; ci++) {
        int end = (int)rd16(ends + ci * 2);
        if (end < start || end >= npt) break;
        for (int i = start; i <= end; i++) {
            float x = (float)xs[i], y = (float)ys[i];
            ttf_pt_t* t = &s_pts[s_npts++];
            t->x = a * x + c * y + e;
            t->y = b * x + d * y + fdx;
            t->on = (flags[i] & 1) ? 1 : 0;
            t->last = (i == end);
        }
        start = end + 1;
    }
    return 1;
}

// Составной глиф: компоненты — ссылки на другие глифы со своим смещением.
// Кириллическая «А» во многих шрифтах — это латинская «A», поэтому без разбора
// составных глифов половина алфавита оказалась бы пустой.
static int load_glyph_outline(const ttf_face_t* f, int gi,
                              float a, float b, float c, float d, float e, float fdy, int depth) {
    uint32_t len = 0;
    uint32_t off = glyph_offset(f, gi, &len);
    if (!off || len < 10) return 0;
    int nc = rds16(f->data + off);
    if (nc >= 0) return load_simple(f, off, len, a, b, c, d, e, fdy);
    if (depth > 4) return 0;

    const uint8_t* p = f->data + off + 10;
    while (1) {
        uint32_t flags = rd16(p);
        int idx = (int)rd16(p + 2);
        p += 4;
        float dx, dy;
        if (flags & 1) {                       // ARG_1_AND_2_ARE_WORDS
            dx = (float)rds16(p); dy = (float)rds16(p + 2); p += 4;
        } else {
            dx = (float)(int8_t)p[0]; dy = (float)(int8_t)p[1]; p += 2;
        }
        float sa = 1.0f, sb = 0.0f, sc = 0.0f, sd = 1.0f;
        if (flags & 8) {                       // WE_HAVE_A_SCALE
            sa = sd = (float)rds16(p) / 16384.0f; p += 2;
        } else if (flags & 0x40) {             // X_AND_Y_SCALE
            sa = (float)rds16(p) / 16384.0f; sd = (float)rds16(p + 2) / 16384.0f; p += 4;
        } else if (flags & 0x80) {             // TWO_BY_TWO
            sa = (float)rds16(p) / 16384.0f;     sb = (float)rds16(p + 2) / 16384.0f;
            sc = (float)rds16(p + 4) / 16384.0f; sd = (float)rds16(p + 6) / 16384.0f;
            p += 8;
        }
        if (!(flags & 2)) { dx = 0; dy = 0; }   // точки, а не смещение — не поддерживаем

        // Складываем преобразования: сначала компонента, затем общее
        float na = a * sa + c * sb;
        float nb = b * sa + d * sb;
        float nc2 = a * sc + c * sd;
        float nd = b * sc + d * sd;
        float ne = a * dx + c * dy + e;
        float nf = b * dx + d * dy + fdy;
        load_glyph_outline(f, idx, na, nb, nc2, nd, ne, nf, depth + 1);

        if (!(flags & 0x20)) break;             // MORE_COMPONENTS
    }
    return 1;
}

// --- растеризация ------------------------------------------------------------
typedef struct { float y0, y1, x0, slope; int dir; } ttf_edge_t;
static ttf_edge_t s_edges[MAX_EDGES];
static int        s_nedges;

static void add_edge(float x0, float y0, float x1, float y1) {
    if (y0 == y1 || s_nedges >= MAX_EDGES) return;
    int dir = 1;
    if (y1 < y0) { float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1;
    }
    ttf_edge_t* e = &s_edges[s_nedges++];
    e->y0 = y0; e->y1 = y1; e->x0 = x0;
    e->slope = (x1 - x0) / (y1 - y0);
    e->dir = dir;
}

// Квадратичная кривая Безье отрезками: шаг выбран по размеру глифа
static void quad_to(float x0, float y0, float cx, float cy, float x1, float y1) {
    const int n = 8;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        float nx = u * u * x0 + 2 * u * t * cx + t * t * x1;
        float ny = u * u * y0 + 2 * u * t * cy + t * t * y1;
        add_edge(px, py, nx, ny);
        px = nx; py = ny;
    }
}

// Контуры (s_pts) -> рёбра
static void build_edges(void) {
    s_nedges = 0;
    int start = 0;
    for (int i = 0; i < s_npts; i++) {
        if (!s_pts[i].last) continue;
        int end = i;
        int n = end - start + 1;
        if (n >= 2) {
            // Начальная точка на кривой: если её нет, берём середину
            float sx, sy;
            int first_on = -1;
            for (int k = 0; k < n; k++) if (s_pts[start + k].on) { first_on = k; break; }
            if (first_on < 0) {
                sx = (s_pts[start].x + s_pts[end].x) * 0.5f;
                sy = (s_pts[start].y + s_pts[end].y) * 0.5f;
                first_on = 0;
            } else {
                sx = s_pts[start + first_on].x;
                sy = s_pts[start + first_on].y;
            }
            float cx = sx, cy = sy;
            int have_ctrl = 0;
            float px = sx, py = sy;
            for (int k = 1; k <= n; k++) {
                const ttf_pt_t* pt = &s_pts[start + ((first_on + k) % n)];
                if (pt->on) {
                    if (have_ctrl) { quad_to(px, py, cx, cy, pt->x, pt->y); have_ctrl = 0; }
                    else add_edge(px, py, pt->x, pt->y);
                    px = pt->x; py = pt->y;
                } else {
                    if (have_ctrl) {
                        // Две управляющие подряд — между ними подразумевается точка на кривой
                        float mx = (cx + pt->x) * 0.5f, my = (cy + pt->y) * 0.5f;
                        quad_to(px, py, cx, cy, mx, my);
                        px = mx; py = my;
                    }
                    cx = pt->x; cy = pt->y;
                    have_ctrl = 1;
                }
            }
            if (have_ctrl) quad_to(px, py, cx, cy, sx, sy);
            else add_edge(px, py, sx, sy);
        }
        start = end + 1;
    }
}

// Пересечения строки подвыборки с рёбрами
typedef struct { float x; int dir; } ttf_hit_t;

static int fill_scanline(float y, ttf_hit_t* hits, int max) {
    int n = 0;
    for (int i = 0; i < s_nedges && n < max; i++) {
        const ttf_edge_t* e = &s_edges[i];
        if (y < e->y0 || y >= e->y1) continue;
        hits[n].x = e->x0 + (y - e->y0) * e->slope;
        hits[n].dir = e->dir;
        n++;
    }
    // Сортировка вставками: пересечений на строку мало
    for (int i = 1; i < n; i++) {
        ttf_hit_t t = hits[i];
        int j = i - 1;
        while (j >= 0 && hits[j].x > t.x) { hits[j + 1] = hits[j]; j--; }
        hits[j + 1] = t;
    }
    return n;
}

static const ttf_glyph_t* cache_find(uint32_t key) {
    for (int i = 0; i < g_cache_used; i++) if (g_cache[i].key == key) return &g_cache[i];
    return 0;
}

const ttf_glyph_t* ttf_glyph(ttf_face_t* f, uint32_t cp, int px) {
    if (!f || !f->data || px <= 0 || px > 255) return 0;
    uint32_t key = (cp << 8) | (uint32_t)px;
    const ttf_glyph_t* hit = cache_find(key);
    if (hit) return hit;
    if (g_cache_used >= TTF_CACHE_SLOTS) {        // кэш полон — начинаем заново
        g_cache_used = 0;
        g_arena_used = 0;
    }

    int gi = glyph_index(f, cp);
    if (!gi && cp != 0) return 0;

    float scale = (float)px / (float)f->upem;
    s_npts = 0;
    load_glyph_outline(f, gi, scale, 0.0f, 0.0f, -scale, 0.0f, 0.0f, 0);   // y вниз
    build_edges();

    ttf_glyph_t* slot = &g_cache[g_cache_used];
    slot->key = key;
    slot->adv = (int16_t)((float)glyph_advance(f, gi) * scale + 0.5f);
    slot->w = slot->h = slot->bx = slot->by = 0;
    slot->off = g_arena_used;

    if (s_nedges == 0) {                           // пробел и прочие пустые глифы
        g_cache_used++;
        return slot;
    }

    // Рамка глифа
    float minx = s_edges[0].x0, maxx = minx, miny = s_edges[0].y0, maxy = s_edges[0].y1;
    for (int i = 0; i < s_nedges; i++) {
        const ttf_edge_t* e = &s_edges[i];
        float xa = e->x0, xb = e->x0 + (e->y1 - e->y0) * e->slope;
        if (xa < minx) minx = xa;
        if (xb < minx) minx = xb;
        if (xa > maxx) maxx = xa;
        if (xb > maxx) maxx = xb;
        if (e->y0 < miny) miny = e->y0;
        if (e->y1 > maxy) maxy = e->y1;
    }
    int x0 = (int)(minx > 0 ? minx : minx - 1.0f);
    int y0 = (int)(miny > 0 ? miny : miny - 1.0f);
    int x1 = (int)(maxx > 0 ? maxx + 1.0f : maxx);
    int y1 = (int)(maxy > 0 ? maxy + 1.0f : maxy);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > MAX_GLYPH_W || h > MAX_GLYPH_H) { g_cache_used++; return slot; }
    if (g_arena_used + (uint32_t)(w * h) > TTF_CACHE_BYTES) {   // арена кончилась
        g_cache_used = 0;
        g_arena_used = 0;
        slot = &g_cache[0];
        slot->key = key;
        slot->off = 0;
        slot->adv = (int16_t)((float)glyph_advance(f, gi) * scale + 0.5f);
    }

    uint8_t* cov = &g_arena[slot->off];
    for (int i = 0; i < w * h; i++) cov[i] = 0;

    ttf_hit_t hits[64];
    for (int sy = 0; sy < h * SS; sy++) {
        float y = (float)y0 + ((float)sy + 0.5f) / (float)SS;
        int n = fill_scanline(y, hits, 64);
        if (n < 2) continue;
        uint8_t* row = &cov[(sy / SS) * w];
        int wind = 0;
        for (int i = 0; i < n - 1; i++) {
            wind += hits[i].dir;
            if (!wind) continue;                   // правило ненулевого обхода
            int sa = (int)((hits[i].x - (float)x0) * SS + 0.5f);
            int sb = (int)((hits[i + 1].x - (float)x0) * SS + 0.5f);
            if (sa < 0) sa = 0;
            if (sb > w * SS) sb = w * SS;
            for (int sx = sa; sx < sb; sx++) {
                uint8_t* c = &row[sx / SS];
                if (*c < 255) (*c)++;              // копим подвыборки (их 16 на пиксель)
            }
        }
    }
    // Доля закрытых подвыборок -> прозрачность
    for (int i = 0; i < w * h; i++) {
        uint32_t v = cov[i];
        cov[i] = (v >= SS * SS) ? 255 : (uint8_t)((v * 255) / (SS * SS));
    }

    slot->w = (int16_t)w;
    slot->h = (int16_t)h;
    slot->bx = (int16_t)x0;
    slot->by = (int16_t)(-y0);                     // верх карты над базовой линией
    g_arena_used += (uint32_t)(w * h);
    g_cache_used++;
    return slot;
}

int ttf_text_width(ttf_face_t* f, const char* utf8, int px) {
    const uint8_t* s = (const uint8_t*)utf8;
    int wsum = 0;
    while (*s) {
        uint32_t cp = ttf_utf8_next(&s);
        const ttf_glyph_t* g = ttf_glyph(f, cp, px);
        if (g) wsum += g->adv;
    }
    return wsum;
}

int ttf_ascent(const ttf_face_t* f, int px)      { return (f->ascent * px) / f->upem; }
int ttf_line_height(const ttf_face_t* f, int px) { return ((f->ascent + f->descent) * px) / f->upem; }
// Базовая линия внутри строки: прописные начинаются на 2 пикселя ниже верха —
// так новый шрифт встаёт на место прежнего растрового 8x16 и вёрстка не едет
int ttf_baseline(const ttf_face_t* f, int px)    { return (f->cap_height * px) / f->upem + 2; }
