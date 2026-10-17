#ifndef OVCRAFT_PHYSICS_H
#define OVCRAFT_PHYSICS_H
// =============================================================================
// physics.h — столкновения коробки с миром: общие для игрока, мобов и
// выпавших предметов.
//
// Тело — прямоугольник половинной ширины hw и высоты h, pos — середина низа.
// Сдвиг идёт по одной оси за раз: упёрлись — встаём вплотную к грани блока
// и гасим скорость по этой оси. Так вдоль стены можно скользить.
// =============================================================================

#include "engine/math3d.h"

class World;

namespace phys {

inline int ifloor(float v) { int i = (int)v; return v < (float)i ? i - 1 : i; }

struct Body {
    ov::Vec3 pos, vel;
    float hw = 0.3f, h = 1.8f;
    bool on_ground = false;
    bool hit_wall = false;      // в этом шаге упёрлись по X или Z
};

bool collides(const World& w, const ov::Vec3& pos, float hw, float h);
// Сдвиг по оси (0 — X, 1 — Y, 2 — Z); true — упёрлись
bool move_axis(const World& w, Body& b, int axis, float delta);
// Обычный шаг: Y, потом X и Z. on_ground и hit_wall — по итогам шага
void move(const World& w, Body& b, const ov::Vec3& delta);
// Есть ли опора под телом, стоящим в pos (для «не упасть с края» на Shift)
bool ground_below(const World& w, const ov::Vec3& pos, float hw);

} // namespace phys

#endif
