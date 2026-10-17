// =============================================================================
// icons.cpp — значки блоков и предметов (см. icons.h).
// =============================================================================

#include "icons.h"
#include "items.h"
#include "libui/libui_ll.h"

namespace icons {

namespace {

ov::Renderer* g_r = nullptr;

struct Icon {
    uint16_t id = 0;
    int size = 0;
    uint32_t* px = nullptr;         // 0 — прозрачно (цвет никогда не равен нулю: см. ниже)
};
const int CACHE = 96;
Icon g_cache[CACHE];
int g_next = 0;

uint32_t shade(uint32_t c, int k) {           // k/256
    uint32_t r = (((c >> 16) & 255) * (uint32_t)k) >> 8;
    uint32_t g = (((c >> 8) & 255) * (uint32_t)k) >> 8;
    uint32_t b = ((c & 255) * (uint32_t)k) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

uint32_t texel(int tex, float u, float v) {
    int w = 0, h = 0;
    const uint32_t* px = g_r->texturePixels(tex, &w, &h);
    if (!px) return 0xFFFF00FFu;
    int x = (int)(u * (float)w), y = (int)(v * (float)h);
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    return px[y * w + x];
}

// Кубик: углы проекции (ось Y вниз), грань — начало и два ребра
void build_block(Icon& ic, uint8_t b) {
    const int N = ic.size;
    const BlockInfo& bi = BLOCKS[b];
    // Куб с ребром 1: полуширина 0.707, полувысота ромба верха 0.354,
    // вертикальное ребро 0.866 — вписываем всё в 0.95 высоты значка
    const float k = 0.95f * (float)N / 1.573f;
    const float hw = 0.7071f * k, q = 0.3536f * k, e = 0.866f * k;
    const float cx = (float)N * 0.5f, top = ((float)N - (2.0f * q + e)) * 0.5f;
    struct V { float x, y; };
    const V T = { cx, top }, L = { cx - hw, top + q }, R = { cx + hw, top + q }, C = { cx, top + 2 * q };
    const V BL = { cx - hw, top + q + e }, B = { cx, top + 2 * q + e };
    struct Face { V o, a, b; int tex; int light; };
    const Face faces[3] = {
        { L, { T.x - L.x, T.y - L.y }, { C.x - L.x, C.y - L.y }, bi.top, 256 },        // верх
        { L, { C.x - L.x, C.y - L.y }, { BL.x - L.x, BL.y - L.y }, bi.side, 204 },     // левая сторона
        { C, { R.x - C.x, R.y - C.y }, { B.x - C.x, B.y - C.y }, bi.side, 150 },       // правая
    };
    // Стекло и листва — по альфе текстуры: прозрачные пиксели значка пусты
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            uint32_t out = 0;
            for (int f = 0; f < 3 && !out; f++) {
                const Face& fc = faces[f];
                float det = fc.a.x * fc.b.y - fc.a.y * fc.b.x;
                float dx = px - fc.o.x, dy = py - fc.o.y;
                float u = (dx * fc.b.y - dy * fc.b.x) / det;
                float v = (fc.a.x * dy - fc.a.y * dx) / det;
                if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) continue;
                uint32_t c = texel(fc.tex, u, v);
                uint32_t a = c >> 24;
                if (a < 40) { out = 1; break; }              // дырка в грани: дальше не ищем
                out = shade(c, fc.light);
            }
            ic.px[y * N + x] = out == 1 ? 0 : out;
        }
}

void build_item(Icon& ic, uint16_t id) {
    const int N = ic.size;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            uint32_t c = texel(item_texture(id), ((float)x + 0.5f) / (float)N, ((float)y + 0.5f) / (float)N);
            ic.px[y * N + x] = (c >> 24) >= 128 ? (0xFF000000u | c) : 0;
        }
}

const Icon* get(uint16_t id, int size) {
    for (int i = 0; i < CACHE; i++)
        if (g_cache[i].id == id && g_cache[i].size == size) return &g_cache[i];
    Icon& ic = g_cache[g_next];
    g_next = (g_next + 1) % CACHE;
    if (ic.size != size) { delete[] ic.px; ic.px = new uint32_t[size * size]; }
    ic.id = id;
    ic.size = size;
    if (item_is_block(id)) build_block(ic, (uint8_t)id);
    else build_item(ic, id);
    return &ic;
}

} // namespace

void init(ov::Renderer* r) { g_r = r; }

void draw(ui_win_t* w, int x, int y, int size, uint16_t id) {
    if (!g_r || !item_valid(id) || size <= 0) return;
    const Icon* ic = get(id, size);
    uint32_t* fb = ui_pixels(w);
    int W = ui_width(w), H = ui_height(w);
    for (int j = 0; j < size; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= H) continue;
        const uint32_t* src = ic->px + j * size;
        uint32_t* dst = fb + yy * W;
        for (int i = 0; i < size; i++) {
            int xx = x + i;
            if (src[i] && xx >= 0 && xx < W) dst[xx] = src[i] & 0xFFFFFF;
        }
    }
}

void slot(ui_win_t* w, int x, int y, int size, uint16_t id, int count) {
    if (!item_valid(id) || count <= 0) return;
    draw(w, x, y, size, id);
    if (count > 1) {
        char t[8];
        ui_itoa(count, t);
        int tw = ui_text_width(t);
        int tx = x + size - tw + 1, ty = y + size - UI_FONT_H + 3;
        ui_text(w, tx + 1, ty + 1, t, 0x3A3A3A);
        ui_text(w, tx, ty, t, 0xFFFFFF);
    }
}

} // namespace icons
