#ifndef OVCRAFT_HUD_H
#define OVCRAFT_HUD_H
// =============================================================================
// hud.h — всё, что рисуется поверх мира: прицел, панель блоков, рамка вокруг
// блока под прицелом, отладочный экран (F3) и подсказка, пока мышь не
// захвачена. Рисуется прямо в кадр окна после движка.
// =============================================================================

#include "engine/ovengine_ll.h"
#include "libui/ui.h"
#include "libui/libui_ll.h"

// Строка без snprintf (его в программе нет)
class Text {
public:
    Text& add(const char* s) {
        while (*s && n_ < (int)sizeof(buf_) - 1) buf_[n_++] = *s++;
        buf_[n_] = '\0';
        return *this;
    }
    Text& num(int v) { char t[16]; ui_itoa(v, t); return add(t); }
    Text& fixed(float v, int digits) {          // v с digits знаками после точки
        if (v < 0) { add("-"); v = -v; }
        int scale = 1;
        for (int i = 0; i < digits; i++) scale *= 10;
        int iv = (int)(v * (float)scale + 0.5f);
        num(iv / scale);
        if (digits) {
            add(".");
            char t[8];
            int frac = iv % scale;
            for (int i = digits - 1; i >= 0; i--) { t[i] = (char)('0' + frac % 10); frac /= 10; }
            t[digits] = '\0';
            add(t);
        }
        return *this;
    }
    const char* str() const { return buf_; }
private:
    char buf_[192] = { 0 };
    int n_ = 0;
};

class Player;
class Inventory;

struct HudInfo {
    int fps;
    float frame_ms;
    ov::FrameStats st;
    int chunks, sections;
    ov::Vec3 pos;
    float yaw;
    bool flying, in_water, grabbed, debug;
    const Player* player;
    const Inventory* inv;
    float name_time;            // > 0 — показать название предмета в руке (сек, гаснет к нулю)
    const char* message;        // сообщение посреди экрана (режим игры) или nullptr
    const char* target;         // блок под прицелом или nullptr
    int progress;               // мир ещё строится: 0..100, иначе -1
    int res;                    // разрешение мира, % от окна
    uint64_t eng_used, eng_total, eng_free;   // куча движка (F3): занято, всего, самый большой кусок
    uint64_t game_used;                       // куча игры
};

namespace hud {

void draw(ui_win_t* w, ov::Renderer& r, const HudInfo& h);
// Экран смерти поверх мира
void death(ui_win_t* w);
// Рамка вокруг блока (x, y, z): проекция 12 рёбер куба
// Пиксели, закрытые чем-то ближе (другой блок, рука), не рисуются: сверка с z-буфером движка
void outline(ui_win_t* w, ov::Renderer& r, const ov::Mat4& view_proj, const ov::Vec3& eye, int x, int y, int z);
// Экран «мир строится»
void loading(ui_win_t* w, int percent);

} // namespace hud

#endif
