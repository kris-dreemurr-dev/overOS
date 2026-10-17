#ifndef OVCRAFT_WORLD_H
#define OVCRAFT_WORLD_H
// =============================================================================
// world.h — мир вокруг игрока.
//
// Чанки лежат в квадрате (2L+1) x (2L+1) слотов, где L = VIEW_RADIUS + 1:
// слот выбирается по координатам чанка по модулю, поэтому при ходьбе далёкий
// чанк просто уступает свой слот новому, и ничего не двигается в памяти.
// Видны чанки в радиусе VIEW_RADIUS; кольцо за ним генерируется, но не
// рисуется: сетке нужны соседи (грани на границе и тени в углах).
//
// Каждый кадр update() берёт ближайшие к игроку недостающие чанки и
// раздаёт их движку на все ядра (oe_parallel): сначала генерацию, потом
// сетки. Всё, что эти задачи пишут, выделено заранее основным потоком.
//
// Правки игрока хранятся списком и накладываются при каждой генерации чанка,
// так что сломанный блок не возвращается, когда уходишь и приходишь обратно.
// =============================================================================

#include "chunk.h"
#include "generator.h"
#include "mesher.h"
#include "engine/ovengine_ll.h"

struct RayHit {
    bool hit = false;
    int x, y, z;            // блок, в который попали
    int px, py, pz;         // клетка перед ним (сюда ставится новый блок)
    float dist;
};

class World {
public:
    static const int L = cfg::VIEW_RADIUS + 1;
    static const int D = 2 * L + 1;

    bool init(ov::Renderer* r);

    // Подготовить чанки вокруг точки (генерация и сетки на всех ядрах)
    void update(const ov::Vec3& pos);
    bool ready_at(const ov::Vec3& pos) const;          // чанк под игроком построен
    int progress_percent(const ov::Vec3& pos) const;   // сколько видимых чанков готово

    uint8_t get(int x, int y, int z) const;            // незагруженное — AIR
    bool solid(int x, int y, int z) const;             // незагруженное — твёрдое (не провалиться)
    bool set(int x, int y, int z, uint8_t b);
    int surface(int x, int z) const { return gen::surface_height(x, z); }

    void draw(ov::Renderer& r, const ov::Mat4& view_proj, const ov::Vec3& eye);
    RayHit raycast(const ov::Vec3& from, const ov::Vec3& dir, float max_dist) const;

    int chunks_loaded() const;
    int sections_drawn() const { return drawn_; }

private:
    Chunk* slot(int cx, int cz);
    const Chunk* find(int cx, int cz) const;
    const Chunk* find_generated(int cx, int cz) const;
    void mark_dirty_around(int cx, int cz);

    static void gen_job(void* ctx, int i, int thread);
    static void mesh_job(void* ctx, int i, int thread);

    ov::Renderer* r_ = nullptr;
    Chunk* chunks_ = nullptr;                        // D * D слотов
    MeshScratch* scratch_ = nullptr;                 // по одной рамке на поток
    int threads_ = 1;
    BlockEdit* edits_ = nullptr;
    int nedits_ = 0, edits_cap_ = 0;
    Chunk* batch_[64];
    int nbatch_ = 0;
    int drawn_ = 0;
    // Чанки видимого круга от ближних к дальним: так z-буфер сразу отбрасывает
    // то, что закрыто ближними холмами, и дальние пиксели не красятся зря
    int order_[(2 * cfg::VIEW_RADIUS + 1) * (2 * cfg::VIEW_RADIUS + 1)][2];
    int norder_ = 0;
};

#endif
