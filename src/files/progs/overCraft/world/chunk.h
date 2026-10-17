#ifndef OVCRAFT_CHUNK_H
#define OVCRAFT_CHUNK_H
// =============================================================================
// chunk.h — столб мира 16 x WORLD_H x 16 блоков.
//
// Блоки — по байту (номер из blocks.h). Сетка для движка строится не на весь
// чанк, а по секциям высотой SECTION_H: правка блока перестраивает одну-две
// секции, а пустые секции (небо) ничего не стоят. Буферы сетки принадлежат
// секции и растут только на основном потоке — задачам на рабочих ядрах
// выделять память нельзя (см. world.cpp).
// =============================================================================

#include <stdint.h>
#include "config.h"
#include "engine/ovengine.h"

struct SectionMesh {
    ov::Vertex* v = nullptr;
    ov::Face* f = nullptr;
    int nv = 0, nf = 0;
    int cap_v = 0, cap_f = 0;
    int need_v = 0, need_f = 0;     // сколько не влезло: основной поток расширит буфер
    bool dirty = true;              // блоки менялись — сетку перестроить
    ov::Mesh mesh;                  // вся секция (указатели на v/f)
    // Та же сетка по направлениям граней (+X, -X, +Y, -Y, +Z, -Z): группу,
    // повёрнутую от камеры, можно не отдавать движку вовсе
    ov::Mesh dir[6];

    void grow(int vcap, int fcap);
    void release();
};

enum ChunkState : int { CHUNK_EMPTY = 0, CHUNK_GENERATED, CHUNK_MESHED };

struct Chunk {
    int cx = 0, cz = 0;             // координаты чанка (в чанках)
    int state = CHUNK_EMPTY;
    uint8_t blocks[cfg::WORLD_H * cfg::CHUNK * cfg::CHUNK];
    SectionMesh sec[cfg::SECTIONS];

    static int index(int x, int y, int z) { return (y * cfg::CHUNK + z) * cfg::CHUNK + x; }
    uint8_t get(int x, int y, int z) const { return blocks[index(x, y, z)]; }
    void set(int x, int y, int z, uint8_t b) { blocks[index(x, y, z)] = b; }
};

#endif
