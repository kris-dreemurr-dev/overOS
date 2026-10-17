#ifndef OVCRAFT_PLAYER_H
#define OVCRAFT_PLAYER_H
// =============================================================================
// player.h — игрок: камера, ходьба, бег, приседание, прыжок, полёт,
// плавание, столкновения и выживание (здоровье, голод, воздух).
//
// Тело — прямоугольник PLAYER_W x высота x PLAYER_W (присев — ниже), pos —
// середина ступней. Физика идёт фиксированными шагами PHYS_DT: высота прыжка
// и скорость не зависят от FPS; столкновения — game/physics.
//
// Выживание — по правилам Minecraft: 20 очков здоровья и голода, насыщение
// тратится раньше голода, истощение (бег, прыжки, лечение) копится и съедает
// насыщение или голод. Сытый (голод >= 18) лечится, голодный (0) теряет
// здоровье. Падение выше 3 блоков, удушье под водой. В творческом режиме
// ничего этого нет и можно летать.
// =============================================================================

#include "engine/math3d.h"
#include "game/input.h"

class World;

class Player {
public:
    ov::Vec3 pos;
    ov::Vec3 vel;
    float yaw = 0.0f, pitch = 0.0f;   // yaw 0 — смотрим на -Z
    float body_yaw = 0.0f;            // тело поворачивается за головой с задержкой (модель)
    bool on_ground = false;
    bool flying = false;
    bool in_water = false, head_in_water = false;
    bool sneaking = false, sprinting = false;
    bool creative = false;

    // Выживание
    int health = 20, food = 20;
    float saturation = 5.0f, exhaustion = 0.0f;
    float air = 15.0f;
    float hurt_time = 0.0f;           // > 0 — только что ударили (красная вспышка, наклон камеры)
    bool dead = false;
    float walk_dist = 0.0f;           // пройдено по земле: качание руки и шаги модели
    float walk_amount = 0.0f;         // 0..1: насколько сейчас идёт (сглажено)

    void spawn(World& w, int x, int z);
    void respawn(World& w);                            // после смерти: на точку появления, всё заново
    void look(const Input& in);                        // мышь — каждый кадр
    void update(float dt, const Input& in, World& w);  // ходьба, физика, голод
    // Урон; false — не прошёл (творческий режим, неуязвимость после удара)
    bool damage(int amount);
    void eat(int food_points, float sat);
    void add_exhaustion(float e) { if (!creative) exhaustion += e; }
    void set_creative(bool on);

    ov::Vec3 eye() const { return ov::Vec3(pos.x, pos.y + eye_h_, pos.z); }
    ov::Vec3 forward() const;                          // куда смотрим (единичный)
    float height() const;
    bool intersects_block(int x, int y, int z) const;  // нельзя ставить блок в себя

private:
    void step(const Input& in, World& w);
    void vitals(float dt, World& w);
    float acc_ = 0.0f;
    float clock_ = 0.0f;
    float last_space_ = -1.0f, last_w_ = -1.0f;       // двойной пробел — полёт, двойное W — бег
    float eye_h_ = 1.62f;
    float fall_ = 0.0f;                                // сколько уже падаем (блоков)
    float regen_t_ = 0.0f, starve_t_ = 0.0f, drown_t_ = 0.0f, invuln_ = 0.0f;
    int spawn_x_ = 8, spawn_z_ = 8;
};

#endif
