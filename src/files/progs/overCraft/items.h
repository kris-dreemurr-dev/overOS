#ifndef OVCRAFT_ITEMS_H
#define OVCRAFT_ITEMS_H
// =============================================================================
// items.h — что бывает в инвентаре.
//
// Один ряд номеров на всё: 1..BLOCK_COUNT-1 — блоки (номер из blocks.h, значок
// — кубик из его текстур), с ITEM_FIRST — предметы со своим плоским значком
// 16x16 из resources/items/. 0 — пусто.
//
// Новый предмет: строка в enum Item, PNG в ITEM_TEXTURES (в том же порядке)
// и строка в ITEMS.
// =============================================================================

#include <stdint.h>
#include "blocks.h"

enum Item : uint16_t {
    ITEM_NONE = 0,
    ITEM_FIRST = 64,
    PORKCHOP = ITEM_FIRST,  // тестовый предмет: падает со свиньи, утоляет голод
    ITEM_END
};
static const int ITEM_COUNT = ITEM_END - ITEM_FIRST;

// Значки предметов: грузятся у движка сразу после текстур блоков,
// номер у движка = TEX_COUNT + номер здесь
static const char* const ITEM_TEXTURES[ITEM_COUNT] = {
    "resources/items/porkchop.png",
};

struct ItemInfo {
    const char* name;
    int max_stack;
    int food;               // сколько очков голода вернёт (0 — несъедобно)
    float saturation;       // насыщение: пока оно есть, голод не тратится
};

static const ItemInfo ITEMS[ITEM_COUNT] = {
    { "Сырая свинина", 64, 3, 1.8f },
};

inline bool item_is_block(uint16_t id) { return id > 0 && id < BLOCK_COUNT; }
inline bool item_is_item(uint16_t id) { return id >= ITEM_FIRST && id < ITEM_END; }
inline bool item_valid(uint16_t id) { return item_is_block(id) || item_is_item(id); }
inline const ItemInfo* item_info(uint16_t id) { return item_is_item(id) ? &ITEMS[id - ITEM_FIRST] : nullptr; }
inline const char* item_name(uint16_t id) {
    if (item_is_block(id)) return BLOCKS[id].name;
    if (item_is_item(id)) return ITEMS[id - ITEM_FIRST].name;
    return "";
}
inline int item_max_stack(uint16_t id) { return item_is_item(id) ? ITEMS[id - ITEM_FIRST].max_stack : 64; }
inline int item_texture(uint16_t id) { return TEX_COUNT + (id - ITEM_FIRST); }   // только для предметов

#endif
