// Заглушка ovengine_ll.h для стенда: ov::Renderer прямо поверх Rasterizer,
// без .ll и без рабочих ядер; текстуры — тем же декодером PNG
#pragma once
#include <stdio.h>
#include "ovos.h"
#include "renderer.h"
#include "libgfx/png.h"

__attribute__((weak)) char g_oe_dir[96];

namespace ov {
class Renderer {
public:
    explicit Renderer(int = -1) { r_.startThreads(0); }
    bool ok() const { return true; }
    int threads() const { return 1; }
    bool setTarget(uint32_t* fb, int w, int h, int pct = 100) { return r_.setTarget(fb, w, h, pct); }
    int loadTexture(const char* rel) {
        char path[256];
        snprintf(path, sizeof path, "%s/%s", g_oe_dir, rel);
        static uint8_t file[1 << 20], scratch[1 << 20];
        FILE* f = fopen(path, "rb");
        if (!f) return OE_ERR_OPEN;
        size_t n = fread(file, 1, sizeof file, f);
        fclose(f);
        int w = (int)png_be32(file + 16), h = (int)png_be32(file + 20);
        uint32_t* px = new uint32_t[w * h];
        png_info_t info;
        if (png_decode(file, (uint32_t)n, px, (uint32_t)(w * h), scratch, sizeof scratch, &info) != PNG_OK) return OE_ERR_READ;
        return r_.addTexture(w, h, px, true);
    }
    int addTexture(int w, int h, const uint32_t* px) {
        uint32_t* own = new uint32_t[w * h];
        for (int i = 0; i < w * h; i++) own[i] = px[i] & 0xFFFFFF;
        return r_.addTexture(w, h, own);
    }
    uint32_t textureColor(int t) const { return r_.textureColor(t); }
    const uint32_t* texturePixels(int t, int* w, int* h) const { return r_.texturePixels(t, w, h); }
    const float* depth(int* w, int* h) const { return r_.depthBuffer(w, h); }
    void begin(const Camera& c) { r_.begin(c); }
    void draw(const Mesh& m, const Mat4& model) { r_.draw(&m, model); }
    void drawOverlay(const Mesh& m, const Mat4& model) { r_.draw(&m, model, OE_DRAW_OVERLAY); }
    void end() { r_.end(); }
#ifdef OE_DEBUG
    void debugPixel(int x, int y) const { r_.debugPixel(x, y); }
#endif
    FrameStats stats() const { return r_.stats(); }
    void parallel(int count, JobFn fn, void* ctx) { r_.jobs().run(count, fn, ctx); }
    static const char* errorText(int) { return "ошибка"; }
    static void memory(uint64_t* u, uint64_t* t, uint64_t* l) { if (u) *u = 0; if (t) *t = 0; if (l) *l = 0; }
private:
    Rasterizer r_;
};
}
