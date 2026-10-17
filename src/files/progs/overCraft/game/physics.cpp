// =============================================================================
// physics.cpp — столкновения коробки с миром (см. physics.h).
// =============================================================================

#include "physics.h"
#include "world/world.h"

using ov::Vec3;

namespace phys {

namespace { const float EPS = 0.001f; }

bool collides(const World& w, const Vec3& pos, float hw, float h) {
    int x0 = ifloor(pos.x - hw + EPS), x1 = ifloor(pos.x + hw - EPS);
    int y0 = ifloor(pos.y + EPS), y1 = ifloor(pos.y + h - EPS);
    int z0 = ifloor(pos.z - hw + EPS), z1 = ifloor(pos.z + hw - EPS);
    for (int y = y0; y <= y1; y++)
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++)
                if (w.solid(x, y, z)) return true;
    return false;
}

bool move_axis(const World& w, Body& b, int axis, float delta) {
    if (delta == 0.0f) return false;
    float* p = axis == 0 ? &b.pos.x : (axis == 1 ? &b.pos.y : &b.pos.z);
    float* v = axis == 0 ? &b.vel.x : (axis == 1 ? &b.vel.y : &b.vel.z);
    float old = *p;
    *p += delta;
    if (!collides(w, b.pos, b.hw, b.h)) return false;
    // Ближайшая к старому месту точка без пересечения — грань блока
    if (axis == 1) {
        if (delta > 0) *p = (float)ifloor(old + b.h + delta) - b.h - EPS;
        else *p = (float)ifloor(old + delta) + 1.0f + EPS;
    } else {
        if (delta > 0) *p = (float)ifloor(old + b.hw + delta) - b.hw - EPS;
        else *p = (float)ifloor(old - b.hw + delta) + 1.0f + b.hw + EPS;
    }
    if (collides(w, b.pos, b.hw, b.h)) *p = old;     // крайний случай: остаёмся где были
    if (axis == 1 && delta < 0) b.on_ground = true;
    *v = 0.0f;
    return true;
}

void move(const World& w, Body& b, const Vec3& d) {
    b.on_ground = false;
    b.hit_wall = false;
    move_axis(w, b, 1, d.y);
    if (move_axis(w, b, 0, d.x)) b.hit_wall = true;
    if (move_axis(w, b, 2, d.z)) b.hit_wall = true;
}

bool ground_below(const World& w, const Vec3& pos, float hw) {
    return collides(w, Vec3(pos.x, pos.y - 0.6f, pos.z), hw, 0.55f);
}

} // namespace phys
