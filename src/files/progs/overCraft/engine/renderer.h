#ifndef OVENGINE_RENDERER_H
#define OVENGINE_RENDERER_H
// =============================================================================
// renderer.h — программный растеризатор, внутренность ovengine.ll.
// Игре этот файл не виден: она работает через oe_* (ovengine.h).
//
// Видеокарты у нас нет, поэтому весь конвейер — на процессоре, как в Quake,
// только на всех ядрах. Кадр идёт тремя параллельными проходами:
//
//   1. геометрия (задача = одна сетка): вершины через матрицы, отсечение по
//      ближней плоскости, деление на w, отбрасывание задних граней. Готовые
//      экранные треугольники кладутся в общий буфер кадра; место в нём поток
//      берёт блоками по 64 одной атомарной операцией.
//   2. раскладка (задача = кусок буфера): какие треугольники задевают какую
//      полосу экрана (16 строк). Сначала подсчёт, потом основной поток
//      расставляет смещения, потом каждый кусок пишет свои номера.
//   3. растеризация (задача = полоса): небо и z-буфер полосы, затем все её
//      треугольники. Полосы не пересекаются, поэтому потоки не мешают друг
//      другу и блокировок нет.
//
// Текстура интерполируется с поправкой на перспективу (u/w, v/w, 1/w), иначе
// на гранях, уходящих вглубь, она «плывёт». Выборка — ближайший тексель и
// повтор (UV больше 1): пиксельный вид блоков, как в Minecraft. Яркость
// вершин (затенение углов) — линейно по экрану, туман — по расстоянию к
// цвету неба той же строки.
// =============================================================================

#include <stdint.h>
#include "ovengine.h"
#include "jobs.h"

namespace ov {

struct Texture {
    int w = 0, h = 0;               // степени двойки
    int sh = 0;                     // log2(w): строка текстуры — сдвигом
    uint32_t* px = nullptr;         // 0xAARRGGBB, принадлежит движку
    uint32_t avg = 0;               // средний цвет
    int mode = 0;                   // TEX_OPAQUE / TEX_CUTOUT / TEX_BLEND
};

// Как текстура ложится на экран — по её альфа-каналу:
enum {
    TEX_OPAQUE = 0,                 // альфы нет: обычная выборка
    TEX_CUTOUT = 1,                 // альфа только 0/255: дырки, z пишется (листва, решётки)
    TEX_BLEND  = 2,                 // промежуточная альфа: смешивание вторым проходом (вода, стекло)
};

// Экранный треугольник после геометрии (80 байт)
struct STri {
    float x[3], y[3];               // пиксели внутреннего буфера
    float iw[3];                    // 1/w
    float uw[3], vw[3];             // u/w, v/w
    float s[3];                     // яркость вершины * освещение грани (0..1)
    uint16_t tex;
    int16_t y0, y1;                 // занятые строки [y0, y1)
    uint16_t valid;                 // 0 — пусто, иначе 1 | STRI_OVERLAY
};
enum { STRI_OVERLAY = 2 };

class Rasterizer {
public:
    Rasterizer() = default;
    ~Rasterizer();
    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;

    void startThreads(int max) { jobs_.start(max); }
    JobPool& jobs() { return jobs_; }

    bool setTarget(uint32_t* fb, int w, int h, int percent);
    // with_alpha = false — альфа пикселей не смотрится (0xRRGGBB), текстура непрозрачна
    int addTexture(int w, int h, uint32_t* px_owned, bool with_alpha = false);
    uint32_t textureColor(int t) const { return (t >= 0 && t < ntex_) ? tex_[t].avg : 0; }
    const float* depthBuffer(int* w, int* h) const {
        if (w) *w = w_;
        if (h) *h = h_;
        return depth_;
    }
    const uint32_t* texturePixels(int t, int* w, int* h) const {
        if (t < 0 || t >= ntex_) return nullptr;
        if (w) *w = tex_[t].w;
        if (h) *h = tex_[t].h;
        return tex_[t].px;
    }

    void begin(const Camera& cam);
    void draw(const Mesh* mesh, const Mat4& model, int flags = 0);
    void end();
    const FrameStats& stats() const { return stats_; }
#ifdef OE_DEBUG
    void debugPixel(int x, int y) const;    // стенд: какие треугольники лежат на пикселе
#endif

    // Задачи проходов (вызываются из JobPool)
    void geometryJob(int draw, int thread);
    void countJob(int part, int thread);
    void fillJob(int part, int thread);
    void bandJob(int band, int thread);

private:
    struct CV {                                   // вершина в пространстве отсечения
        Vec4 p;
        Vec2 uv;
        float s;
    };
    struct Draw {
        const Mesh* mesh;
        Mat4 mvp;
        Mat4 model;
        int flags;
    };
    struct ThreadState {                          // место в буфере кадра у потока
        uint32_t cur = 0, end = 0;
        CV* scratch = nullptr;
        int scratch_cap = 0;
        int submitted = 0, drawn = 0, dropped = 0;
        uint16_t tri_flags = 1;                   // valid для треугольников текущей сетки
    };

    void clipAndEmit(ThreadState& ts, const CV& a, const CV& b, const CV& c, int tex);
    void emit(ThreadState& ts, const CV& a, const CV& b, const CV& c, int tex);
    template <int MODE> void rasterBand(const STri& t, int by0, int by1, float* zbuf);
    void rasterList(uint32_t k0, uint32_t k1, int by0, int by1, bool overlay, float* zbuf);

    // Цель
    uint32_t* fb_ = nullptr;                      // окно
    int fbw_ = 0, fbh_ = 0;
    uint32_t* color_ = nullptr;                   // куда рисуем: окно или own_color_
    uint32_t* own_color_ = nullptr;               // внутренний буфер (разрешение меньше 100%)
    int* xmap_ = nullptr;                         // столбец окна -> столбец буфера
    int xmap_cap_ = 0;
    float* depth_ = nullptr;
    float* odepth_ = nullptr;                     // z руки (OE_DRAW_OVERLAY): мировой не трогаем
    int w_ = 0, h_ = 0, pct_ = 100, cap_ = 0;
    uint32_t* sky_ = nullptr;                     // цвет неба по строкам
    int sky_cap_ = 0;

    Texture tex_[OE_MAX_TEXTURES];
    int ntex_ = 0;

    // Кадр
    Camera cam_;
    Mat4 vp_;
    Vec3 to_light_;
    Draw* draws_ = nullptr;
    int ndraws_ = 0, draws_cap_ = 0, max_nv_ = 0;
    STri* tris_ = nullptr;
    uint32_t tris_cap_ = 0;
    volatile uint32_t tri_top_ = 0;
    uint32_t* bins_ = nullptr;                    // номера треугольников по полосам подряд
    uint32_t bins_cap_ = 0;
    int nbands_ = 0, nparts_ = 0, bands_cap_ = 0;
    uint32_t* counts_ = nullptr;                  // [part][band], потом смещения
    uint32_t* band_start_ = nullptr;              // [band + 1]
    ThreadState ts_[OE_MAX_THREADS];
    JobPool jobs_;
    FrameStats stats_ = {};
    uint64_t tsc_per_ms_ = 0;
    bool in_frame_ = false;
};

} // namespace ov

#endif
