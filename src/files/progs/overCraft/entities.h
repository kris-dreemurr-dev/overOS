#ifndef OVCRAFT_ENTITIES_H
#define OVCRAFT_ENTITIES_H
// =============================================================================
// entities.h — кто ходит по миру: игрок (его тело видно с третьего лица),
// мобы и выпавшие предметы.
//
// Модель — текстовый файл resources/models/<имя>.mdl (коробки и точки
// вращения, формат — в game/model.h), развёртка — PNG в resources/entities/
// в раскладке Minecraft. Как модель двигается (ноги, голова), решает код
// по именам частей: head, body, right_arm, left_arm, right_leg, left_leg,
// leg0..leg3 (четвероногие).
//
// Новый моб: строка в enum EntityType, строка в ENTITIES, файлы модели и
// развёртки; поведение — в game/entity.cpp.
// =============================================================================

#include <stdint.h>
#include "items.h"

enum EntityType : uint8_t {
    ENT_PLAYER = 0,
    ENT_PIG,                // тестовый моб: бродит, убегает, если ударить
    ENT_ITEM,               // выпавший предмет (без модели: кубик или значок)
    ENT_TYPE_COUNT
};

struct EntityInfo {
    const char* name;
    const char* model;      // nullptr — модели нет
    const char* texture;
    float width, height;    // хитбокс в блоках
    float scale;            // модель в пикселях: 16 пикселей = блок * scale
    int health;             // очков (2 = одно сердце)
    float speed;            // блоков в секунду
    uint16_t drop;          // что выпадает при смерти
    int drop_min, drop_max;
};

static const EntityInfo ENTITIES[ENT_TYPE_COUNT] = {
    { "Игрок",   "resources/models/steve.mdl", "resources/entities/steve.png", 0.6f, 1.8f, 0.9375f, 20, 4.3f, 0, 0, 0 },
    { "Свинья",  "resources/models/pig.mdl",   "resources/entities/pig.png",   0.9f, 0.9f, 1.0f,    10, 1.6f, PORKCHOP, 1, 3 },
    { "Предмет", nullptr,                      nullptr,                        0.25f, 0.25f, 1.0f,  1,  0.0f, 0, 0, 0 },
};

#endif
