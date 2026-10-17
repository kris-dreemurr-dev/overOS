// =============================================================================
// hand.cpp — рука от первого лица (см. hand.h).
// =============================================================================

#include "hand.h"
#include "items.h"
#include "game/player.h"
#include "game/shapes.h"

using ov::Vec3;
using ov::Mat4;

void HandAnim::update(float dt, uint16_t held) {
    if (swing >= 0.0f) {
        swing += dt / 0.3f;
        if (swing >= 1.0f) swing = -1.0f;
    }
    // Другой предмет — рука опускается, меняет его внизу и поднимается
    if (held != shown) {
        equip -= dt * 7.0f;
        if (equip <= 0.0f) { equip = 0.0f; shown = held; }
    } else if (equip < 1.0f) {
        equip += dt * 7.0f;
        if (equip > 1.0f) equip = 1.0f;
    }
}

namespace hand {

void draw(ov::Renderer& r, const Model& steve, const Player& p, const HandAnim& a) {
    const float PI = ov::PI;
    float s = a.swing_value();
    float ss = ov::sqrt(s);
    // Покачивание от шагов (как «view bobbing»): по кругу вправо-влево, вниз на каждом шаге
    float ph = p.walk_dist * 1.25f;
    float bob_x = ov::sin(ph * PI) * 0.025f * p.walk_amount;
    float bob_y = -ov::fabs(ov::cos(ph * PI)) * 0.035f * p.walk_amount;
    float drop = (1.0f - a.equip) * 0.6f;                     // смена предмета
    // Взмах: рука уходит влево-вверх и кивает вниз (как в ItemRenderer оригинала)
    Vec3 sw(-0.38f * ov::sin(ss * PI), 0.18f * ov::sin(ss * PI * 2.0f), -0.18f * ov::sin(s * PI));

    if (a.shown == 0) {
        int ra = steve.find("right_arm");
        if (ra < 0) return;
        // Плечо ниже-правее камеры, рука вытянута вперёд и чуть внутрь
        // Плечо — за правым нижним краем кадра, кисть — вперёд и вверх к центру
        Mat4 m = Mat4::translate(Vec3(0.42f + bob_x, -0.50f + bob_y - drop, -0.30f) + sw * 0.6f) *
                 Mat4::rotateY(0.35f + ov::sin(ss * PI) * 0.5f) *
                 Mat4::rotateX(1.95f - ov::sin(s * s * PI) * 0.6f) *
                 Mat4::rotateZ(0.10f) *
                 Mat4::scale(1.0f / 16.0f);
        r.drawOverlay(steve.part(ra).mesh, m);
        return;
    }
    // Еда: предмет к центру и мелко качается
    Vec3 eat(0, 0, 0);
    if (a.eat > 0.0f) {
        float e = a.eat > 0.15f ? 1.0f : a.eat / 0.15f;
        eat = Vec3(-0.30f * e, 0.12f * e + ov::fabs(ov::sin(a.eat * 40.0f)) * 0.03f * e, 0.05f * e);
    }
    Vec3 at = Vec3(0.44f + bob_x, -0.32f + bob_y - drop, -0.80f) + sw + eat;
    float nod = -ov::sin(ss * PI) * 1.0f;                      // взмах наклоняет предмет вниз
    Mat4 m;
    if (item_is_block(a.shown)) {
        m = Mat4::translate(at) * Mat4::rotateX(0.38f + nod * 0.6f) * Mat4::rotateY(0.785f) * Mat4::scale(0.30f);
    } else {
        // Плоский предмет: ребром к камере, наклонён вперёд, как в руке
        m = Mat4::translate(at + Vec3(0.04f, 0.06f, 0)) * Mat4::rotateX(nod) * Mat4::rotateY(-1.25f) *
            Mat4::rotateZ(0.42f) * Mat4::scale(0.60f);
    }
    r.drawOverlay(shapes::held(a.shown), m);
}

} // namespace hand
