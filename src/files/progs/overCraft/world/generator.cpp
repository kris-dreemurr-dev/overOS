// =============================================================================
// generator.cpp — процедурный мир (см. generator.h).
// =============================================================================

#include "generator.h"
#include "noise.h"
#include "blocks.h"

namespace gen {

using namespace cfg;

int surface_height(int wx, int wz) {
    float x = (float)wx, z = (float)wz;
    // Холмы: несколько октав шума значений
    float hills = noise::fbm2(x * TERRAIN_SCALE, z * TERRAIN_SCALE, 5, SEED);
    float h = TERRAIN_BASE + (hills - 0.5f) * 2.0f * TERRAIN_AMP;
    // Горы: редкие широкие пятна, поднимающие рельеф квадратично
    float m = noise::fbm2(x * TERRAIN_SCALE * 0.45f + 311.0f, z * TERRAIN_SCALE * 0.45f - 97.0f, 4, SEED + 7u);
    if (m > 0.55f) {
        float k = (m - 0.55f) / 0.45f;
        h += k * k * MOUNTAIN_AMP * 2.2f;
    }
    int hi = (int)h;
    if (hi < 4) hi = 4;
    if (hi > WORLD_H - 12) hi = WORLD_H - 12;
    return hi;
}

static bool is_cave(int wx, int y, int wz, int top) {
    if (y < 2 || y > top - 1) return false;
    // У воды пещеры не роем: иначе океан провалится в дыру
    if (top <= SEA_LEVEL + 1 && y >= SEA_LEVEL - 3) return false;
    float s = CAVE_SCALE;
    float a = noise::value3((float)wx * s, (float)y * s * 1.7f, (float)wz * s, SEED + 31u);
    float b = noise::value3((float)wx * s * 2.1f, (float)y * s * 2.4f, (float)wz * s * 2.1f, SEED + 37u);
    return a * 0.65f + b * 0.35f > CAVE_THRESHOLD;
}

// Есть ли в столбе (wx, wz) дерево: решает хеш, а не случайность —
// соседний чанк получит тот же ответ
static bool tree_at(int wx, int wz, int top, int* height) {
    if (top <= SEA_LEVEL + 1 || top >= 66) return false;          // ни на пляже, ни в горах
    uint32_t r = noise::hash(wx, 0, wz, SEED + 501u);
    if ((int)(r % 10000u) >= TREE_CHANCE) return false;
    // Не вплотную к другому дереву: соседний столб с деревом «главнее», если левее/ниже
    for (int dz = -2; dz <= 2; dz++)
        for (int dx = -2; dx <= 2; dx++) {
            if (!dx && !dz) continue;
            if (dz < 0 || (dz == 0 && dx < 0)) {
                uint32_t o = noise::hash(wx + dx, 0, wz + dz, SEED + 501u);
                if ((int)(o % 10000u) < TREE_CHANCE) return false;
            }
        }
    *height = 4 + (int)((r >> 16) % 3u);
    return true;
}

// mode 0 — только в воздух (листва), 1 — и поверх листвы (ствол), 2 — всегда
static void put(Chunk& c, int wx, int y, int wz, uint8_t b, int mode) {
    int lx = wx - c.cx * CHUNK, lz = wz - c.cz * CHUNK;
    if (lx < 0 || lx >= CHUNK || lz < 0 || lz >= CHUNK || y < 0 || y >= WORLD_H) return;
    uint8_t cur = c.get(lx, y, lz);
    if (mode == 0 && cur != AIR) return;
    if (mode == 1 && cur != AIR && cur != LEAVES) return;
    c.set(lx, y, lz, b);
}

void fill(Chunk& c, const BlockEdit* edits, int nedits) {
    const int bx = c.cx * CHUNK, bz = c.cz * CHUNK;
    for (int i = 0; i < WORLD_H * CHUNK * CHUNK; i++) c.blocks[i] = AIR;

    for (int z = 0; z < CHUNK; z++)
        for (int x = 0; x < CHUNK; x++) {
            int wx = bx + x, wz = bz + z;
            int top = surface_height(wx, wz);
            bool beach = top <= SEA_LEVEL + 1;
            bool peak = top >= 70;
            for (int y = 0; y <= top; y++) {
                uint8_t b;
                if (y == 0) b = BEDROCK;
                else if (y < top - 3) b = STONE;
                else if (y < top) b = beach ? SAND : (peak ? STONE : DIRT);
                else b = beach ? SAND : (peak ? SNOW : GRASS);
                if (y > 0 && is_cave(wx, y, wz, top)) b = AIR;
                c.set(x, y, z, b);
            }
            for (int y = top + 1; y <= SEA_LEVEL; y++) c.set(x, y, z, WATER);
        }

    // Деревья: столбы на 2 блока за краем тоже смотрим — их крона может
    // заходить к нам
    for (int wz = bz - 2; wz < bz + CHUNK + 2; wz++)
        for (int wx = bx - 2; wx < bx + CHUNK + 2; wx++) {
            int top = surface_height(wx, wz), th;
            if (!tree_at(wx, wz, top, &th)) continue;
            int crown = top + th;
            for (int y = crown - 2; y <= crown + 1; y++) {
                int r = (y >= crown) ? 1 : 2;
                for (int dz = -r; dz <= r; dz++)
                    for (int dx = -r; dx <= r; dx++) {
                        // Углы кроны срезаем через раз — чтобы не была кубом
                        if (r == 2 && (dx == -2 || dx == 2) && (dz == -2 || dz == 2) &&
                            (noise::hash(wx + dx, y, wz + dz, SEED + 77u) & 1)) continue;
                        put(c, wx + dx, y, wz + dz, LEAVES, 0);
                    }
            }
            for (int y = top + 1; y <= crown; y++) put(c, wx, y, wz, LOG, 1);
            put(c, wx, top, wz, DIRT, 2);              // под деревом трава не растёт
        }

    // Правки игрока поверх
    for (int i = 0; i < nedits; i++) {
        const BlockEdit& e = edits[i];
        int lx = e.x - bx, lz = e.z - bz;
        if (lx < 0 || lx >= CHUNK || lz < 0 || lz >= CHUNK || e.y < 0 || e.y >= WORLD_H) continue;
        c.set(lx, e.y, lz, e.block);
    }
}

} // namespace gen
