#ifndef OVCRAFT_ENTITY_H
#define OVCRAFT_ENTITY_H
// =============================================================================
// entity.h — всё, что ходит по миру, кроме самого игрока: мобы (entities.h)
// и выпавшие предметы. Здесь же рисуется тело игрока (с третьего лица).
//
// Мобы — фиксированный массив слотов: никто не выделяет память на ходу.
// Физика — та же коробка, что у игрока (game/physics), поведение свиньи —
// бродить, иногда стоять, смотреть на игрока, после удара убегать.
// =============================================================================

#include "engine/ovengine_ll.h"
#include "entities.h"
#include "game/model.h"
#include "game/physics.h"

class World;
class Player;
class Inventory;

struct Entity {
    uint8_t type = 0;
    bool alive = false;
    phys::Body body;
    float yaw = 0.0f, head_yaw = 0.0f, head_pitch = 0.0f;
    int health = 0;
    float hurt = 0.0f, invuln = 0.0f;   // красная вспышка и неуязвимость после удара
    float death = 0.0f;                 // > 0 — падает на бок (секунды)
    float walk_dist = 0.0f, walk_amount = 0.0f;
    // Поведение
    float ai_t = 0.0f, target_yaw = 0.0f, panic = 0.0f;
    bool walking = false;
    // Выпавший предмет
    uint16_t item = 0;
    int count = 0;
    float age = 0.0f, pickup = 0.0f;
};

class Entities {
public:
    static const int MAX = 160;

    // Модели и развёртки всех типов (и красные копии для вспышки урона)
    bool init(ov::Renderer& r);
    const char* error() const { return err_; }

    void update(float dt, World& w, Player& p, Inventory& inv);
    void draw(ov::Renderer& r, const ov::Vec3& eye) const;
    // Тело игрока: в руке held, swing — взмах 0..1
    void draw_player(ov::Renderer& r, const Player& p, uint16_t held, float swing) const;

    // Ближайший моб на луче (не предметы): номер или -1
    int raycast(const ov::Vec3& from, const ov::Vec3& dir, float max_dist, float* dist) const;
    void attack(int i, int damage, const ov::Vec3& from);
    void drop(const ov::Vec3& pos, const ov::Vec3& vel, uint16_t id, int count, float pickup_delay);
    // Выбросить из руки игрока вперёд
    void throw_from(const Player& p, uint16_t id, int count);
    int spawn(uint8_t type, const ov::Vec3& pos);
    int spawn_near(World& w, const ov::Vec3& at, float rmin, float rmax, int n);   // сколько появилось

    int count(uint8_t type) const;
    const Model& model(uint8_t type) const { return models_[type]; }

private:
    void update_mob(Entity& e, float dt, World& w, Player& p);
    void update_item(Entity& e, float dt, World& w, Player& p, Inventory& inv);
    void draw_mob(ov::Renderer& r, const Entity& e) const;
    bool try_spawn(World& w, float x, float z);
    float rnd();

    Entity ents_[MAX];
    Model models_[ENT_TYPE_COUNT];
    bool has_model_[ENT_TYPE_COUNT] = {};
    uint32_t seed_ = 0x9E3779B9u;
    float spawn_t_ = 0.0f;
    const char* err_ = nullptr;
};

#endif
