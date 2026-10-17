// =============================================================================
// api.cpp — то, что ovengine.ll отдаёт наружу: функции oe_* (привязка C).
//
// Внутри — C++ (Rasterizer), снаружи — таблица функций: у .ll нет импорта и
// нет C++-имён в таблице символов, поэтому граница проходит по C.
// Память движка — своя арена (cxxrt с CXXRT_ARENA_KB), с кучей игры она не
// пересекается: brk принадлежит программе.
// =============================================================================

#include "ovengine.h"
#include "renderer.h"
#include "cxxrt/cxxrt.h"
#include "ovos.h"
#include "libgfx/png.h"

struct oe_renderer {
    ov::Rasterizer r;
};

static int is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

extern "C" {

int oe_version(void) { return OE_VERSION; }

oe_renderer* oe_create(int max_threads) {
    oe_renderer* r = new oe_renderer();
    r->r.startThreads(max_threads);
    return r;
}

void oe_destroy(oe_renderer* r) { delete r; }

int oe_threads(const oe_renderer* r) { return r ? const_cast<oe_renderer*>(r)->r.jobs().threads() : 1; }

int oe_set_target(oe_renderer* r, uint32_t* fb, int w, int h, int percent) {
    if (!r || !fb || w <= 0 || h <= 0) return 0;
    return r->r.setTarget(fb, w, h, percent) ? 1 : 0;
}

int oe_texture_add(oe_renderer* r, int w, int h, const uint32_t* px) {
    if (!r || !px) return OE_ERR_OPEN;
    if (!is_pow2(w) || !is_pow2(h) || w > 2048 || h > 2048) return OE_ERR_SIZE;
    uint32_t* own = new uint32_t[(size_t)w * (size_t)h];
    if (!own) return OE_ERR_MEM;
    for (int i = 0; i < w * h; i++) own[i] = px[i] & 0xFFFFFF;
    return r->r.addTexture(w, h, own);
}

uint32_t oe_texture_color(const oe_renderer* r, int tex) { return r ? r->r.textureColor(tex) : 0; }

void oe_memory(uint64_t* used, uint64_t* total, uint64_t* largest) {
    if (used) *used = ov_heap_used();
    if (total) *total = (uint64_t)CXXRT_ARENA_KB * 1024;
    if (largest) *largest = ov_heap_largest_free();
}

const float* oe_depth(const oe_renderer* r, int* w, int* h) {
    if (!r) return nullptr;
    return r->r.depthBuffer(w, h);
}

const uint32_t* oe_texture_pixels(const oe_renderer* r, int tex, int* w, int* h) {
    if (!r) return nullptr;
    return r->r.texturePixels(tex, w, h);
}

void oe_begin(oe_renderer* r, const ov::Camera* cam) { if (r && cam) r->r.begin(*cam); }

void oe_draw(oe_renderer* r, const ov::Mesh* mesh, const ov::Mat4* model) {
    if (r && mesh && model) r->r.draw(mesh, *model);
}

void oe_draw_ex(oe_renderer* r, const ov::Mesh* mesh, const ov::Mat4* model, int flags) {
    if (r && mesh && model) r->r.draw(mesh, *model, flags);
}

void oe_end(oe_renderer* r) { if (r) r->r.end(); }

void oe_stats(const oe_renderer* r, ov::FrameStats* out) { if (r && out) *out = r->r.stats(); }

void oe_parallel(oe_renderer* r, int count, ov::JobFn fn, void* ctx) {
    if (!r || !fn) return;
    r->r.jobs().run(count, fn, ctx);
}

// Текстура из PNG. Размеры — степени двойки: так выборка обходится без деления
int oe_texture_load(oe_renderer* r, const char* path) {
    if (!r || !path) return OE_ERR_OPEN;
    int fd = ov_open(path, 0);
    if (fd < 0) return OE_ERR_OPEN;
    int64_t size = ov_fsize(fd);
    if (size < 33 || size > 16 * 1024 * 1024) { ov_close(fd); return OE_ERR_READ; }
    uint8_t* file = (uint8_t*)ov_malloc((size_t)size);
    if (!file) { ov_close(fd); return OE_ERR_MEM; }
    int64_t got = 0;
    while (got < size) {
        int64_t n = ov_read(fd, file + got, (uint64_t)(size - got));
        if (n <= 0) break;
        got += n;
    }
    ov_close(fd);
    if (got != size) { ov_free(file); return OE_ERR_READ; }

    // Ширина и высота — в IHDR сразу за подписью (big-endian), до распаковки:
    // по ним и выделяется память
    int w = (int)png_be32(file + 16), h = (int)png_be32(file + 20);
    if (!is_pow2(w) || !is_pow2(h) || w > 2048 || h > 2048) { ov_free(file); return OE_ERR_SIZE; }
    uint32_t px_cap = (uint32_t)w * (uint32_t)h;
    uint32_t scratch_cap = (uint32_t)h * (1u + (uint32_t)w * 8u);  // худший случай: RGBA 16 бит
    uint32_t* px = new uint32_t[px_cap];
    uint8_t* scratch = (uint8_t*)ov_malloc(scratch_cap);
    if (!px || !scratch) { delete[] px; ov_free(scratch); ov_free(file); return OE_ERR_MEM; }

    png_info_t info;
    int rc = png_decode(file, (uint32_t)size, px, px_cap, scratch, scratch_cap, &info);
    ov_free(scratch);
    ov_free(file);
    if (rc != PNG_OK) { delete[] px; return rc; }
    return r->r.addTexture(w, h, px, true);     // режим (вырез, смешивание) — по альфе PNG
}

const char* oe_error_text(int code) {
    switch (code) {
        case OE_ERR_OPEN:       return "файл не открылся";
        case OE_ERR_READ:       return "файл не прочитался";
        case OE_ERR_SIZE:       return "размер не степень двойки или больше 2048";
        case OE_ERR_MEM:        return "не хватило памяти движка";
        case OE_ERR_FULL:       return "слишком много текстур";
        case PNG_ERR_SIG:       return "это не PNG";
        case PNG_ERR_TRUNC:     return "PNG обрывается";
        case PNG_ERR_FORMAT:    return "неподдержанный формат PNG";
        case PNG_ERR_INTERLACE: return "чересстрочный PNG не поддержан";
        default:                return "ошибка PNG";
    }
}

} // extern "C"
