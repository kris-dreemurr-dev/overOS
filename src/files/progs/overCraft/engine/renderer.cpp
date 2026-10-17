// =============================================================================
// renderer.cpp — конвейер растеризатора ovengine.ll (см. renderer.h).
// =============================================================================

#include "renderer.h"
#include "cxxrt/cxxrt.h"
#include "ovos.h"

namespace ov {

static const int kBand = 16;                 // строк в полосе растеризации
static const uint32_t kTriCap = 160000;      // треугольников в кадре
static const uint32_t kBlock = 64;           // столько поток берёт за раз

static inline uint64_t tsc() {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

Rasterizer::~Rasterizer() {
    delete[] depth_;
    delete[] odepth_;
    delete[] own_color_;
    delete[] xmap_;
    delete[] sky_;
    delete[] draws_;
    delete[] tris_;
    delete[] bins_;
    delete[] counts_;
    delete[] band_start_;
    for (int i = 0; i < OE_MAX_THREADS; i++) delete[] ts_[i].scratch;
    for (int i = 0; i < ntex_; i++) delete[] tex_[i].px;
}

int Rasterizer::addTexture(int w, int h, uint32_t* px, bool with_alpha) {
    if (ntex_ >= OE_MAX_TEXTURES) { delete[] px; return OE_ERR_FULL; }
    Texture& t = tex_[ntex_];
    t.w = w;
    t.h = h;
    t.sh = 0;
    while ((1 << t.sh) < w) t.sh++;
    t.px = px;
    // Режим — по альфе: вся 255 — непрозрачная, только 0 и 255 — вырез,
    // иначе — полупрозрачная. Средний цвет — по видимым пикселям.
    bool holes = false, partial = false;
    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (int i = 0; i < w * h; i++) {
        uint32_t a = with_alpha ? px[i] >> 24 : 255;
        if (!with_alpha) px[i] |= 0xFF000000u;
        if (a == 0) { holes = true; continue; }
        if (a < 255) partial = true;
        r += (px[i] >> 16) & 255; g += (px[i] >> 8) & 255; b += px[i] & 255; n++;
    }
    t.mode = partial ? TEX_BLEND : (holes ? TEX_CUTOUT : TEX_OPAQUE);
    if (!n) n = 1;
    t.avg = (uint32_t)(((r / n) << 16) | ((g / n) << 8) | (b / n));
    return ntex_++;
}

// percent — доля разрешения окна, в которой рисуется мир (100 — полное).
// Меньше 100 — внутренний буфер и увеличение ближайшим пикселем в конце
// полосы: так игра держит FPS на слабом процессоре (динамическое разрешение).
bool Rasterizer::setTarget(uint32_t* fb, int w, int h, int percent) {
    if (percent < 25) percent = 25;
    if (percent > 100) percent = 100;
    fb_ = fb;
    fbw_ = w;
    fbh_ = h;
    int iw = w * percent / 100, ih = h * percent / 100;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    // Буферы — сразу на всё окно, а не на текущий процент: динамическое
    // разрешение меняет его каждый кадр, и раньше каждый шаг вверх заново
    // выделял три буфера по несколько мегабайт. Освобождённые меньшие куски
    // оставались дырами, куча движка росла, и развёрнутая игра падала с
    // кодом 12 (нехватка памяти в cxxrt). Теперь память меняется только
    // вместе с размером окна, и сначала всё старое освобождается.
    const int full = w * h;
    if (full > cap_) {
        delete[] depth_;
        delete[] odepth_;
        delete[] own_color_;
        depth_ = odepth_ = nullptr;
        own_color_ = nullptr;
        cap_ = full;
        depth_ = new float[cap_];
        odepth_ = new float[cap_];
        own_color_ = new uint32_t[cap_];
    }
    if (percent < 100) {
        color_ = own_color_;
        if (w > xmap_cap_) {
            delete[] xmap_;
            xmap_cap_ = w;
            xmap_ = new int[xmap_cap_];
        }
        for (int x = 0; x < w; x++) xmap_[x] = x * iw / w;     // столбец окна -> столбец буфера
    } else {
        color_ = fb;
    }
    if (ih > sky_cap_) {
        delete[] sky_;
        sky_cap_ = ih;
        sky_ = new uint32_t[sky_cap_];
    }
    pct_ = percent;
    w_ = iw;
    h_ = ih;
    return fb_ && depth_ && color_;
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, int t, int n) {
    int ra = (a >> 16) & 255, ga = (a >> 8) & 255, ba = a & 255;
    int rb = (b >> 16) & 255, gb = (b >> 8) & 255, bb = b & 255;
    int r = ra + (rb - ra) * t / n, g = ga + (gb - ga) * t / n, bl = ba + (bb - ba) * t / n;
    return (uint32_t)((r << 16) | (g << 8) | bl);
}

void Rasterizer::begin(const Camera& cam) {
    cam_ = cam;
    vp_ = cam.proj * cam.view;
    to_light_ = normalize(-cam.light.dir);
    ndraws_ = 0;
    max_nv_ = 0;
    in_frame_ = true;
    for (int y = 0; y < h_; y++) sky_[y] = lerp_rgb(cam.sky_top, cam.sky_bottom, y, h_ > 1 ? h_ - 1 : 1);
}

void Rasterizer::draw(const Mesh* mesh, const Mat4& model, int flags) {
    if (!in_frame_ || !mesh || mesh->nf <= 0 || mesh->nv <= 0) return;
    if (ndraws_ >= draws_cap_) {
        int nc = draws_cap_ ? draws_cap_ * 2 : 256;
        Draw* nd = new Draw[nc];
        for (int i = 0; i < ndraws_; i++) nd[i] = draws_[i];
        delete[] draws_;
        draws_ = nd;
        draws_cap_ = nc;
    }
    Draw& d = draws_[ndraws_++];
    d.mesh = mesh;
    d.model = model;
    d.flags = flags;
    d.mvp = (flags & OE_DRAW_OVERLAY) ? cam_.overlay_proj * model : vp_ * model;
    if (mesh->nv > max_nv_) max_nv_ = mesh->nv;
}

// --------------------------------------------------------------- проходы
namespace {
struct Ctx { Rasterizer* r; };
void geom_fn(void* c, int i, int t)  { ((Ctx*)c)->r->geometryJob(i, t); }
void count_fn(void* c, int i, int t) { ((Ctx*)c)->r->countJob(i, t); }
void fill_fn(void* c, int i, int t)  { ((Ctx*)c)->r->fillJob(i, t); }
void band_fn(void* c, int i, int t)  { ((Ctx*)c)->r->bandJob(i, t); }
}

void Rasterizer::end() {
    if (!in_frame_) return;
    in_frame_ = false;
    if (!fb_ || !depth_) return;
    if (!tsc_per_ms_) tsc_per_ms_ = ov_tsc_per_ms();
    uint64_t t0 = tsc();
    jobs_.reset_busy();

    // Всё, что задачи будут писать, готовим здесь: им нельзя выделять память
    int nth = jobs_.threads();
    if (!tris_) { tris_ = new STri[kTriCap]; tris_cap_ = kTriCap; }
    for (int i = 0; i < nth; i++) {
        ThreadState& ts = ts_[i];
        if (ts.scratch_cap < max_nv_) {
            delete[] ts.scratch;
            ts.scratch_cap = max_nv_ + 1024;
            ts.scratch = new CV[ts.scratch_cap];
        }
        ts.cur = ts.end = 0;
        ts.submitted = ts.drawn = ts.dropped = 0;
    }
    tri_top_ = 0;
    nbands_ = (h_ + kBand - 1) / kBand;
    int want_parts = nth * 2;
    if (want_parts != nparts_ || nbands_ > bands_cap_) {
        delete[] counts_;
        delete[] band_start_;
        nparts_ = want_parts;
        bands_cap_ = nbands_;
        counts_ = new uint32_t[(size_t)nparts_ * (size_t)bands_cap_];
        band_start_ = new uint32_t[bands_cap_ + 1];
    }

    Ctx ctx = { this };
    // 1. Геометрия
    jobs_.run(ndraws_, geom_fn, &ctx);
    uint32_t total = tri_top_ < tris_cap_ ? tri_top_ : tris_cap_;
    for (int i = 0; i < nth; i++)                          // недобранные хвосты блоков
        for (uint32_t k = ts_[i].cur; k < ts_[i].end && k < total; k++) tris_[k].valid = 0;

    // 2. Раскладка по полосам: подсчёт, смещения, номера
    for (int i = 0; i < nparts_ * nbands_; i++) counts_[i] = 0;
    jobs_.run(nparts_, count_fn, &ctx);
    uint32_t run = 0;
    for (int b = 0; b < nbands_; b++) {
        band_start_[b] = run;
        for (int p = 0; p < nparts_; p++) {
            uint32_t c = counts_[p * nbands_ + b];
            counts_[p * nbands_ + b] = run;
            run += c;
        }
    }
    band_start_[nbands_] = run;
    if (run > bins_cap_) {
        delete[] bins_;
        bins_cap_ = run + run / 4 + 1024;
        bins_ = new uint32_t[bins_cap_];
    }
    jobs_.run(nparts_, fill_fn, &ctx);
    uint64_t t1 = tsc();

    // 3. Полосы
    jobs_.run(nbands_, band_fn, &ctx);
    uint64_t t2 = tsc();

    FrameStats& s = stats_;
    s.threads = nth;
    s.submitted = s.drawn = s.dropped = 0;
    for (int i = 0; i < nth; i++) {
        s.submitted += ts_[i].submitted;
        s.drawn += ts_[i].drawn;
        s.dropped += ts_[i].dropped;
    }
    float k = tsc_per_ms_ ? 1.0f / (float)tsc_per_ms_ : 0.0f;
    s.ms_geometry = (float)(t1 - t0) * k;
    s.ms_raster = (float)(t2 - t1) * k;
    s.ms_total = (float)(t2 - t0) * k;
    for (int i = 0; i < OE_MAX_THREADS; i++)
        s.thread_busy[i] = (i < nth && t2 > t0) ? (float)jobs_.busy(i) / (float)(t2 - t0) : 0.0f;
}

// ------------------------------------------------------------ 1. геометрия
void Rasterizer::geometryJob(int di, int thread) {
    const Draw& d = draws_[di];
    const Mesh& m = *d.mesh;
    ThreadState& ts = ts_[thread];
    CV* cv = ts.scratch;
    ts.tri_flags = (d.flags & OE_DRAW_OVERLAY) ? (uint16_t)(1 | STRI_OVERLAY) : (uint16_t)1;
    for (int i = 0; i < m.nv; i++) {
        cv[i].p = d.mvp * Vec4(m.v[i].pos, 1.0f);
        cv[i].uv = m.v[i].uv;
        cv[i].s = m.v[i].shade;
    }
    const float amb = cam_.light.ambient;
    const bool lit = amb < 1.0f;
    for (int i = 0; i < m.nf; i++) {
        const Face& f = m.f[i];
        ts.submitted++;
        float k = 1.0f;
        if (lit) {
            Vec3 n = normalize(d.model.rotate(f.normal));
            float lam = dot(n, to_light_);
            if (lam < 0.0f) lam = 0.0f;
            k = amb + (1.0f - amb) * lam;
        }
        CV a = cv[f.a], b = cv[f.b], c = cv[f.c];
        a.s *= k; b.s *= k; c.s *= k;
        clipAndEmit(ts, a, b, c, f.tex < ntex_ ? f.tex : 0);
    }
}

// Отсечение по ближней плоскости (z >= -w в пространстве отсечения). Без него
// вершина за камерой даёт w <= 0, и деление выворачивает треугольник на весь
// экран. Остальные плоскости не нужны: экран и так обрезает растеризатор.
void Rasterizer::clipAndEmit(ThreadState& ts, const CV& a, const CV& b, const CV& c, int tex) {
    const CV* in[3] = { &a, &b, &c };
    float d[3];
    int inside = 0;
    for (int i = 0; i < 3; i++) {
        d[i] = in[i]->p.z + in[i]->p.w;
        if (d[i] >= 0.0f) inside++;
    }
    if (inside == 0) return;
    // Целиком за левым, правым, верхним или нижним краем — дальше не нужен
    if ((a.p.x > a.p.w && b.p.x > b.p.w && c.p.x > c.p.w) ||
        (a.p.x < -a.p.w && b.p.x < -b.p.w && c.p.x < -c.p.w) ||
        (a.p.y > a.p.w && b.p.y > b.p.w && c.p.y > c.p.w) ||
        (a.p.y < -a.p.w && b.p.y < -b.p.w && c.p.y < -c.p.w)) return;
    if (inside == 3) { emit(ts, a, b, c, tex); return; }

    CV out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        const CV& p = *in[i];
        const CV& q = *in[j];
        if (d[i] >= 0.0f) out[n++] = p;
        if ((d[i] >= 0.0f) != (d[j] >= 0.0f)) {
            float k = d[i] / (d[i] - d[j]);
            CV mid;
            mid.p = p.p + (q.p - p.p) * k;
            mid.uv = p.uv + (q.uv - p.uv) * k;
            mid.s = p.s + (q.s - p.s) * k;
            out[n++] = mid;
        }
    }
    for (int i = 1; i + 1 < n; i++) emit(ts, out[0], out[i], out[i + 1], tex);
}

// 1/n для кусков строки: деление в цикле стоило заметную долю кадра
static const float kRecip[17] = { 0.0f, 1.0f, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6, 1.0f / 7, 1.0f / 8,
                                  1.0f / 9, 1.0f / 10, 1.0f / 11, 1.0f / 12, 1.0f / 13, 1.0f / 14, 1.0f / 15, 1.0f / 16 };

// Тексель * яркость (S в 1/65536), затем туман к цвету неба (F в 1/65536 доли)
static inline uint32_t shade_fog(uint32_t c, int32_t S, int32_t F, int sr, int sg, int sb) {
    int s8 = S >> 8;
    if (s8 > 256) s8 = 256; else if (s8 < 0) s8 = 0;
    int r = (int)((((c >> 16) & 255) * (uint32_t)s8) >> 8);
    int g = (int)((((c >> 8) & 255) * (uint32_t)s8) >> 8);
    int b = (int)(((c & 255) * (uint32_t)s8) >> 8);
    int f = F >> 8;
    if (f > 0) {
        if (f > 256) f = 256;
        r += ((sr - r) * f) >> 8;
        g += ((sg - g) * f) >> 8;
        b += ((sb - b) * f) >> 8;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Один пиксель после прошедшей проверки z. Вырез — дырки не пишут ничего;
// смешивание — z не пишется (за стеклом видно то, что нарисовано позже),
// кроме совсем непрозрачных текселей вроде рамки стекла
template <int MODE>
static inline void put_pixel(uint32_t& dst, float& zdst, float z, uint32_t c, int32_t S, int32_t F, int sr, int sg, int sb) {
    if (MODE == TEX_OPAQUE) {
        zdst = z;
        dst = shade_fog(c, S, F, sr, sg, sb);
    } else if (MODE == TEX_CUTOUT) {
        if (c < 0x80000000u) return;
        zdst = z;
        dst = shade_fog(c, S, F, sr, sg, sb);
    } else {
        uint32_t a = c >> 24;
        if (!a) return;
        uint32_t s = shade_fog(c, S, F, sr, sg, sb);
        if (a == 255) { zdst = z; dst = s; return; }
        uint32_t d = dst;
        uint32_t rb = ((d & 0xFF00FF) * (255 - a) + (s & 0xFF00FF) * a) >> 8;
        uint32_t g = ((d & 0x00FF00) * (255 - a) + (s & 0x00FF00) * a) >> 8;
        dst = (rb & 0xFF00FF) | (g & 0x00FF00);
    }
}

static inline float edgef(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void Rasterizer::emit(ThreadState& ts, const CV& ca, const CV& cb, const CV& cc, int tex) {
    const CV* src[3] = { &ca, &cb, &cc };
    float x[3], y[3], iw[3];
    for (int i = 0; i < 3; i++) {
        iw[i] = 1.0f / src[i]->p.w;
        x[i] = (src[i]->p.x * iw[i] * 0.5f + 0.5f) * (float)w_;
        y[i] = (0.5f - src[i]->p.y * iw[i] * 0.5f) * (float)h_;   // экранная Y — вниз
    }
    // Лицевая грань (против часовой в модели) на экране с Y вниз идёт по
    // часовой — её площадь отрицательна. Положительная — задняя, её не видно.
    float area = edgef(x[0], y[0], x[1], y[1], x[2], y[2]);
    if (area >= 0.0f) return;
    float fy0 = fmin(y[0], fmin(y[1], y[2])), fy1 = fmax(y[0], fmax(y[1], y[2]));
    float fx0 = fmin(x[0], fmin(x[1], x[2])), fx1 = fmax(x[0], fmax(x[1], x[2]));
    if (fx1 < 0.0f || fx0 >= (float)w_ || fy1 < 0.0f || fy0 >= (float)h_) return;
    int y0 = (int)fy0, y1 = (int)fy1 + 1;
    if (y0 < 0) y0 = 0;
    if (y1 > h_) y1 = h_;
    if (y0 >= y1) return;

    if (ts.cur >= ts.end) {
        uint32_t base = __atomic_fetch_add(&tri_top_, kBlock, __ATOMIC_RELAXED);
        if (base >= tris_cap_) { ts.dropped++; return; }
        ts.cur = base;
        ts.end = base + kBlock < tris_cap_ ? base + kBlock : tris_cap_;
    }
    STri& t = tris_[ts.cur++];
    static const int order[3] = { 0, 2, 1 };               // обход становится положительным
    for (int k = 0; k < 3; k++) {
        int i = order[k];
        t.x[k] = x[i];
        t.y[k] = y[i];
        t.iw[k] = iw[i];
        t.uw[k] = src[i]->uv.x * iw[i];
        t.vw[k] = src[i]->uv.y * iw[i];
        t.s[k] = src[i]->s;
    }
    t.tex = (uint16_t)tex;
    t.y0 = (int16_t)y0;
    t.y1 = (int16_t)y1;
    t.valid = ts.tri_flags;
    ts.drawn++;
}

// ------------------------------------------------------------ 2. раскладка
void Rasterizer::countJob(int part, int thread) {
    (void)thread;
    uint32_t total = tri_top_ < tris_cap_ ? tri_top_ : tris_cap_;
    uint32_t a = (uint32_t)((uint64_t)total * part / nparts_), b = (uint32_t)((uint64_t)total * (part + 1) / nparts_);
    uint32_t* cnt = counts_ + part * nbands_;
    for (uint32_t i = a; i < b; i++) {
        const STri& t = tris_[i];
        if (!t.valid) continue;
        int b0 = t.y0 / kBand, b1 = (t.y1 - 1) / kBand;
        for (int k = b0; k <= b1; k++) cnt[k]++;
    }
}

void Rasterizer::fillJob(int part, int thread) {
    (void)thread;
    uint32_t total = tri_top_ < tris_cap_ ? tri_top_ : tris_cap_;
    uint32_t a = (uint32_t)((uint64_t)total * part / nparts_), b = (uint32_t)((uint64_t)total * (part + 1) / nparts_);
    uint32_t* off = counts_ + part * nbands_;
    for (uint32_t i = a; i < b; i++) {
        const STri& t = tris_[i];
        if (!t.valid) continue;
        int b0 = t.y0 / kBand, b1 = (t.y1 - 1) / kBand;
        for (int k = b0; k <= b1; k++) bins_[off[k]++] = i;
    }
}

// ------------------------------------------------------------ 3. полосы
void Rasterizer::bandJob(int band, int thread) {
    (void)thread;
    int by0 = band * kBand, by1 = by0 + kBand;
    if (by1 > h_) by1 = h_;
    for (int y = by0; y < by1; y++) {
        uint32_t c = sky_[y];
        uint32_t* row = color_ + y * w_;
        float* z = depth_ + y * w_;
        for (int x = 0; x < w_; x++) { row[x] = c; z[x] = 0.0f; }   // в z-буфере 1/w: 0 — бесконечность
    }
    const uint32_t k0 = band_start_[band], k1 = band_start_[band + 1];
    bool any_overlay = false;
    for (uint32_t k = k0; k < k1 && !any_overlay; k++) any_overlay = (tris_[bins_[k]].valid & STRI_OVERLAY) != 0;
    rasterList(k0, k1, by0, by1, false, depth_);
    if (any_overlay) {
        // Рука поверх всего: у неё свой z (мир её уже не закроет), а в мировой
        // z, где она легла, пишем «ближе всего» — так рамка блока и прочее,
        // что игра сверяет с oe_depth, прячется и за руку. Обнулять мировой z
        // нельзя: по нему же рамка прячется за блоками на этих строках
        for (int y = by0; y < by1; y++) {
            float* z = odepth_ + y * w_;
            for (int x = 0; x < w_; x++) z[x] = 0.0f;
        }
        rasterList(k0, k1, by0, by1, true, odepth_);
        for (int y = by0; y < by1; y++) {
            const float* o = odepth_ + y * w_;
            float* z = depth_ + y * w_;
            for (int x = 0; x < w_; x++) if (o[x] > 0.0f) z[x] = 1e30f;
        }
    }

    if (pct_ < 100) {
        // Строки окна, которые берут цвет из строк этой полосы: [ceil(by0*H/h), ceil(by1*H/h))
        int fy0 = (by0 * fbh_ + h_ - 1) / h_, fy1 = (by1 * fbh_ + h_ - 1) / h_;
        if (fy1 > fbh_) fy1 = fbh_;
        for (int fy = fy0; fy < fy1; fy++) {
            const uint32_t* src = color_ + (fy * h_ / fbh_) * w_;
            uint32_t* dst = fb_ + fy * fbw_;
            for (int x = 0; x < fbw_; x++) dst[x] = src[xmap_[x]];
        }
    }
}

// Треугольники полосы одного слоя (мир или рука). Сначала всё, что пишет z
// (непрозрачное и с вырезами), потом полупрозрачное поверх — в обратном
// порядке: игра отдаёт сетки от ближних к дальним, а смешивать нужно от
// дальних к ближним
void Rasterizer::rasterList(uint32_t k0, uint32_t k1, int by0, int by1, bool overlay, float* zbuf) {
    const uint16_t want = overlay ? STRI_OVERLAY : 0;
    bool any_blend = false;
    for (uint32_t k = k0; k < k1; k++) {
        const STri& t = tris_[bins_[k]];
        if ((t.valid & STRI_OVERLAY) != want) continue;
        switch (tex_[t.tex].mode) {
            case TEX_OPAQUE: rasterBand<TEX_OPAQUE>(t, by0, by1, zbuf); break;
            case TEX_CUTOUT: rasterBand<TEX_CUTOUT>(t, by0, by1, zbuf); break;
            default: any_blend = true; break;
        }
    }
    if (any_blend)
        for (uint32_t k = k1; k-- > k0;) {
            const STri& t = tris_[bins_[k]];
            if ((t.valid & STRI_OVERLAY) == want && tex_[t.tex].mode == TEX_BLEND) rasterBand<TEX_BLEND>(t, by0, by1, zbuf);
        }
}

template <int MODE>
void Rasterizer::rasterBand(const STri& t, int by0, int by1, float* zbuf) {
    int y0 = t.y0 > by0 ? t.y0 : by0, y1 = t.y1 < by1 ? t.y1 : by1;
    if (y0 >= y1) return;
    float fx0 = fmin(t.x[0], fmin(t.x[1], t.x[2])), fx1 = fmax(t.x[0], fmax(t.x[1], t.x[2]));
    int bx0 = (int)fx0, bx1 = (int)fx1 + 1;
    if (bx0 < 0) bx0 = 0;
    if (bx1 > w_) bx1 = w_;
    if (bx0 >= bx1) return;

    float area = edgef(t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2]);
    if (area <= 0.0f) return;
    const float ia = 1.0f / area;

    // Рёберные функции: e_i = вес вершины i * area; шаг по x — a_i, по y — b_i
    const float a[3] = { t.y[1] - t.y[2], t.y[2] - t.y[0], t.y[0] - t.y[1] };
    const float b[3] = { t.x[2] - t.x[1], t.x[0] - t.x[2], t.x[1] - t.x[0] };
    const float px = (float)bx0 + 0.5f, py = (float)y0 + 0.5f;
    // Обратные шаги рёбер: концы отрезка строки — умножением, а не делением
    // Концы отрезков строк — в double: рёберная функция крупного треугольника
    // доходит до 1e5 пикс², и во float её шаг по строкам копил ошибку до
    // сотых пикселя. На ребре, где сходятся грани разной длины (склеенные
    // greedy-прямоугольники), это давало сквозную щель в пиксель — небо
    // сквозь блоки
    double inva[3], erd[3];
    for (int k = 0; k < 3; k++) inva[k] = a[k] != 0.0f ? 1.0 / (double)a[k] : 0.0;
    float er[3] = { edgef(t.x[1], t.y[1], t.x[2], t.y[2], px, py),
                    edgef(t.x[2], t.y[2], t.x[0], t.y[0], px, py),
                    edgef(t.x[0], t.y[0], t.x[1], t.y[1], px, py) };
    for (int k = 0; k < 3; k++) {
        const int i = (k + 1) % 3, j = (k + 2) % 3;          // ребро напротив вершины k
        erd[k] = ((double)t.x[j] - t.x[i]) * ((double)py - t.y[i]) - ((double)t.y[j] - t.y[i]) * ((double)px - t.x[i]);
    }

    // Градиенты атрибутов по экрану и значение в (px, py)
    struct Grad { float v, dx, dy; };
    auto grad = [&](const float q[3]) {
        Grad g;
        g.dx = (a[0] * q[0] + a[1] * q[1] + a[2] * q[2]) * ia;
        g.dy = (b[0] * q[0] + b[1] * q[1] + b[2] * q[2]) * ia;
        g.v  = (er[0] * q[0] + er[1] * q[1] + er[2] * q[2]) * ia;
        return g;
    };
    Grad giw = grad(t.iw), guw = grad(t.uw), gvw = grad(t.vw), gs = grad(t.s);

    const Texture& tx = tex_[t.tex];
    const int tmx = tx.w - 1, tmy = tx.h - 1, tsh = tx.sh;
    const float ftw = (float)tx.w * 65536.0f, fth = (float)tx.h * 65536.0f;
    const uint32_t* tp = tx.px;
    const float fs = cam_.fog_start;
    const float finv = cam_.fog_end > cam_.fog_start ? 256.0f / (cam_.fog_end - cam_.fog_start) : 0.0f;
    const bool fog_on = finv > 0.0f;

    // Дальний (или стоящий лицом к камере) треугольник: w по нему почти не
    // меняется, и перспективная поправка незаметна. Тогда u, v и туман
    // линейны по всему треугольнику — ни одного деления на строку. Таких
    // треугольников большинство, а строки у них короткие.
    // Ошибка аффинной выборки растёт с числом текселей на треугольнике: на
    // склеенной грани текстура повторяется до 16 раз, и те же 8 % разброса w
    // сдвинули бы её на несколько текселей. Поэтому порог — по размаху w,
    // умноженному на размах текстуры в текселях: не больше полутекселя.
    float iwmin = fmin(t.iw[0], fmin(t.iw[1], t.iw[2])), iwmax = fmax(t.iw[0], fmax(t.iw[1], t.iw[2]));
    bool affine = false;
    if (iwmax < iwmin * 1.08f) {
        float span = 0.0f;
        for (int k = 0; k < 3; k++) {
            float wk = 1.0f / t.iw[k];
            float du = (t.uw[k] * wk - t.uw[0] / t.iw[0]) * (float)tx.w, dv = (t.vw[k] * wk - t.vw[0] / t.iw[0]) * (float)tx.h;
            span = fmax(span, fmax(du < 0 ? -du : du, dv < 0 ? -dv : dv));
        }
        affine = (iwmax - iwmin) * span < 0.5f * iwmin;
    }
    Grad gu = {}, gv = {}, gf = {};
    if (affine) {
        float uq[3], vq[3], fq[3];
        for (int k = 0; k < 3; k++) {
            float wk = 1.0f / t.iw[k];
            uq[k] = t.uw[k] * wk * ftw;
            vq[k] = t.vw[k] * wk * fth;
            fq[k] = fog_on ? (wk - fs) * finv * 256.0f : 0.0f;
        }
        gu = grad(uq); gv = grad(vq); gf = grad(fq);
    }

    for (int y = y0; y < y1; y++) {
        // Отрезок строки: для каждого ребра — с какого x функция >= 0 (a > 0)
        // или до какого (a < 0)
        float xs = (float)bx0, xe = (float)bx1;
        bool empty = false;
        for (int k = 0; k < 3; k++) {
            double e = erd[k];
            float ak = a[k];
            if (ak > 0.0f) { float x = (float)((double)bx0 - e * inva[k]) - 2e-3f; if (x > xs) xs = x; }   // запас: без щелей на общих рёбрах
            else if (ak < 0.0f) { float x = (float)((double)bx0 - e * inva[k]) + 2e-3f; if (x + 1.0f < xe) xe = x + 1.0f; }
            else if (e < 0.0f) { empty = true; break; }
        }
        int ix0 = 0, ix1 = 0;
        if (!empty) {
            ix0 = (int)xs;
            if ((float)ix0 < xs) ix0++;                    // первый центр пикселя внутри
            ix1 = (int)xe;
            if (ix0 < bx0) ix0 = bx0;
            if (ix1 > bx1) ix1 = bx1;
        }
        if (ix0 < ix1) {
            float off = (float)(ix0 - bx0);
            float iw = giw.v + giw.dx * off, uw = guw.v + guw.dx * off, vw = gvw.v + gvw.dx * off;
            float sh = gs.v + gs.dx * off;
            uint32_t* frow = color_ + y * w_;
            float* zrow = zbuf + y * w_;
            const uint32_t sky = sky_[y];
            const int sr = (sky >> 16) & 255, sg = (sky >> 8) & 255, sb = sky & 255;

            if (affine) {
                // u/v — 16.16 по модулю 2^32: важны только младшие биты (wrap
                // текстуры), а перенос у беззнаковых определён, в отличие от int
                uint32_t U = (uint32_t)(int32_t)(gu.v + gu.dx * off), V = (uint32_t)(int32_t)(gv.v + gv.dx * off);
                uint32_t dU = (uint32_t)(int32_t)gu.dx, dV = (uint32_t)(int32_t)gv.dx;
                int32_t F = (int32_t)(gf.v + gf.dx * off), dF = (int32_t)gf.dx;
                int32_t S = (int32_t)(sh * 65536.0f), dS = (int32_t)(gs.dx * 65536.0f);
                float z = iw;
                const float dz = giw.dx;
                for (int x = ix0; x < ix1; x++) {
                    if (z > zrow[x]) put_pixel<MODE>(frow[x], zrow[x], z, tp[(((V >> 16) & tmy) << tsh) + ((U >> 16) & tmx)], S, F, sr, sg, sb);
                    z += dz;
                    U += dU; V += dV; F += dF; S += dS;
                }
                ix0 = ix1;                                 // перспективный цикл ниже не нужен
            }
            float w0 = 1.0f / iw;
            int x = ix0;
            while (x < ix1) {
                int n = ix1 - x;
                if (n > 16) n = 16;
                // Концы куска: здесь честная перспектива
                float iw1 = iw + giw.dx * (float)n;
                float w1 = 1.0f / iw1;
                float u0 = uw * w0, v0 = vw * w0;
                float u1 = (uw + guw.dx * (float)n) * w1, v1 = (vw + gvw.dx * (float)n) * w1;
                const float rn = kRecip[n];
                uint32_t U = (uint32_t)(int32_t)(u0 * ftw), V = (uint32_t)(int32_t)(v0 * fth);
                uint32_t dU = (uint32_t)(int32_t)((u1 - u0) * ftw * rn), dV = (uint32_t)(int32_t)((v1 - v0) * fth * rn);
                // Туман и яркость — тоже линейно по куску, в 1/256
                int32_t F = 0, dF = 0;
                if (fog_on) {
                    float f0 = (w0 - fs) * finv, f1 = (w1 - fs) * finv;
                    F = (int32_t)(f0 * 256.0f);
                    dF = (int32_t)((f1 - f0) * 256.0f * rn);
                }
                int32_t S = (int32_t)(sh * 65536.0f), dS = (int32_t)(gs.dx * 65536.0f);
                float z = iw;
                const float dz = giw.dx;
                for (int i = 0; i < n; i++, x++) {
                    if (z > zrow[x]) put_pixel<MODE>(frow[x], zrow[x], z, tp[(((V >> 16) & tmy) << tsh) + ((U >> 16) & tmx)], S, F, sr, sg, sb);
                    z += dz;
                    U += dU; V += dV; F += dF; S += dS;
                }
                iw = iw1;
                uw += guw.dx * (float)n;
                vw += gvw.dx * (float)n;
                sh += gs.dx * (float)n;
                w0 = w1;
            }
        }
        for (int k = 0; k < 3; k++) erd[k] += (double)b[k];
        giw.v += giw.dy; guw.v += guw.dy; gvw.v += gvw.dy; gs.v += gs.dy;
        gu.v += gu.dy; gv.v += gv.dy; gf.v += gf.dy;        // без этого каждая строка начиналась с того же текселя
    }
}

#ifdef OE_DEBUG
}
#include <stdio.h>
namespace ov {
void Rasterizer::debugPixel(int x, int y) const {
    int band = y / kBand;
    for (uint32_t k = band_start_[band]; k < band_start_[band + 1]; k++) {
        const STri& t = tris_[bins_[k]];
        float fx0 = fmin(t.x[0], fmin(t.x[1], t.x[2])), fx1 = fmax(t.x[0], fmax(t.x[1], t.x[2]));
        if (fx1 < x - 2 || fx0 > x + 3 || y < t.y0 || y >= t.y1) continue;
        double px = x + 0.5, py = y + 0.5, e[3];
        for (int q = 0; q < 3; q++) { int i = (q + 1) % 3, j = (q + 2) % 3;
            e[q] = ((double)t.x[j] - t.x[i]) * (py - t.y[i]) - ((double)t.y[j] - t.y[i]) * (px - t.x[i]); }
        printf("tri %u tex %d: (%.4f,%.4f) (%.4f,%.4f) (%.4f,%.4f)  e=%.4f %.4f %.4f iw=%.5f\n", bins_[k], t.tex,
               t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2], e[0], e[1], e[2], t.iw[0]);
    }
}
#endif
} // namespace ov
