// svg.c — растеризатор значков SVG (подмножество) для overOS.
//
// Разбор идёт по тексту файла: ищем элементы, у каждого читаем атрибуты, пути
// переводим в отрезки (кривые Безье разбиваются), а затем закрашиваем тем же
// способом, что и глифы шрифта: построчная развёртка с правилом ненулевого
// обхода и супервыборкой 4x4. Обводка (stroke) рисуется как набор
// четырёхугольников по отрезкам — для значков этого достаточно.
//
// Всё на статических массивах: libc в системе нет, а значки рисуются и в
// композиторе, и в программах.

#include "svg.h"

// Сглаживание: по вертикали 16 подстрок на пиксель, по горизонтали покрытие
// считается точно (доля пикселя, а не подвыборки) — это даёт 4096 градаций
// вместо 64 и убирает «лесенку» на пологих краях значков.
#define SVG_SS        16     // подстрок на пиксель
#define SVG_MAX_PTS   6144
#define SVG_MAX_EDGES 24576

typedef struct { float x, y; uint8_t last; } svg_pt_t;   // last — конец контура
typedef struct { float y0, y1, x0, slope; int dir; } svg_edge_t;

static svg_pt_t   s_pts[SVG_MAX_PTS];
static int        s_npts;
static svg_edge_t s_edges[SVG_MAX_EDGES];
static int        s_nedges;

// --- мелкие помощники --------------------------------------------------------
static int svg_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; }
static int svg_is_digit(char c) { return c >= '0' && c <= '9'; }

static float svg_atof(const char** p) {
    const char* s = *p;
    while (svg_is_space(*s)) s++;
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }
    float v = 0.0f;
    while (svg_is_digit(*s)) { v = v * 10.0f + (float)(*s - '0'); s++; }
    if (*s == '.') {
        s++;
        float f = 0.1f;
        while (svg_is_digit(*s)) { v += (float)(*s - '0') * f; f *= 0.1f; s++; }
    }
    if (*s == 'e' || *s == 'E') {              // экспонента встречается в экспортах
        s++;
        int eneg = 0;
        if (*s == '+') s++; else if (*s == '-') { eneg = 1; s++; }
        int e = 0;
        while (svg_is_digit(*s)) { e = e * 10 + (*s - '0'); s++; }
        while (e-- > 0) v = eneg ? v * 0.1f : v * 10.0f;
    }
    *p = s;
    return neg ? -v : v;
}

static int svg_streq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// Атрибут элемента: ищем name="..." внутри куска текста [el, end)
static const char* svg_attr(const char* el, const char* end, const char* name, int* out_len) {
    int nl = 0;
    while (name[nl]) nl++;
    for (const char* p = el; p + nl + 2 < end; p++) {
        if (!svg_streq(p, name, nl)) continue;
        if (p > el && !svg_is_space(p[-1])) continue;      // не кусок другого имени
        const char* q = p + nl;
        while (q < end && svg_is_space(*q)) q++;
        if (q >= end || *q != '=') continue;
        q++;
        while (q < end && svg_is_space(*q)) q++;
        if (q >= end || (*q != '"' && *q != '\'')) continue;
        char quote = *q++;
        const char* val = q;
        while (q < end && *q != quote) q++;
        *out_len = (int)(q - val);
        return val;
    }
    return 0;
}

// --- цвет --------------------------------------------------------------------
#define SVG_NONE 0x01000000u      // «не закрашивать» — альфа 0 и бит-маркер

static uint32_t svg_hex(char c) {
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint32_t)(c - 'A' + 10);
    return 0;
}

static uint32_t svg_color(const char* v, int len, uint32_t tint, uint32_t inherited) {
    if (!v || len <= 0) return inherited;
    if (v[0] == '#') {
        if (len >= 7) {
            return (svg_hex(v[1]) << 20) | (svg_hex(v[2]) << 16) | (svg_hex(v[3]) << 12) |
                   (svg_hex(v[4]) << 8)  | (svg_hex(v[5]) << 4)  |  svg_hex(v[6]);
        }
        if (len >= 4) {
            uint32_t r = svg_hex(v[1]), g = svg_hex(v[2]), b = svg_hex(v[3]);
            return (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
        }
        return inherited;
    }
    if (len >= 4 && svg_streq(v, "none", 4)) return SVG_NONE;
    if (len >= 12 && svg_streq(v, "currentColor", 12)) return tint;
    if (len >= 5 && svg_streq(v, "white", 5)) return 0xFFFFFF;
    if (len >= 5 && svg_streq(v, "black", 5)) return 0x000000;
    if (len >= 3 && svg_streq(v, "red", 3)) return 0xFF0000;
    return inherited;
}

// --- преобразования ----------------------------------------------------------
typedef struct { float a, b, c, d, e, f; } svg_mat_t;

static svg_mat_t svg_mul(svg_mat_t m, svg_mat_t n) {
    svg_mat_t r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}

static svg_mat_t svg_parse_transform(const char* v, int len, svg_mat_t base) {
    svg_mat_t m = base;
    const char* p = v;
    const char* end = v + len;
    while (p < end) {
        while (p < end && svg_is_space(*p)) p++;
        if (p + 6 < end && svg_streq(p, "translate", 9)) {
            p += 9;
            while (p < end && *p != '(') p++;
            p++;
            float tx = svg_atof(&p), ty = 0;
            while (p < end && svg_is_space(*p)) p++;
            if (p < end && *p != ')') ty = svg_atof(&p);
            svg_mat_t t = { 1, 0, 0, 1, tx, ty };
            m = svg_mul(m, t);
        } else if (p + 5 < end && svg_streq(p, "scale", 5)) {
            p += 5;
            while (p < end && *p != '(') p++;
            p++;
            float sx = svg_atof(&p), sy;
            while (p < end && svg_is_space(*p)) p++;
            sy = (p < end && *p != ')') ? svg_atof(&p) : sx;
            svg_mat_t t = { sx, 0, 0, sy, 0, 0 };
            m = svg_mul(m, t);
        } else if (p + 6 < end && svg_streq(p, "matrix", 6)) {
            p += 6;
            while (p < end && *p != '(') p++;
            p++;
            svg_mat_t t;
            t.a = svg_atof(&p); t.b = svg_atof(&p); t.c = svg_atof(&p);
            t.d = svg_atof(&p); t.e = svg_atof(&p); t.f = svg_atof(&p);
            m = svg_mul(m, t);
        } else {
            p++;
        }
        while (p < end && *p != ')' ) p++;
        if (p < end) p++;
    }
    return m;
}

// Синус и косинус без libm и без потери точности на больших углах.
//
// Ряд Тейлора хорош только около нуля: на 180 градусах укороченный ряд давал
// cos = -1.21 вместо -1, и круг в значке получался с изломом слева. Поэтому
// считаем ряд для четверти угла (|a/4| <= 0.8, там ошибка ~1e-7) и дважды
// применяем формулы двойного угла.
static void svg_sincos(float a, float* so, float* co) {
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    float q4 = a * 0.25f, q = q4 * q4;
    float s1 = q4 * (1.0f - q / 6.0f * (1.0f - q / 20.0f * (1.0f - q / 42.0f)));
    float c1 = 1.0f - q / 2.0f * (1.0f - q / 12.0f * (1.0f - q / 30.0f));
    float s2 = 2.0f * s1 * c1, c2 = 1.0f - 2.0f * s1 * s1;   // половина угла
    *so = 2.0f * s2 * c2;
    *co = 1.0f - 2.0f * s2 * s2;                             // весь угол
}

// --- накопление контуров -----------------------------------------------------
static float s_cur_x, s_cur_y, s_start_x, s_start_y;
static int   s_contour_start;

static void pt_add(float x, float y) {
    if (s_npts < SVG_MAX_PTS) { s_pts[s_npts].x = x; s_pts[s_npts].y = y; s_pts[s_npts].last = 0; s_npts++; }
}

static void edge_add(float x0, float y0, float x1, float y1) {
    if (y0 == y1 || s_nedges >= SVG_MAX_EDGES) return;
    int dir = 1;
    if (y1 < y0) { float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1;
    }
    svg_edge_t* e = &s_edges[s_nedges++];
    e->y0 = y0; e->y1 = y1; e->x0 = x0;
    e->slope = (x1 - x0) / (y1 - y0);
    e->dir = dir;
}

// Закрыть контур: просто помечаем последнюю точку. Рёбра строятся потом, уже
// после преобразования координат, и границы контуров при этом важны — иначе
// у буквы «о» внешний и внутренний контур соединились бы отрезком.
static void contour_close(void) {
    if (s_npts > s_contour_start) s_pts[s_npts - 1].last = 1;
    s_contour_start = s_npts;
}

// Контуры -> рёбра (каждый контур замыкается сам на себя)
static void build_edges_from_pts(void) {
    s_nedges = 0;
    int start = 0;
    for (int i = 0; i < s_npts; i++) {
        if (!s_pts[i].last && i + 1 < s_npts) continue;
        int end = i;
        for (int k = start; k < end; k++)
            edge_add(s_pts[k].x, s_pts[k].y, s_pts[k + 1].x, s_pts[k + 1].y);
        if (end > start)
            edge_add(s_pts[end].x, s_pts[end].y, s_pts[start].x, s_pts[start].y);
        start = end + 1;
    }
}

// Сколько отрезков на кривую: по длине её опорной ломаной в пикселях вывода.
// Фиксированное число ломало и мелкие значки (лишняя работа), и крупные
// (кривая превращалась в многоугольник с видимыми гранями).
static int curve_steps(float dx0, float dy0, float dx1, float dy1, float dx2, float dy2) {
    float a = dx0 < 0 ? -dx0 : dx0, b = dy0 < 0 ? -dy0 : dy0;
    float c = dx1 < 0 ? -dx1 : dx1, d = dy1 < 0 ? -dy1 : dy1;
    float e = dx2 < 0 ? -dx2 : dx2, f = dy2 < 0 ? -dy2 : dy2;
    float len = a + b + c + d + e + f;               // длина по манхэттену: дёшево и достаточно
    int n = (int)(len * 0.7f);
    if (n < 4) n = 4;
    if (n > 64) n = 64;
    return n;
}

static void cubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
    const int n = curve_steps(x1 - x0, y1 - y0, x2 - x1, y2 - y1, x3 - x2, y3 - y2);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        float x = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
        float y = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
        pt_add(x, y);
    }
}

static void quad(float x0, float y0, float x1, float y1, float x2, float y2) {
    const int n = curve_steps(x1 - x0, y1 - y0, x2 - x1, y2 - y1, 0.0f, 0.0f);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        pt_add(u*u*x0 + 2*u*t*x1 + t*t*x2, u*u*y0 + 2*u*t*y1 + t*t*y2);
    }
}

// atan2 без libm: приближение через отношение катетов, значкам точности хватает
static float svg_atan2(float y, float x) {
    if (x == 0.0f && y == 0.0f) return 0.0f;
    float ax = y < 0 ? -y : y, bx = x < 0 ? -x : x;
    float z = (ax < bx) ? ax / bx : bx / ax;
    float t = z * (0.9724f - 0.1919f * z * z);
    if (ax > bx) t = 1.5708f - t;
    if (x < 0) t = 3.14159f - t;
    if (y < 0) t = -t;
    return t;
}

// Дуга (A): переводим в кривые приближённо — через несколько отрезков по углу
static void arc_to(float x0, float y0, float rx, float ry, float rot,
                   int large, int sweep, float x, float y) {
    (void)rot;
    if (rx <= 0 || ry <= 0) { pt_add(x, y); return; }
    // Центр окружности для приближения: берём середину хорды и смещаем по нормали
    float dx = (x0 - x) * 0.5f, dy = (y0 - y) * 0.5f;
    float d2 = dx * dx / (rx * rx) + dy * dy / (ry * ry);
    float scale = 0.0f;
    if (d2 < 1.0f) {
        float t = (1.0f - d2) / d2;
        // корень без libm: метод Ньютона, значения тут небольшие
        float r = t > 1.0f ? t : 1.0f;
        for (int i = 0; i < 12; i++) r = 0.5f * (r + t / r);
        scale = r;
    }
    if (large == sweep) scale = -scale;
    float cx = scale * rx * dy / ry + (x0 + x) * 0.5f;
    float cy = -scale * ry * dx / rx + (y0 + y) * 0.5f;
    float a0 = 0, a1 = 0;
    // Углы считаем через отношение координат, достаточно грубого приближения
    float ux = (x0 - cx) / rx, uy = (y0 - cy) / ry;
    float vx = (x - cx) / rx, vy = (y - cy) / ry;
    a0 = svg_atan2(uy, ux);
    a1 = svg_atan2(vy, vx);
    float sweep_a = a1 - a0;
    if (sweep && sweep_a < 0) sweep_a += 6.28318f;
    if (!sweep && sweep_a > 0) sweep_a -= 6.28318f;
    int steps = (int)((rx > ry ? rx : ry) * (sweep_a < 0 ? -sweep_a : sweep_a) * 0.7f);
    if (steps < 6) steps = 6;
    if (steps > 64) steps = 64;
    for (int i = 1; i <= steps; i++) {
        float sn, cs;
        svg_sincos(a0 + sweep_a * (float)i / (float)steps, &sn, &cs);
        pt_add(cx + rx * cs, cy + ry * sn);
    }
}

// Разбор атрибута d="..."
static void path_parse(const char* d, int len) {
    const char* p = d;
    const char* end = d + len;
    char cmd = 0, prev = 0;
    float cx1 = 0, cy1 = 0;     // прошлая управляющая точка (для S и T)
    while (p < end) {
        while (p < end && svg_is_space(*p)) p++;
        if (p >= end) break;
        if (!svg_is_digit(*p) && *p != '-' && *p != '+' && *p != '.') { prev = cmd; cmd = *p++; }
        int rel = (cmd >= 'a' && cmd <= 'z');
        char c = rel ? (char)(cmd - 32) : cmd;
        float bx = rel ? s_cur_x : 0.0f, by = rel ? s_cur_y : 0.0f;
        switch (c) {
            case 'M': {
                float x = svg_atof(&p) + bx, y = svg_atof(&p) + by;
                contour_close();
                pt_add(x, y);
                s_cur_x = s_start_x = x; s_cur_y = s_start_y = y;
                cmd = rel ? 'l' : 'L';                 // дальнейшие пары — линии
                break;
            }
            case 'L': {
                float x = svg_atof(&p) + bx, y = svg_atof(&p) + by;
                pt_add(x, y); s_cur_x = x; s_cur_y = y;
                break;
            }
            case 'H': { float x = svg_atof(&p) + bx; pt_add(x, s_cur_y); s_cur_x = x; break; }
            case 'V': { float y = svg_atof(&p) + by; pt_add(s_cur_x, y); s_cur_y = y; break; }
            case 'C': {
                float x1 = svg_atof(&p) + bx, y1 = svg_atof(&p) + by;
                float x2 = svg_atof(&p) + bx, y2 = svg_atof(&p) + by;
                float x = svg_atof(&p) + bx, y = svg_atof(&p) + by;
                cubic(s_cur_x, s_cur_y, x1, y1, x2, y2, x, y);
                cx1 = x2; cy1 = y2; s_cur_x = x; s_cur_y = y;
                break;
            }
            case 'S': {
                float x2 = svg_atof(&p) + bx, y2 = svg_atof(&p) + by;
                float x = svg_atof(&p) + bx, y = svg_atof(&p) + by;
                float x1 = s_cur_x, y1 = s_cur_y;
                if (prev == 'C' || prev == 'c' || prev == 'S' || prev == 's') {
                    x1 = 2 * s_cur_x - cx1; y1 = 2 * s_cur_y - cy1;
                }
                cubic(s_cur_x, s_cur_y, x1, y1, x2, y2, x, y);
                cx1 = x2; cy1 = y2; s_cur_x = x; s_cur_y = y;
                break;
            }
            case 'Q': {
                float x1 = svg_atof(&p) + bx, y1 = svg_atof(&p) + by;
                float x = svg_atof(&p) + bx, y = svg_atof(&p) + by;
                quad(s_cur_x, s_cur_y, x1, y1, x, y);
                cx1 = x1; cy1 = y1; s_cur_x = x; s_cur_y = y;
                break;
            }
            case 'T': {
                float x = svg_atof(&p) + bx, y = svg_atof(&p) + by;
                float x1 = s_cur_x, y1 = s_cur_y;
                if (prev == 'Q' || prev == 'q' || prev == 'T' || prev == 't') {
                    x1 = 2 * s_cur_x - cx1; y1 = 2 * s_cur_y - cy1;
                }
                quad(s_cur_x, s_cur_y, x1, y1, x, y);
                cx1 = x1; cy1 = y1; s_cur_x = x; s_cur_y = y;
                break;
            }
            case 'A': {
                float rx = svg_atof(&p), ry = svg_atof(&p), rot = svg_atof(&p);
                int large = (int)svg_atof(&p), sweep = (int)svg_atof(&p);
                float x = svg_atof(&p) + bx, y = svg_atof(&p) + by;
                arc_to(s_cur_x, s_cur_y, rx, ry, rot, large, sweep, x, y);
                s_cur_x = x; s_cur_y = y;
                break;
            }
            case 'Z':
                contour_close();
                s_cur_x = s_start_x; s_cur_y = s_start_y;
                break;
            default:
                p++;                                   // неизвестная команда — пропускаем
                break;
        }
    }
    contour_close();
}

// --- заливка -----------------------------------------------------------------
typedef struct { float x; int dir; } svg_hit_t;

static void blend_px(uint32_t* px, uint32_t color, uint32_t a) {
    if (!a) return;
    uint32_t dst = *px;
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t na = a + da * (255 - a) / 255;
    if (!na) { *px = 0; return; }
    uint32_t r = (((color >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * da * (255 - a) / 255) / na;
    uint32_t g = (((color >> 8)  & 0xFF) * a + ((dst >> 8)  & 0xFF) * da * (255 - a) / 255) / na;
    uint32_t b = (((color)       & 0xFF) * a + ((dst)       & 0xFF) * da * (255 - a) / 255) / na;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *px = (na << 24) | (r << 16) | (g << 8) | b;
}

// Закрасить накопленные рёбра цветом color с прозрачностью alpha (0..255).
//
// Для каждой подстроки находим пересечения с рёбрами, сортируем их и по правилу
// ненулевого обхода получаем отрезки закраски. Отрезок добавляется в покрытие
// точно: целым пикселям идёт полная доля, крайним — та часть, которую отрезок
// реально накрывает. Поэтому вертикаль квантуется подстроками (1/16 пикселя), а
// горизонталь не квантуется вовсе.
#define SVG_UNIT 256u                        // доля пикселя в одной подстроке

static void fill_edges(uint32_t* out, int w, int h, uint32_t color, uint32_t alpha) {
    if (!s_nedges || !alpha) return;
    static uint16_t cov[256 * 256];
    if (w > 256 || h > 256) return;
    for (int i = 0; i < w * h; i++) cov[i] = 0;

    svg_hit_t hits[256];
    for (int sy = 0; sy < h * SVG_SS; sy++) {
        float y = ((float)sy + 0.5f) / (float)SVG_SS;
        int n = 0;
        for (int i = 0; i < s_nedges && n < 256; i++) {
            const svg_edge_t* e = &s_edges[i];
            if (y < e->y0 || y >= e->y1) continue;
            hits[n].x = e->x0 + (y - e->y0) * e->slope;
            hits[n].dir = e->dir;
            n++;
        }
        if (n < 2) continue;
        for (int i = 1; i < n; i++) {               // сортировка вставками
            svg_hit_t t = hits[i];
            int j = i - 1;
            while (j >= 0 && hits[j].x > t.x) { hits[j + 1] = hits[j]; j--; }
            hits[j + 1] = t;
        }
        uint16_t* row = &cov[(sy / SVG_SS) * w];
        int wind = 0;
        for (int i = 0; i < n - 1; i++) {
            wind += hits[i].dir;
            if (!wind) continue;
            float xa = hits[i].x, xb = hits[i + 1].x;
            if (xb <= 0.0f || xa >= (float)w || xb <= xa) continue;
            if (xa < 0.0f) xa = 0.0f;
            if (xb > (float)w) xb = (float)w;
            int pa = (int)xa, pb = (int)xb;
            if (pb >= w) pb = w - 1;
            if (pa == pb) {                          // отрезок внутри одного пикселя
                row[pa] = (uint16_t)(row[pa] + (uint32_t)((xb - xa) * SVG_UNIT));
                continue;
            }
            row[pa] = (uint16_t)(row[pa] + (uint32_t)(((float)(pa + 1) - xa) * SVG_UNIT));
            for (int px = pa + 1; px < pb; px++) row[px] = (uint16_t)(row[px] + SVG_UNIT);
            row[pb] = (uint16_t)(row[pb] + (uint32_t)((xb - (float)pb) * SVG_UNIT));
        }
    }
    const uint32_t full = SVG_UNIT * SVG_SS;
    for (int i = 0; i < w * h; i++) {
        uint32_t v = cov[i];
        if (!v) continue;
        uint32_t a = (v >= full) ? 255 : (v * 255 + full / 2) / full;
        blend_px(&out[i], color, a * alpha / 255);
    }
}

// Обводка: каждый отрезок превращается в четырёхугольник, стыки и концы
// закрываются кружками. Все фигуры копим в один набор рёбер и закрашиваем
// разом: если заливать каждый отрезок отдельно, на стыках прозрачность
// складывается и видны тёмные пятна.
// Многоугольник в набор рёбер с приведением обхода к одной стороне.
//
// Это важно именно для обводки: она складывается из прямоугольников отрезков и
// кружков на стыках, и всё это закрашивается одним проходом по правилу
// ненулевого обхода. Если один кусок обойдён по часовой стрелке, а другой
// против, их обходы в месте наложения взаимно уничтожаются — в кольце
// появляются «зубцы» ровно на стыках. Поэтому ориентацию выравниваем по знаку
// площади.
static void edge_add_poly(const svg_pt_t* p, int n) {
    if (n < 3) return;
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        const svg_pt_t* a = &p[i];
        const svg_pt_t* b = &p[(i + 1) % n];
        area += a->x * b->y - b->x * a->y;
    }
    if (area >= 0.0f) {
        for (int i = 0; i < n; i++) edge_add(p[i].x, p[i].y, p[(i + 1) % n].x, p[(i + 1) % n].y);
    } else {
        for (int i = n - 1; i >= 0; i--) {
            int j = (i == 0) ? n - 1 : i - 1;
            edge_add(p[i].x, p[i].y, p[j].x, p[j].y);
        }
    }
}

static void stroke_add_disc(float cx, float cy, float r) {
    int n = (int)(r * 6.0f);
    if (n < 8) n = 8;
    if (n > 48) n = 48;
    svg_pt_t pts[48];
    for (int i = 0; i < n; i++) {
        float sn, cs;
        svg_sincos(6.28318531f * (float)i / (float)n, &sn, &cs);
        pts[i].x = cx + r * cs;
        pts[i].y = cy + r * sn;
        pts[i].last = 0;
    }
    edge_add_poly(pts, n);
}

static void stroke_contours(uint32_t* out, int w, int h, float width, uint32_t color, uint32_t alpha) {
    if (width <= 0.0f || !alpha) return;
    int npts = s_npts;
    static svg_pt_t saved[SVG_MAX_PTS];
    for (int i = 0; i < npts; i++) saved[i] = s_pts[i];

    float hw = width * 0.5f;
    if (hw < 0.35f) hw = 0.35f;
    s_nedges = 0;
    int cstart = 0;
    for (int i = 0; i < npts; i++) {
        float x0 = saved[i].x, y0 = saved[i].y, x1, y1;
        if (saved[i].last) {
            // Замкнутый контур: последний отрезок идёт обратно в начало. Без него
            // у круга оставался разрыв шириной в линию.
            x1 = saved[cstart].x; y1 = saved[cstart].y;
            cstart = i + 1;
        } else {
            if (i + 1 >= npts) break;
            x1 = saved[i + 1].x; y1 = saved[i + 1].y;
        }
        float dx = x1 - x0, dy = y1 - y0;
        float len2 = dx * dx + dy * dy;
        if (len2 < 0.000001f) continue;
        float len = len2 > 1.0f ? len2 : 1.0f;       // корень методом Ньютона
        for (int k = 0; k < 12; k++) len = 0.5f * (len + len2 / len);
        float nx = -dy / len * hw, ny = dx / len * hw;
        svg_pt_t q[4] = { { x0 + nx, y0 + ny, 0 }, { x1 + nx, y1 + ny, 0 },
                          { x1 - nx, y1 - ny, 0 }, { x0 - nx, y0 - ny, 0 } };
        edge_add_poly(q, 4);
        stroke_add_disc(x0, y0, hw);                 // скругление стыков и концов
        stroke_add_disc(x1, y1, hw);
    }
    fill_edges(out, w, h, color, alpha);
    s_nedges = 0;
}

// --- разбор документа --------------------------------------------------------
typedef struct {
    uint32_t fill, stroke;
    float    stroke_w;
    uint32_t fill_a, stroke_a;
    svg_mat_t m;
} svg_state_t;

static void shape_begin(void) { s_npts = 0; s_nedges = 0; s_contour_start = 0; }

static void apply_matrix(svg_mat_t m) {
    for (int i = 0; i < s_npts; i++) {
        float x = s_pts[i].x, y = s_pts[i].y;
        s_pts[i].x = m.a * x + m.c * y + m.e;
        s_pts[i].y = m.b * x + m.d * y + m.f;
    }
}

static float attr_float(const char* el, const char* end, const char* name, float dflt) {
    int len = 0;
    const char* v = svg_attr(el, end, name, &len);
    if (!v) return dflt;
    const char* p = v;
    return svg_atof(&p);
}

int svg_render(const uint8_t* data, uint32_t len, uint32_t* out, int w, int h, uint32_t tint) {
    if (!data || !out || w <= 0 || h <= 0 || w > 256 || h > 256) return -1;
    for (int i = 0; i < w * h; i++) out[i] = 0;

    const char* d = (const char*)data;
    const char* dend = d + len;

    // viewBox задаёт систему координат значка
    float vb_x = 0, vb_y = 0, vb_w = 0, vb_h = 0;
    const char* svg_el = 0;
    for (const char* p = d; p + 4 < dend; p++) {
        if (p[0] == '<' && p[1] == 's' && p[2] == 'v' && p[3] == 'g') { svg_el = p; break; }
    }
    if (!svg_el) return -2;
    const char* svg_end = svg_el;
    while (svg_end < dend && *svg_end != '>') svg_end++;
    int vlen = 0;
    const char* vb = svg_attr(svg_el, svg_end, "viewBox", &vlen);
    if (vb) {
        const char* p = vb;
        vb_x = svg_atof(&p); vb_y = svg_atof(&p); vb_w = svg_atof(&p); vb_h = svg_atof(&p);
    }
    if (vb_w <= 0 || vb_h <= 0) {
        vb_w = attr_float(svg_el, svg_end, "width", 16.0f);
        vb_h = attr_float(svg_el, svg_end, "height", 16.0f);
        if (vb_w <= 0) vb_w = 16.0f;
        if (vb_h <= 0) vb_h = 16.0f;
    }

    // Общее преобразование: единицы значка -> пиксели буфера (с сохранением пропорций)
    float sx = (float)w / vb_w, sy = (float)h / vb_h;
    float sc = sx < sy ? sx : sy;
    float ox = ((float)w - vb_w * sc) * 0.5f - vb_x * sc;
    float oy = ((float)h - vb_h * sc) * 0.5f - vb_y * sc;
    svg_mat_t root = { sc, 0, 0, sc, ox, oy };

    // Унаследованные настройки группы: один уровень <g> (значкам хватает)
    svg_state_t st;
    st.fill = tint; st.stroke = SVG_NONE; st.stroke_w = 1.0f;
    st.fill_a = 255; st.stroke_a = 255; st.m = root;
    svg_state_t group = st;

    for (const char* p = svg_end; p < dend; p++) {
        if (*p != '<') continue;
        const char* el = p + 1;
        if (el >= dend) break;
        if (*el == '/') {                                  // закрытие
            if (el + 2 < dend && el[1] == 'g') group = st;  // вышли из <g>
            continue;
        }
        const char* eend = el;
        while (eend < dend && *eend != '>') eend++;

        svg_state_t cur = group;
        int alen = 0;
        const char* a = svg_attr(el, eend, "fill", &alen);
        if (a) cur.fill = svg_color(a, alen, tint, cur.fill);
        a = svg_attr(el, eend, "stroke", &alen);
        if (a) cur.stroke = svg_color(a, alen, tint, cur.stroke);
        a = svg_attr(el, eend, "stroke-width", &alen);
        if (a) { const char* q = a; cur.stroke_w = svg_atof(&q); }
        a = svg_attr(el, eend, "fill-opacity", &alen);
        if (a) { const char* q = a; cur.fill_a = (uint32_t)(svg_atof(&q) * 255.0f); }
        a = svg_attr(el, eend, "opacity", &alen);
        if (a) { const char* q = a; uint32_t o = (uint32_t)(svg_atof(&q) * 255.0f);
                 cur.fill_a = cur.fill_a * o / 255; cur.stroke_a = cur.stroke_a * o / 255; }
        a = svg_attr(el, eend, "transform", &alen);
        if (a) cur.m = svg_parse_transform(a, alen, cur.m);

        if (svg_streq(el, "g", 1) && (svg_is_space(el[1]) || el[1] == '>')) { group = cur; continue; }

        shape_begin();
        int have = 0;
        if (svg_streq(el, "path", 4)) {
            a = svg_attr(el, eend, "d", &alen);
            if (a) { s_cur_x = s_cur_y = 0; path_parse(a, alen); have = 1; }
        } else if (svg_streq(el, "rect", 4)) {
            float x = attr_float(el, eend, "x", 0), y = attr_float(el, eend, "y", 0);
            float rw = attr_float(el, eend, "width", 0), rh = attr_float(el, eend, "height", 0);
            float r = attr_float(el, eend, "rx", 0);
            if (rw > 0 && rh > 0) {
                if (r > 0) {
                    if (r > rw / 2) r = rw / 2;
                    if (r > rh / 2) r = rh / 2;
                    pt_add(x + r, y);
                    pt_add(x + rw - r, y);
                    quad(x + rw - r, y, x + rw, y, x + rw, y + r);
                    pt_add(x + rw, y + rh - r);
                    quad(x + rw, y + rh - r, x + rw, y + rh, x + rw - r, y + rh);
                    pt_add(x + r, y + rh);
                    quad(x + r, y + rh, x, y + rh, x, y + rh - r);
                    pt_add(x, y + r);
                    quad(x, y + r, x, y, x + r, y);
                } else {
                    pt_add(x, y); pt_add(x + rw, y); pt_add(x + rw, y + rh); pt_add(x, y + rh);
                }
                contour_close();
                have = 1;
            }
        } else if (svg_streq(el, "circle", 6) || svg_streq(el, "ellipse", 7)) {
            float cx = attr_float(el, eend, "cx", 0), cy = attr_float(el, eend, "cy", 0);
            float rx = attr_float(el, eend, "r", 0), ry = rx;
            if (rx <= 0) { rx = attr_float(el, eend, "rx", 0); ry = attr_float(el, eend, "ry", 0); }
            if (rx > 0 && ry > 0) {
                // Точек по размеру: для значка 16 px хватает 24, для 128 px нужно больше
                float rmax = rx > ry ? rx : ry;
                int n = (int)(rmax * 2.0f);
                if (n < 24) n = 24;
                if (n > 128) n = 128;
                for (int i = 0; i < n; i++) {
                    float sn, cs;
                    svg_sincos(6.28318531f * (float)i / (float)n, &sn, &cs);
                    pt_add(cx + rx * cs, cy + ry * sn);
                }
                contour_close();
                have = 1;
            }
        } else if (svg_streq(el, "line", 4)) {
            pt_add(attr_float(el, eend, "x1", 0), attr_float(el, eend, "y1", 0));
            pt_add(attr_float(el, eend, "x2", 0), attr_float(el, eend, "y2", 0));
            s_contour_start = s_npts;
            have = 1;
        } else if (svg_streq(el, "polygon", 7) || svg_streq(el, "polyline", 8)) {
            a = svg_attr(el, eend, "points", &alen);
            if (a) {
                const char* q = a;
                const char* qe = a + alen;
                while (q < qe) {
                    float x = svg_atof(&q), y = svg_atof(&q);
                    pt_add(x, y);
                    while (q < qe && svg_is_space(*q)) q++;
                    if (q >= qe) break;
                }
                contour_close();
                have = 1;
            }
        }
        if (!have) continue;

        apply_matrix(cur.m);
        if (s_contour_start < s_npts) contour_close();     // незакрытый хвост (polyline)
        if (cur.fill != SVG_NONE) {
            build_edges_from_pts();
            fill_edges(out, w, h, cur.fill, cur.fill_a);
        }
        if (cur.stroke != SVG_NONE)
            stroke_contours(out, w, h, cur.stroke_w * sc, cur.stroke, cur.stroke_a);
    }
    return 0;
}
