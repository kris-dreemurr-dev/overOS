// =============================================================================
// burn.c — «пушистый бублик» лучами по функции расстояния (см. burn.h).
//
// Векторы — расширение GCC (vector_size), а не <xmmintrin.h>: тот тянет за
// собой stdlib.h, которого в системе нет. sqrt, min, max — встроенные SSE.
// Синус — свой (в программах нет libm): свёртка к [-pi, pi] и парабола с
// уточнением, ошибки ~0.001 для меха хватает.
// =============================================================================

#include "burn.h"

typedef float v4 __attribute__((vector_size(16)));
typedef int   v4i __attribute__((vector_size(16)));

static inline v4 vs(float x) { return (v4){ x, x, x, x }; }
static inline v4 vsqrt(v4 a) { return __builtin_ia32_sqrtps(a); }
static inline v4 vmax(v4 a, v4 b) { return __builtin_ia32_maxps(a, b); }
static inline v4 vmin(v4 a, v4 b) { return __builtin_ia32_minps(a, b); }
static inline v4 vabs(v4 a) { return (v4)((v4i)a & (v4i){ 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF }); }
static inline v4 vround(v4 a) { return __builtin_ia32_cvtdq2ps(__builtin_ia32_cvtps2dq(a)); }
static inline v4 vsel(v4i m, v4 a, v4 b) { return (v4)(((v4i)a & m) | ((v4i)b & ~m)); }

static inline v4 vsin(v4 x) {
    const float PI = 3.14159265f;
    x = x - vround(x * vs(1.0f / (2.0f * PI))) * vs(2.0f * PI);      // [-pi, pi]
    v4 y = x * vs(4.0f / PI) - x * vabs(x) * vs(4.0f / (PI * PI));
    return y + vs(0.225f) * (y * vabs(y) - y);                      // уточнение параболы
}

// Сцена в координатах тора (поворот уже применён к лучу)
typedef struct { float R, r, fur, fa, fb, fc, t; } scene_t;

static inline v4 sdf(const scene_t* s, v4 px, v4 py, v4 pz, v4* hair) {
    v4 qx = vsqrt(px * px + pz * pz) - vs(s->R);
    v4 d = vsqrt(qx * qx + py * py) - vs(s->r);
    // Мех: три синуса под разными частотами, шевелятся во времени
    v4 h = vsin(px * vs(s->fa) + vs(s->t)) * vsin(py * vs(s->fb) - vs(s->t * 0.7f)) * vsin(pz * vs(s->fc) + vs(s->t * 1.3f));
    *hair = h;
    return d - h * vs(s->fur);
}

// Четыре луча: возвращает цвет в r/g/b (0..1)
static void trace4(const scene_t* s, const float m[9], v4 ox, v4 oy, v4 oz, v4 dx, v4 dy, v4 dz,
                   int steps, v4* cr, v4* cg, v4* cb) {
    // Луч в координаты тора: поворот — транспонированная матрица
    v4 rox = ox * vs(m[0]) + oy * vs(m[3]) + oz * vs(m[6]);
    v4 roy = ox * vs(m[1]) + oy * vs(m[4]) + oz * vs(m[7]);
    v4 roz = ox * vs(m[2]) + oy * vs(m[5]) + oz * vs(m[8]);
    v4 rdx = dx * vs(m[0]) + dy * vs(m[3]) + dz * vs(m[6]);
    v4 rdy = dx * vs(m[1]) + dy * vs(m[4]) + dz * vs(m[7]);
    v4 rdz = dx * vs(m[2]) + dy * vs(m[5]) + dz * vs(m[8]);
    v4 t = vs(0.0f), d = vs(1.0f), hair = vs(0.0f);
    for (int i = 0; i < steps; i++) {
        v4 px = rox + rdx * t, py = roy + rdy * t, pz = roz + rdz * t;
        d = sdf(s, px, py, pz, &hair);
        t = t + vmax(d, vs(0.0f)) * vs(0.6f);         // мех ломает «честное» расстояние — шаг короче
        t = vmin(t, vs(12.0f));
    }
    v4i hit = (v4i)(d < vs(0.03f)) & (v4i)(t < vs(11.0f));
    // Нормаль — четыре пробы тетраэдром
    v4 px = rox + rdx * t, py = roy + rdy * t, pz = roz + rdz * t, hh;
    const float e = 0.004f;
    v4 a = sdf(s, px + vs(e), py - vs(e), pz - vs(e), &hh);
    v4 b = sdf(s, px - vs(e), py - vs(e), pz + vs(e), &hh);
    v4 c = sdf(s, px - vs(e), py + vs(e), pz - vs(e), &hh);
    v4 f = sdf(s, px + vs(e), py + vs(e), pz + vs(e), &hh);
    v4 nx = a - b - c + f, ny = -a - b + c + f, nz = -a + b - c + f;
    v4 inv = vs(1.0f) / vsqrt(nx * nx + ny * ny + nz * nz + vs(1e-12f));
    nx *= inv; ny *= inv; nz *= inv;
    // Свет — в координатах тора (тот же поворот), чтобы блик не ездил по шерсти
    const float lx = 0.57f, ly = 0.66f, lz = 0.49f;
    v4 llx = vs(lx * m[0] + ly * m[3] + lz * m[6]), lly = vs(lx * m[1] + ly * m[4] + lz * m[7]), llz = vs(lx * m[2] + ly * m[5] + lz * m[8]);
    v4 diff = vmax(nx * llx + ny * lly + nz * llz, vs(0.0f));
    // Полувектор к камере: блик — 16-я степень повторным возведением в квадрат
    v4 hx = llx - rdx, hy = lly - rdy, hz = llz - rdz;
    v4 hl = vs(1.0f) / vsqrt(hx * hx + hy * hy + hz * hz);
    v4 sp = vmax((nx * hx + ny * hy + nz * hz) * hl, vs(0.0f));
    sp = sp * sp; sp = sp * sp; sp = sp * sp; sp = sp * sp;
    v4 rim = vs(1.0f) - vabs(nx * rdx + ny * rdy + nz * rdz);
    rim = rim * rim;
    // Мех: во впадинах темнее (как затенение у шерсти)
    v4 ao = vs(0.45f) + vs(0.55f) * (hair * vs(0.5f) + vs(0.5f));
    v4 lit = (vs(0.18f) + vs(0.82f) * diff) * ao;
    v4 fr = lit * vs(0.95f) + sp * vs(0.5f) + rim * vs(0.35f);
    v4 fg = lit * vs(0.52f) + sp * vs(0.45f) + rim * vs(0.22f);
    v4 fb = lit * vs(0.16f) + sp * vs(0.4f) + rim * vs(0.55f);
    // Фон: тёмный градиент по направлению луча
    v4 br = vs(0.05f) + dy * vs(0.10f), bgc = vs(0.06f) + dy * vs(0.06f), bb = vs(0.12f) + dy * vs(0.25f);
    *cr = vsel(hit, fr, br);
    *cg = vsel(hit, fg, bgc);
    *cb = vsel(hit, fb, bb);
}

static inline uint32_t pack(float r, float g, float b) {
    int R = (int)(r * 255.0f), G = (int)(g * 255.0f), B = (int)(b * 255.0f);
    R = R < 0 ? 0 : (R > 255 ? 255 : R);
    G = G < 0 ? 0 : (G > 255 ? 255 : G);
    B = B < 0 ? 0 : (B > 255 ? 255 : B);
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

static float fsin(float x) { v4 v = vsin(vs(x)); return v[0]; }

void burn_rows(const burn_frame_t* f, int y0, int y1) {
    const float t = f->time;
    scene_t s = { 1.0f, 0.42f, 0.06f, 31.0f, 29.0f, 37.0f, t * 3.0f };
    // Поворот тора: два угла во времени
    float a = t * 0.7f, b = t * 0.45f;
    float ca = fsin(a + 1.5707963f), sa = fsin(a), cb = fsin(b + 1.5707963f), sb = fsin(b);
    const float m[9] = { ca, -sa * cb, sa * sb,  sa, ca * cb, -ca * sb,  0.0f, sb, cb };
    const float aspect = (float)f->w / (float)(f->h > 0 ? f->h : 1);
    const float k = 2.0f / (float)f->h;
    const v4 oz = vs(3.4f);
    for (int y = y0; y < y1; y++) {
        uint32_t* row = f->out + y * f->w;
        if (f->ssaa) {
            // Четыре луча — четыре подвыборки одного пикселя
            for (int x = 0; x < f->w; x++) {
                v4 sx = (vs((float)x) + (v4){ 0.25f, 0.75f, 0.25f, 0.75f }) * vs(k) - vs(aspect);
                v4 sy = vs(1.0f) - (vs((float)y) + (v4){ 0.25f, 0.25f, 0.75f, 0.75f }) * vs(k);
                v4 dz = vs(-1.7f), il = vs(1.0f) / vsqrt(sx * sx + sy * sy + dz * dz);
                v4 r, g, bb;
                trace4(&s, m, vs(0.0f), vs(0.0f), oz, sx * il, sy * il, dz * il, f->steps, &r, &g, &bb);
                row[x] = pack((r[0] + r[1] + r[2] + r[3]) * 0.25f, (g[0] + g[1] + g[2] + g[3]) * 0.25f,
                              (bb[0] + bb[1] + bb[2] + bb[3]) * 0.25f);
            }
        } else {
            for (int x = 0; x < f->w; x += 4) {
                v4 sx = (vs((float)x) + (v4){ 0.5f, 1.5f, 2.5f, 3.5f }) * vs(k) - vs(aspect);
                v4 sy = vs(1.0f - ((float)y + 0.5f) * k);
                v4 dz = vs(-1.7f), il = vs(1.0f) / vsqrt(sx * sx + sy * sy + dz * dz);
                v4 r, g, bb;
                trace4(&s, m, vs(0.0f), vs(0.0f), oz, sx * il, sy * il, dz * il, f->steps, &r, &g, &bb);
                for (int i = 0; i < 4 && x + i < f->w; i++) row[x + i] = pack(r[i], g[i], bb[i]);
            }
        }
    }
}
