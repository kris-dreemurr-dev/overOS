#ifndef OVCRAFT_HAND_H
#define OVCRAFT_HAND_H
// =============================================================================
// hand.h — рука от первого лица.
//
// Рисуется отдельным слоем движка (OE_DRAW_OVERLAY): своя проекция, z-буфер
// сброшен, поэтому рука не уходит в стену, а FOV бега её не растягивает.
// Пустая рука — правая рука модели игрока (та же развёртка, что у тела).
// С предметом — кубик блока или выдавленный значок, сама рука скрыта, как
// в Minecraft. Анимации: покачивание при ходьбе, взмах (удар, установка),
// смена предмета (опустить и поднять), еда.
// =============================================================================

#include "engine/ovengine_ll.h"
#include "game/model.h"

class Player;

struct HandAnim {
    float swing = -1.0f;        // 0..1 — идёт взмах, < 0 — нет
    float equip = 1.0f;         // 0 — рука внизу (меняем предмет), 1 — поднята
    float eat = 0.0f;           // 0..1 — сколько съедено (держим ПКМ)
    uint16_t shown = 0;         // что сейчас в руке на экране (меняется, когда рука внизу)

    void start_swing() { if (swing < 0.0f || swing > 0.5f) swing = 0.0f; }
    void update(float dt, uint16_t held);
    float swing_value() const { return swing < 0.0f ? 0.0f : swing; }
};

namespace hand {
void draw(ov::Renderer& r, const Model& steve, const Player& p, const HandAnim& a);
}

#endif
