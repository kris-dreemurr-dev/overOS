// Заглушка game/input.h для стенда: состояние задаёт сам сценарий (sim.cpp)
#pragma once
namespace key {
enum : int {
    ESC = 0x01, N1 = 0x02, N9 = 0x0A, Q = 0x10, W = 0x11, E = 0x12, A = 0x1E, S = 0x1F, D = 0x20,
    F = 0x21, CTRL = 0x1D, SHIFT = 0x2A, SPACE = 0x39, F3 = 0x3D, F4 = 0x3E, F5 = 0x3F,
};
}
class Input {
public:
    bool down_[128] = {}, pressed_[128] = {};
    int dx_ = 0, dy_ = 0, buttons_ = 0, prev_ = 0;
    bool down(int sc) const { return down_[sc & 0x7F]; }
    bool pressed(int sc) const { return pressed_[sc & 0x7F]; }
    int dx() const { return dx_; }
    int dy() const { return dy_; }
    int scroll() const { return 0; }
    bool button(int b) const { return (buttons_ >> b) & 1; }
    bool clicked(int b) const { return ((buttons_ >> b) & 1) && !((prev_ >> b) & 1); }
};
