// =============================================================================
// shapes.cpp — сетки кубиков, предметов и трещин (см. shapes.h).
// =============================================================================

#include "shapes.h"
#include "items.h"

using ov::Vec2;
using ov::Vec3;

namespace shapes {

namespace {

// Сетка, собираемая по четырёхугольникам
struct Builder {
    ov::Vertex* v = nullptr;
    ov::Face* f = nullptr;
    int nv = 0, nf = 0, cap = 0;

    void reserve(int quads) {
        cap = quads;
        v = new ov::Vertex[quads * 4];
        f = new ov::Face[quads * 2];
    }
    // Углы bl, br, tr, tl — против часовой, если смотреть снаружи;
    // (u0, v0) — у tl, (u1, v1) — у br
    void quad(Vec3 bl, Vec3 br, Vec3 tr, Vec3 tl, float u0, float v0, float u1, float v1, float shade, int tex, Vec3 n) {
        if (nf / 2 >= cap) return;
        int b = nv;
        const Vec3 q[4] = { bl, br, tr, tl };
        const Vec2 uv[4] = { Vec2(u0, v1), Vec2(u1, v1), Vec2(u1, v0), Vec2(u0, v0) };
        for (int i = 0; i < 4; i++) { v[nv].pos = q[i]; v[nv].uv = uv[i]; v[nv].shade = shade; nv++; }
        ov::Face t;
        t.tex = (uint16_t)tex;
        t.normal = n;
        t.a = (uint16_t)b; t.b = (uint16_t)(b + 1); t.c = (uint16_t)(b + 2); f[nf++] = t;
        t.a = (uint16_t)b; t.b = (uint16_t)(b + 2); t.c = (uint16_t)(b + 3); f[nf++] = t;
    }
    ov::Mesh mesh() const { ov::Mesh m; m.v = v; m.nv = nv; m.f = f; m.nf = nf; return m; }
};

// Куб [-s, s]^3: стороны — side (верх картинки — вверх), верх и низ — свои
ov::Mesh cube(float s, int top, int side, int bottom) {
    Builder b;
    b.reserve(6);
    const float a = -s, z = s;
    b.quad(Vec3(z, a, a), Vec3(a, a, a), Vec3(a, z, a), Vec3(z, z, a), 0, 0, 1, 1, 0.68f, side, Vec3(0, 0, -1));   // -Z
    b.quad(Vec3(a, a, z), Vec3(z, a, z), Vec3(z, z, z), Vec3(a, z, z), 0, 0, 1, 1, 0.68f, side, Vec3(0, 0, 1));    // +Z
    b.quad(Vec3(z, a, z), Vec3(z, a, a), Vec3(z, z, a), Vec3(z, z, z), 0, 0, 1, 1, 0.80f, side, Vec3(1, 0, 0));    // +X
    b.quad(Vec3(a, a, a), Vec3(a, a, z), Vec3(a, z, z), Vec3(a, z, a), 0, 0, 1, 1, 0.80f, side, Vec3(-1, 0, 0));   // -X
    b.quad(Vec3(a, z, z), Vec3(z, z, z), Vec3(z, z, a), Vec3(a, z, a), 0, 0, 1, 1, 1.00f, top, Vec3(0, 1, 0));     // +Y
    b.quad(Vec3(a, a, a), Vec3(z, a, a), Vec3(z, a, z), Vec3(a, a, z), 0, 0, 1, 1, 0.50f, bottom, Vec3(0, -1, 0)); // -Y
    return b.mesh();
}

// Выдавленный значок: пиксели с альфой — лицо и оборот целиком, бортик —
// там, где сосед прозрачен. Бортик берёт цвет своего пикселя (крошечный
// прямоугольник UV внутри текселя)
ov::Mesh extrude(const uint32_t* px, int w, int h, int tex) {
    int edges = 0;
    auto solid = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && (px[y * w + x] >> 24) >= 128; };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (solid(x, y)) edges += !solid(x + 1, y) + !solid(x - 1, y) + !solid(x, y + 1) + !solid(x, y - 1);
    Builder b;
    b.reserve(2 + edges);
    const float t = 0.5f / 16.0f;                     // половина толщины: пиксель
    const float zf = t, zb = -t;
    b.quad(Vec3(-0.5f, -0.5f, zf), Vec3(0.5f, -0.5f, zf), Vec3(0.5f, 0.5f, zf), Vec3(-0.5f, 0.5f, zf), 0, 0, 1, 1, 1.0f, tex, Vec3(0, 0, 1));
    b.quad(Vec3(0.5f, -0.5f, zb), Vec3(-0.5f, -0.5f, zb), Vec3(-0.5f, 0.5f, zb), Vec3(0.5f, 0.5f, zb), 1, 0, 0, 1, 0.8f, tex, Vec3(0, 0, -1));
    const float sx = 1.0f / (float)w, sy = 1.0f / (float)h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (!solid(x, y)) continue;
            float x0 = (float)x * sx - 0.5f, x1 = x0 + sx;
            float y1 = 0.5f - (float)y * sy, y0 = y1 - sy;
            float cu = ((float)x + 0.5f) * sx, cv = ((float)y + 0.5f) * sy, e = 0.1f * sx;
            if (!solid(x + 1, y)) b.quad(Vec3(x1, y0, zf), Vec3(x1, y0, zb), Vec3(x1, y1, zb), Vec3(x1, y1, zf), cu - e, cv - e, cu + e, cv + e, 0.75f, tex, Vec3(1, 0, 0));
            if (!solid(x - 1, y)) b.quad(Vec3(x0, y0, zb), Vec3(x0, y0, zf), Vec3(x0, y1, zf), Vec3(x0, y1, zb), cu - e, cv - e, cu + e, cv + e, 0.75f, tex, Vec3(-1, 0, 0));
            if (!solid(x, y - 1)) b.quad(Vec3(x0, y1, zf), Vec3(x1, y1, zf), Vec3(x1, y1, zb), Vec3(x0, y1, zb), cu - e, cv - e, cu + e, cv + e, 0.9f, tex, Vec3(0, 1, 0));
            if (!solid(x, y + 1)) b.quad(Vec3(x0, y0, zb), Vec3(x1, y0, zb), Vec3(x1, y0, zf), Vec3(x0, y0, zf), cu - e, cv - e, cu + e, cv + e, 0.6f, tex, Vec3(0, -1, 0));
        }
    return b.mesh();
}

ov::Mesh g_blocks[BLOCK_COUNT];
ov::Mesh g_items[ITEM_COUNT];
ov::Mesh g_cracks[10];
ov::Mesh g_empty;

} // namespace

bool init(ov::Renderer& r, int crack_tex0) {
    for (int i = 1; i < BLOCK_COUNT; i++) g_blocks[i] = cube(0.5f, BLOCKS[i].top, BLOCKS[i].side, BLOCKS[i].bottom);
    for (int i = 0; i < ITEM_COUNT; i++) {
        int w = 0, h = 0;
        const uint32_t* px = r.texturePixels(TEX_COUNT + i, &w, &h);
        if (!px) return false;
        g_items[i] = extrude(px, w, h, TEX_COUNT + i);
    }
    for (int k = 0; k < 10; k++) {
        int t = crack_tex0 + k;
        g_cracks[k] = cube(0.5f + 0.003f, t, t, t);
    }
    return true;
}

const ov::Mesh& block(uint8_t b) { return b > 0 && b < BLOCK_COUNT ? g_blocks[b] : g_empty; }
const ov::Mesh& item(uint16_t id) { return item_is_item(id) ? g_items[id - ITEM_FIRST] : g_empty; }
const ov::Mesh& held(uint16_t id) { return item_is_block(id) ? block((uint8_t)id) : item(id); }
const ov::Mesh& crack(int stage) { return g_cracks[stage < 0 ? 0 : (stage > 9 ? 9 : stage)]; }

} // namespace shapes
