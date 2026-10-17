#ifndef OVCRAFT_BLOCKS_H
#define OVCRAFT_BLOCKS_H
// =============================================================================
// blocks.h — какие бывают блоки.
//
// Новый блок: строка в enum Block, строка в BLOCKS (в том же порядке) и, если
// нужно, новая текстура в TEXTURES и PNG 16x16 в resources/.
// Блок — это и предмет в инвентаре с тем же номером (см. items.h).
// =============================================================================

#include <stdint.h>

enum Block : uint8_t {
    AIR = 0,
    GRASS,
    DIRT,
    STONE,
    COBBLE,
    SAND,
    LOG,
    LEAVES,
    PLANKS,
    WATER,
    BEDROCK,
    SNOW,
    GLASS,
    BRICK,
    BLOCK_COUNT
};

// Текстуры: номер в этом списке = номер у движка (грузятся по порядку)
enum Tex : uint16_t {
    T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, T_STONE, T_COBBLE, T_SAND, T_LOG_SIDE,
    T_LOG_TOP, T_LEAVES, T_PLANKS, T_WATER, T_BEDROCK, T_SNOW, T_GLASS, T_BRICK,
    TEX_COUNT
};

static const char* const TEXTURES[TEX_COUNT] = {
    "resources/grass_top.png", "resources/grass_side.png", "resources/dirt.png",
    "resources/stone.png", "resources/cobble.png", "resources/sand.png",
    "resources/log_side.png", "resources/log_top.png", "resources/leaves.png",
    "resources/planks.png", "resources/water.png", "resources/bedrock.png",
    "resources/snow.png", "resources/glass.png", "resources/brick.png",
};

struct BlockInfo {
    const char* name;       // для панели внизу экрана
    bool solid;             // в нём нельзя стоять
    bool opaque;            // закрывает соседние грани (листва и стекло — нет)
    uint16_t top, side, bottom;
    bool placeable;         // есть в панели выбора
    float hardness;         // секунд ломать рукой в выживании; < 0 — не ломается
    uint8_t drop;           // что выпадает (AIR — ничего)
};

static const BlockInfo BLOCKS[BLOCK_COUNT] = {
    { "Воздух",     false, false, 0,           0,            0,           false, 0.0f,  AIR    },
    { "Трава",      true,  true,  T_GRASS_TOP, T_GRASS_SIDE, T_DIRT,      true,  0.9f,  DIRT   },
    { "Земля",      true,  true,  T_DIRT,      T_DIRT,       T_DIRT,      true,  0.75f, DIRT   },
    { "Камень",     true,  true,  T_STONE,     T_STONE,      T_STONE,     true,  2.25f, COBBLE },
    { "Булыжник",   true,  true,  T_COBBLE,    T_COBBLE,     T_COBBLE,    true,  3.0f,  COBBLE },
    { "Песок",      true,  true,  T_SAND,      T_SAND,       T_SAND,      true,  0.75f, SAND   },
    { "Бревно",     true,  true,  T_LOG_TOP,   T_LOG_SIDE,   T_LOG_TOP,   true,  3.0f,  LOG    },
    { "Листва",     true,  false, T_LEAVES,    T_LEAVES,     T_LEAVES,    true,  0.3f,  AIR    },
    { "Доски",      true,  true,  T_PLANKS,    T_PLANKS,     T_PLANKS,    true,  3.0f,  PLANKS },
    { "Вода",       false, true,  T_WATER,     T_WATER,      T_WATER,     false, -1.0f, AIR    },
    { "Коренная порода", true, true, T_BEDROCK, T_BEDROCK,   T_BEDROCK,   false, -1.0f, AIR    },
    { "Снег",       true,  true,  T_SNOW,      T_SNOW,       T_SNOW,      true,  0.3f,  SNOW   },
    { "Стекло",     true,  false, T_GLASS,     T_GLASS,      T_GLASS,     true,  0.45f, AIR    },
    { "Кирпич",     true,  true,  T_BRICK,     T_BRICK,      T_BRICK,     true,  3.0f,  BRICK  },
};

inline const BlockInfo& block_info(uint8_t b) { return BLOCKS[b < BLOCK_COUNT ? b : 0]; }

// Панель выбора в творческом режиме с самого начала (клавиши 1..9 и колесо)
static const uint8_t HOTBAR[9] = { GRASS, DIRT, STONE, COBBLE, PLANKS, LOG, GLASS, BRICK, SAND };

#endif
