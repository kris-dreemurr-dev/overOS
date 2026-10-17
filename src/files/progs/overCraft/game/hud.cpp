// =============================================================================
// hud.cpp — интерфейс поверх мира (см. hud.h).
// =============================================================================

#include "hud.h"
#include "blocks.h"
#include "items.h"
#include "config.h"
#include "game/icons.h"
#include "game/inventory.h"
#include "game/player.h"

using ov::Vec4;
using ov::Mat4;

namespace hud {

namespace {

// Полупрозрачная подложка: смешиваем прямо в кадре окна
void shade_rect(ui_win_t* w, int x, int y, int ww, int hh, uint32_t color, int alpha) {
    uint32_t* px = ui_pixels(w);
    int W = ui_width(w), H = ui_height(w);
    if (x < 0) { ww += x; x = 0; }
    if (y < 0) { hh += y; y = 0; }
    if (x + ww > W) ww = W - x;
    if (y + hh > H) hh = H - y;
    uint32_t cr = (color >> 16) & 255, cg = (color >> 8) & 255, cb = color & 255;
    for (int j = 0; j < hh; j++) {
        uint32_t* row = px + (y + j) * W + x;
        for (int i = 0; i < ww; i++) {
            uint32_t c = row[i];
            uint32_t r = (((c >> 16) & 255) * (uint32_t)(256 - alpha) + cr * (uint32_t)alpha) >> 8;
            uint32_t g = (((c >> 8) & 255) * (uint32_t)(256 - alpha) + cg * (uint32_t)alpha) >> 8;
            uint32_t b = ((c & 255) * (uint32_t)(256 - alpha) + cb * (uint32_t)alpha) >> 8;
            row[i] = (r << 16) | (g << 8) | b;
        }
    }
}

void label(ui_win_t* w, int x, int y, const char* s) {
    ui_text(w, x + 1, y + 1, s, 0x101010);
    ui_text(w, x, y, s, 0xF4F4F4);
}

// Прицел: инвертирует то, что под ним, — виден и на снегу, и в пещере
void crosshair(ui_win_t* w) {
    uint32_t* px = ui_pixels(w);
    int W = ui_width(w), H = ui_height(w), cx = W / 2, cy = H / 2;
    for (int i = -9; i <= 9; i++) {
        for (int t = 0; t < 2; t++) {
            int x = cx + i, y = cy + t - 1;
            if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] ^= 0xFFFFFF;
            if (i < -1 || i > 0) {
                x = cx + t - 1; y = cy + i;
                if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] ^= 0xFFFFFF;
            }
        }
    }
}

// Значки здоровья, голода и воздуха — картинки 9x9 по символам:
// # контур, остальное — цвета ниже. Рисуются квадратиками масштаба интерфейса
const char* const HEART[9] = {
    ".##...##.", "#ww#.#rr#", "#wrr#rrr#", "#rrrrrrr#", "#rrrrrrr#",
    ".#rrrrr#.", "..#rrr#..", "...#r#...", "....#....",
};
const char* const FOOD[9] = {
    "....####.", "...#mmmm#", "..#mmmmm#", "..#mMmmm#", "..#mmmm#.",
    ".#b####..", "#bb#.....", "#b#......", ".#.......",
};
const char* const BUBBLE[9] = {
    "...###...", "..#www#..", ".#wo..o#.", ".#w....#.", ".#.....#.",
    ".#.....#.", "..#...#..", "...###...", ".........",
};

// part: 2 — целый, 1 — половина (левая), 0 — пустой (только контур и тень)
void pic(ui_win_t* w, const char* const* rows, int x, int y, int s, int part, uint32_t full, uint32_t light, uint32_t dark) {
    for (int j = 0; j < 9; j++)
        for (int i = 0; i < 9; i++) {
            char c = rows[j][i];
            if (c == '.') continue;
            uint32_t col;
            if (c == '#') col = 0x1A0A0A;
            else {
                bool lit = part == 2 || (part == 1 && i <= 4);
                if (!lit) col = dark;
                else if (c == 'w' || c == 'M') col = light;
                else if (c == 'b') col = 0xE8E0D0;                 // косточка окорока
                else col = full;
            }
            ui_fill(w, x + i * s, y + j * s, s, s, col);
        }
}

void hotbar(ui_win_t* w, const HudInfo& h) {
    int W = ui_width(w), H = ui_height(w);
    const int s = gui::scale(W, H);
    const int cell = 20 * s, n = 9, total = n * cell + 2 * s;
    int x0 = (W - total) / 2, y0 = H - cell - 4 * s;
    const Inventory& inv = *h.inv;
    shade_rect(w, x0, y0, total, cell + 2 * s, 0x000000, 120);
    ui_frame(w, x0, y0, total, cell + 2 * s, 0x2A2A2A);
    for (int i = 0; i < n; i++) {
        int x = x0 + s + i * cell;
        ui_frame(w, x, y0 + s, cell, cell, 0x6A6A6A);
        const Stack& st = inv.slots[i];
        icons::slot(w, x + 2 * s, y0 + 3 * s, 16 * s, st.id, h.player->creative ? 0 : st.count);
        if (h.player->creative) icons::draw(w, x + 2 * s, y0 + 3 * s, 16 * s, st.id);
    }
    // Выбранный слот — светлая рамка поверх соседних
    int sx = x0 + s + inv.selected * cell - s;
    for (int k = 0; k < s; k++) {
        ui_frame(w, sx + k, y0 - s + k, cell + 2 * s - 2 * k, cell + 4 * s - 2 * k, 0xF0F0F0);
    }

    const Player& p = *h.player;
    int bar_y = y0 - 11 * s;
    if (!p.creative) {
        // Сердца слева, окорока справа (справа налево), пузыри над голодом под водой
        for (int i = 0; i < 10; i++) {
            int hp = p.health - i * 2;
            int part = hp >= 2 ? 2 : (hp == 1 ? 1 : 0);
            int shake = (p.health <= 4 && ((i * 7 + (int)(p.walk_dist * 9.0f)) & 3) == 0) ? -s : 0;
            uint32_t full = p.hurt_time > 0.3f ? 0xFFFFFF : 0xD8202A;
            pic(w, HEART, x0 + i * 8 * s, bar_y + shake, s, part, full, 0xFF9090, 0x3A0A0A);
        }
        for (int i = 0; i < 10; i++) {
            int f = p.food - i * 2;
            int part = f >= 2 ? 2 : (f == 1 ? 1 : 0);
            pic(w, FOOD, x0 + total - 9 * s - i * 8 * s, bar_y, s, part, 0xB0642E, 0xD88A50, 0x3A2010);
        }
        if (p.head_in_water || p.air < cfg::AIR_SECONDS) {
            int bubbles = (int)(p.air / cfg::AIR_SECONDS * 10.0f + 0.999f);
            for (int i = 0; i < bubbles && i < 10; i++)
                pic(w, BUBBLE, x0 + total - 9 * s - i * 8 * s, bar_y - 10 * s, s, 2, 0x2060C0, 0xFFFFFF, 0);
        }
    }
    // Название предмета в руке — сразу после выбора, потом гаснет
    const Stack& held = inv.held();
    if (h.name_time > 0.0f && !held.empty()) {
        const char* name = item_name(held.id);
        int tw = ui_text_width(name);
        int ly = (p.creative ? bar_y + 4 * s : bar_y - 4 * s) - 22;
        label(w, (W - tw) / 2, ly, name);
    }
}

} // namespace

void draw(ui_win_t* w, ov::Renderer& r, const HudInfo& h) {
    int W = ui_width(w), H = ui_height(w);
    if (h.in_water) shade_rect(w, 0, 0, W, H, 0x1C3C9A, 70);
    (void)r;
    crosshair(w);
    hotbar(w, h);

    // Верхняя строка всегда: FPS и сколько ядер рисуют
    Text top;
    top.num(h.fps).add(" FPS   ").fixed(h.frame_ms, 1).add(" мс   потоков ").num(h.st.threads);
    if (h.res < 100) top.add("   разрешение ").num(h.res).add("%");
    if (h.flying) top.add("   полёт");
    if (h.player->creative) top.add("   творческий");
    label(w, 10, 8, top.str());

    if (h.debug) {
        int y = 30;
        Text a;
        a.add("XYZ ").fixed(h.pos.x, 1).add(" / ").fixed(h.pos.y, 1).add(" / ").fixed(h.pos.z, 1);
        label(w, 10, y, a.str()); y += 20;
        Text b;
        b.add("чанков ").num(h.chunks).add(", секций в кадре ").num(h.sections);
        label(w, 10, y, b.str()); y += 20;
        Text c;
        c.add("треугольников ").num(h.st.drawn).add(" из ").num(h.st.submitted);
        if (h.st.dropped) c.add(", не влезло ").num(h.st.dropped);
        label(w, 10, y, c.str()); y += 20;
        Text d;
        d.add("геометрия ").fixed(h.st.ms_geometry, 1).add(" мс, растр ").fixed(h.st.ms_raster, 1).add(" мс");
        label(w, 10, y, d.str()); y += 20;
        // Память: если «кусок» движка меньше пары мегабайт — скоро будет код 12
        Text m;
        const int MB = 1024 * 1024;
        m.add("память: движок ").num((int)(h.eng_used / MB)).add(" из ").num((int)(h.eng_total / MB))
         .add(" МБ (свободный кусок ").num((int)(h.eng_free / MB)).add("), игра ").num((int)(h.game_used / MB)).add(" МБ");
        label(w, 10, y, m.str()); y += 20;
        // Загрузка каждого потока движка: видно, что работают все ядра
        for (int i = 0; i < h.st.threads && i < OE_MAX_THREADS; i++) {
            int pct = (int)(h.st.thread_busy[i] * 100.0f + 0.5f);
            if (pct > 100) pct = 100;
            Text t;
            t.add(i == 0 ? "основной " : "ядро ").num(i).add("  ").num(pct).add("%");
            shade_rect(w, 10, y + 2, 160, 12, 0x000000, 90);
            ui_fill(w, 10, y + 2, 160 * pct / 100, 12, i == 0 ? 0x3DAEE9 : 0x58C060);
            label(w, 178, y - 1, t.str());
            y += 18;
        }
        if (h.target) {
            Text e;
            e.add("под прицелом: ").add(h.target);
            label(w, 10, y + 4, e.str());
        }
    }

    if (h.message) {
        int tw = ui_text_width(h.message);
        shade_rect(w, (W - tw) / 2 - 10, H / 3 - 6, tw + 20, UI_FONT_H + 12, 0x000000, 130);
        label(w, (W - tw) / 2, H / 3, h.message);
    }

    if (!h.grabbed) {
        const char* s1 = "Щелкни по окну, чтобы играть";
        const char* s2 = "WASD — идти, пробел — прыжок, Ctrl или двойное W — бег, Shift — присесть, E — инвентарь";
        const char* s3 = "ЛКМ — ломать и бить, ПКМ — поставить и есть, Q — выбросить, F4 — режим, F5 — камера, Esc — мышь";
        int bw = ui_text_width(s3) + 40;
        if (bw < 360) bw = 360;
        int bx = (W - bw) / 2, by = H / 2 - 70;
        shade_rect(w, bx, by, bw, 92, 0x000000, 150);
        ui_text_center(w, bx, by + 12, bw, s1, 0xFFFFFF);
        ui_text_center(w, bx, by + 42, bw, s2, 0xC8C8C8);
        ui_text_center(w, bx, by + 64, bw, s3, 0xC8C8C8);
    }
}

void death(ui_win_t* w) {
    int W = ui_width(w), H = ui_height(w);
    shade_rect(w, 0, 0, W, H, 0x800000, 110);
    const char* t1 = "Вы умерли!";
    const char* t2 = "Щелкни или нажми пробел, чтобы возродиться";
    label(w, (W - ui_text_width(t1)) / 2, H / 2 - 40, t1);
    label(w, (W - ui_text_width(t2)) / 2, H / 2, t2);
}

void outline(ui_win_t* w, ov::Renderer& r, const Mat4& vp, const ov::Vec3& eye, int bx, int by, int bz) {
    uint32_t* px = ui_pixels(w);
    int W = ui_width(w), H = ui_height(w);
    int dw = 0, dh = 0;
    const float* depth = r.depth(&dw, &dh);
    float sx[8], sy[8], siw[8];
    const float e = 0.004f;                       // чуть больше блока, чтобы не тонуть в гранях
    for (int i = 0; i < 8; i++) {
        float x = (float)bx + ((i & 1) ? 1.0f + e : -e);
        float y = (float)by + ((i & 2) ? 1.0f + e : -e);
        float z = (float)bz + ((i & 4) ? 1.0f + e : -e);
        Vec4 c = vp * Vec4(x, y, z, 1.0f);
        if (c.w < 0.05f) return;                  // угол за камерой — рамку не рисуем
        sx[i] = (c.x / c.w * 0.5f + 0.5f) * (float)W;
        sy[i] = (0.5f - c.y / c.w * 0.5f) * (float)H;
        siw[i] = 1.0f / c.w;                      // как в z-буфере движка: 1/w, по экрану линейно
    }
    static const int E[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                  { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    // Какие грани повёрнуты к камере: по ним и видно ребро. Дальние рёбра
    // (сквозь блок) не рисуем — z-буфера у рамки нет
    bool face[3][2];
    face[0][0] = eye.x < (float)bx; face[0][1] = eye.x > (float)(bx + 1);
    face[1][0] = eye.y < (float)by; face[1][1] = eye.y > (float)(by + 1);
    face[2][0] = eye.z < (float)bz; face[2][1] = eye.z > (float)(bz + 1);
    for (int k = 0; k < 12; k++) {
        int a = E[k][0], b = E[k][1], axis = (a ^ b) == 1 ? 0 : ((a ^ b) == 2 ? 1 : 2);
        bool vis = false;
        for (int o = 0; o < 3; o++) {
            if (o == axis) continue;
            int side = (a >> o) & 1;                  // ребро лежит на грани этой оси с этой стороны
            if (face[o][side]) vis = true;
        }
        if (!vis) continue;
        float x0 = sx[E[k][0]], y0 = sy[E[k][0]], x1 = sx[E[k][1]], y1 = sy[E[k][1]];
        float iw0 = siw[E[k][0]], iw1 = siw[E[k][1]];
        float dx = x1 - x0, dy = y1 - y0;
        float len = ov::fmax(ov::fabs(dx), ov::fabs(dy));
        int n = (int)len + 1;
        if (n > 4000) continue;
        for (int i = 0; i <= n; i++) {
            float t = (float)i / (float)n;
            int x = (int)(x0 + dx * t), y = (int)(y0 + dy * t);
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            // Ближе ли что-то: блок перед рамкой или рука. Запас 2 % — рамка
            // лежит на самих гранях блока, и их глубина почти та же
            if (depth) {
                float d = depth[(y * dh / H) * dw + (x * dw / W)];
                if ((iw0 + (iw1 - iw0) * t) * 1.02f < d) continue;
            }
            px[y * W + x] = 0x141414;
        }
    }
}

void loading(ui_win_t* w, int percent) {
    int W = ui_width(w), H = ui_height(w);
    ui_fill(w, 0, 0, W, H, 0x2B2A33);
    ui_text_center(w, 0, H / 2 - 40, W, "Мир строится...", 0xFFFFFF);
    int bw = W / 2, bx = (W - bw) / 2, by = H / 2;
    ui_fill(w, bx, by, bw, 10, 0x45444F);
    ui_fill(w, bx, by, bw * percent / 100, 10, 0x58C060);
    Text t;
    t.num(percent).add("%");
    ui_text_center(w, 0, by + 20, W, t.str(), 0xC8C8C8);
}

} // namespace hud
