#ifndef OVCRAFT_INPUT_H
#define OVCRAFT_INPUT_H
// =============================================================================
// input.h — состояние клавиатуры и мыши за кадр.
//
// libui отдаёт сырые скан-коды (отпускание = код | 0x80). Отсюда — какие
// клавиши зажаты сейчас и какие нажаты именно в этом кадре. Мышь —
// смещения захваченного указателя и кнопки (ui_motion, ui_buttons).
// =============================================================================

#include "libui/ui.h"
#include "libui/libui_ll.h"

namespace key {
enum : int {
    ESC = 0x01, N1 = 0x02, N9 = 0x0A, Q = 0x10, W = 0x11, E = 0x12, A = 0x1E, S = 0x1F, D = 0x20,
    F = 0x21, CTRL = 0x1D, SHIFT = 0x2A, SPACE = 0x39, F3 = 0x3D, F4 = 0x3E, F5 = 0x3F,
};
}

class Input {
public:
    void frame(ui_win_t* w) {
        for (int i = 0; i < 128; i++) pressed_[i] = false;
        int raw[64];
        int n = ui_raw_keys(w, raw, 64);
        for (int i = 0; i < n; i++) {
            int sc = raw[i] & 0x7F;
            bool down = !(raw[i] & 0x80);
            if (down && !down_[sc]) pressed_[sc] = true;
            down_[sc] = down;
        }
        prev_buttons_ = buttons_;
        buttons_ = ui_buttons(w);
        ui_motion(w, &dx_, &dy_);
        scroll_ = ui_scroll(w);
    }
    // Окно потеряло фокус: отпускания мы уже не услышим
    void release_all() { for (int i = 0; i < 128; i++) down_[i] = pressed_[i] = false; buttons_ = 0; }

    bool down(int sc) const { return down_[sc & 0x7F]; }
    bool pressed(int sc) const { return pressed_[sc & 0x7F]; }
    int dx() const { return dx_; }
    int dy() const { return dy_; }
    int scroll() const { return scroll_; }
    bool button(int b) const { return (buttons_ >> b) & 1; }
    bool clicked(int b) const { return ((buttons_ >> b) & 1) && !((prev_buttons_ >> b) & 1); }

private:
    bool down_[128] = {};
    bool pressed_[128] = {};
    int buttons_ = 0, prev_buttons_ = 0;
    int dx_ = 0, dy_ = 0, scroll_ = 0;
};

#endif
