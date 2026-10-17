// =============================================================================
// world.cpp — чанки вокруг игрока (см. world.h).
// =============================================================================

#include "world.h"
#include "blocks.h"

using namespace cfg;
using ov::Vec3;
using ov::Mat4;

namespace {
inline int fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int fmod_i(int a, int b) { return a - fdiv(a, b) * b; }
inline int ifloor(float v) { int i = (int)v; return v < (float)i ? i - 1 : i; }
const int NO_CHUNK = 1 << 30;
}

bool World::init(ov::Renderer* r) {
    r_ = r;
    threads_ = r->threads();
    chunks_ = new Chunk[D * D];
    for (int i = 0; i < D * D; i++) { chunks_[i].cx = NO_CHUNK; chunks_[i].cz = NO_CHUNK; }
    scratch_ = new MeshScratch[threads_];
    edits_cap_ = 4096;
    edits_ = new BlockEdit[edits_cap_];

    const float lim = ((float)VIEW_RADIUS + 0.5f) * ((float)VIEW_RADIUS + 0.5f);
    norder_ = 0;
    for (int dz = -VIEW_RADIUS; dz <= VIEW_RADIUS; dz++)
        for (int dx = -VIEW_RADIUS; dx <= VIEW_RADIUS; dx++)
            if ((float)(dx * dx + dz * dz) <= lim) { order_[norder_][0] = dx; order_[norder_][1] = dz; norder_++; }
    for (int i = 1; i < norder_; i++) {                 // по расстоянию
        int x = order_[i][0], z = order_[i][1], d = x * x + z * z, j = i - 1;
        while (j >= 0 && order_[j][0] * order_[j][0] + order_[j][1] * order_[j][1] > d) {
            order_[j + 1][0] = order_[j][0]; order_[j + 1][1] = order_[j][1]; j--;
        }
        order_[j + 1][0] = x; order_[j + 1][1] = z;
    }
    return chunks_ && scratch_ && edits_;
}

Chunk* World::slot(int cx, int cz) { return &chunks_[fmod_i(cx, D) * D + fmod_i(cz, D)]; }

const Chunk* World::find(int cx, int cz) const {
    const Chunk* c = &chunks_[fmod_i(cx, D) * D + fmod_i(cz, D)];
    return (c->cx == cx && c->cz == cz && c->state != CHUNK_EMPTY) ? c : nullptr;
}
const Chunk* World::find_generated(int cx, int cz) const { return find(cx, cz); }

int World::chunks_loaded() const {
    int n = 0;
    for (int i = 0; i < D * D; i++) if (chunks_[i].state != CHUNK_EMPTY) n++;
    return n;
}

void World::mark_dirty_around(int cx, int cz) {
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            Chunk* c = const_cast<Chunk*>(find(cx + dx, cz + dz));
            if (!c) continue;
            for (int s = 0; s < SECTIONS; s++) c->sec[s].dirty = true;
            if (c->state == CHUNK_MESHED) c->state = CHUNK_GENERATED;
        }
}

// ------------------------------------------------------------ задачи
void World::gen_job(void* ctx, int i, int thread) {
    (void)thread;
    World* w = (World*)ctx;
    gen::fill(*w->batch_[i], w->edits_, w->nedits_);
}

void World::mesh_job(void* ctx, int i, int thread) {
    World* w = (World*)ctx;
    Chunk* c = w->batch_[i];
    const Chunk* nb[3][3];
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) nb[dz + 1][dx + 1] = w->find(c->cx + dx, c->cz + dz);
    mesher::build(*c, nb, w->scratch_[thread]);
}

// Кандидаты по близости к игроку: квадрат небольшой, хватает вставками
struct Cand { int cx, cz, d2; };
static void sort_cands(Cand* a, int n) {
    for (int i = 1; i < n; i++) {
        Cand k = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].d2 > k.d2) { a[j + 1] = a[j]; j--; }
        a[j + 1] = k;
    }
}

void World::update(const Vec3& pos) {
    const int pcx = fdiv(ifloor(pos.x), CHUNK), pcz = fdiv(ifloor(pos.z), CHUNK);
    static Cand cand[D * D];

    // 1. Генерация: ближайшие недостающие чанки в радиусе L
    int nc = 0;
    for (int dz = -L; dz <= L; dz++)
        for (int dx = -L; dx <= L; dx++)
            if (!find(pcx + dx, pcz + dz)) cand[nc++] = { pcx + dx, pcz + dz, dx * dx + dz * dz };
    sort_cands(cand, nc);
    nbatch_ = 0;
    for (int i = 0; i < nc && nbatch_ < GEN_PER_FRAME && nbatch_ < 64; i++) {
        Chunk* c = slot(cand[i].cx, cand[i].cz);       // далёкий чанк уступает слот
        c->cx = cand[i].cx;
        c->cz = cand[i].cz;
        c->state = CHUNK_EMPTY;
        for (int s = 0; s < SECTIONS; s++) {
            c->sec[s].dirty = true;
            c->sec[s].nv = c->sec[s].nf = 0;
            c->sec[s].mesh = ov::Mesh();
            for (int d = 0; d < 6; d++) c->sec[s].dir[d] = ov::Mesh();
        }
        batch_[nbatch_++] = c;
    }
    if (nbatch_) {
        r_->parallel(nbatch_, gen_job, this);
        for (int i = 0; i < nbatch_; i++) batch_[i]->state = CHUNK_GENERATED;
        int n = nbatch_;
        Chunk* done[64];
        for (int i = 0; i < n; i++) done[i] = batch_[i];
        for (int i = 0; i < n; i++) mark_dirty_around(done[i]->cx, done[i]->cz);
    }

    // 2. Сетки: видимые чанки с грязными секциями, у которых есть все 8 соседей
    nc = 0;
    for (int dz = -VIEW_RADIUS; dz <= VIEW_RADIUS; dz++)
        for (int dx = -VIEW_RADIUS; dx <= VIEW_RADIUS; dx++) {
            const Chunk* c = find(pcx + dx, pcz + dz);
            if (!c || c->state == CHUNK_MESHED) continue;
            bool all = true;
            for (int nz = -1; nz <= 1 && all; nz++)
                for (int nx = -1; nx <= 1 && all; nx++)
                    if (!find(c->cx + nx, c->cz + nz)) all = false;
            if (all) cand[nc++] = { c->cx, c->cz, dx * dx + dz * dz };
        }
    sort_cands(cand, nc);
    nbatch_ = 0;
    for (int i = 0; i < nc && nbatch_ < MESH_PER_FRAME && nbatch_ < 64; i++)
        batch_[nbatch_++] = slot(cand[i].cx, cand[i].cz);
    // Два захода: в первом секции без буфера только считают, сколько им надо;
    // основной поток выделяет память, во втором они строятся
    for (int pass = 0; pass < 2 && nbatch_; pass++) {
        r_->parallel(nbatch_, mesh_job, this);
        int keep = 0;
        for (int i = 0; i < nbatch_; i++) {
            Chunk* c = batch_[i];
            bool clean = true;
            for (int s = 0; s < SECTIONS; s++) {
                SectionMesh& m = c->sec[s];
                if (!m.dirty) continue;
                clean = false;
                if (m.need_v > m.cap_v || m.need_f > m.cap_f)
                    m.grow(m.need_v + m.need_v / 4 + 64, m.need_f + m.need_f / 4 + 32);
            }
            if (clean) c->state = CHUNK_MESHED;
            else batch_[keep++] = c;
        }
        nbatch_ = keep;
    }
}

bool World::ready_at(const Vec3& pos) const {
    const Chunk* c = find(fdiv(ifloor(pos.x), CHUNK), fdiv(ifloor(pos.z), CHUNK));
    return c && c->state == CHUNK_MESHED;
}

int World::progress_percent(const Vec3& pos) const {
    const int pcx = fdiv(ifloor(pos.x), CHUNK), pcz = fdiv(ifloor(pos.z), CHUNK);
    int done = 0, total = 0;
    for (int dz = -VIEW_RADIUS; dz <= VIEW_RADIUS; dz++)
        for (int dx = -VIEW_RADIUS; dx <= VIEW_RADIUS; dx++) {
            total++;
            const Chunk* c = find(pcx + dx, pcz + dz);
            if (c && c->state == CHUNK_MESHED) done++;
        }
    return total ? done * 100 / total : 100;
}

// ------------------------------------------------------------ блоки
uint8_t World::get(int x, int y, int z) const {
    if (y < 0) return BEDROCK;
    if (y >= WORLD_H) return AIR;
    const Chunk* c = find(fdiv(x, CHUNK), fdiv(z, CHUNK));
    if (!c) return AIR;
    return c->get(fmod_i(x, CHUNK), y, fmod_i(z, CHUNK));
}

bool World::solid(int x, int y, int z) const {
    if (y < 0) return true;
    if (y >= WORLD_H) return false;
    const Chunk* c = find(fdiv(x, CHUNK), fdiv(z, CHUNK));
    if (!c) return true;                       // ещё не построено: стена, а не пропасть
    return block_info(c->get(fmod_i(x, CHUNK), y, fmod_i(z, CHUNK))).solid;
}

bool World::set(int x, int y, int z, uint8_t b) {
    if (y < 1 || y >= WORLD_H) return false;  // коренную породу на дне не трогаем
    int cx = fdiv(x, CHUNK), cz = fdiv(z, CHUNK);
    Chunk* c = const_cast<Chunk*>(find(cx, cz));
    if (!c) return false;
    int lx = fmod_i(x, CHUNK), lz = fmod_i(z, CHUNK);
    c->set(lx, y, lz, b);

    // Запомнить правку (та же клетка — заменить)
    int i = 0;
    while (i < nedits_ && !(edits_[i].x == x && edits_[i].y == y && edits_[i].z == z)) i++;
    if (i == nedits_) {
        if (nedits_ == edits_cap_) {
            BlockEdit* ne = new BlockEdit[edits_cap_ * 2];
            for (int k = 0; k < nedits_; k++) ne[k] = edits_[k];
            delete[] edits_;
            edits_ = ne;
            edits_cap_ *= 2;
        }
        nedits_++;
    }
    edits_[i] = { x, y, z, b };

    // Перестроить: свою секцию и соседние по высоте, а у края чанка — и у соседей
    int sy = y / SECTION_H;
    bool edge = lx == 0 || lx == CHUNK - 1 || lz == 0 || lz == CHUNK - 1;
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            if ((dx || dz) && !edge) continue;
            Chunk* n = const_cast<Chunk*>(find(cx + dx, cz + dz));
            if (!n) continue;
            for (int s = sy - 1; s <= sy + 1; s++) if (s >= 0 && s < SECTIONS) n->sec[s].dirty = true;
            if (n->state == CHUNK_MESHED) n->state = CHUNK_GENERATED;
        }
    return true;
}

// ------------------------------------------------------------ отрисовка
namespace {
struct Plane { float a, b, c, d; };

// Плоскости пирамиды видимости из матрицы вид*проекция (Gribb–Hartmann)
void frustum(const Mat4& m, Plane p[5]) {
    const float* r = m.m;
    for (int i = 0; i < 4; i++) {
        int axis = i / 2;
        float s = (i & 1) ? -1.0f : 1.0f;
        p[i] = { r[12] + s * r[axis * 4 + 0], r[13] + s * r[axis * 4 + 1],
                 r[14] + s * r[axis * 4 + 2], r[15] + s * r[axis * 4 + 3] };
    }
    p[4] = { r[12] + r[8], r[13] + r[9], r[14] + r[10], r[15] + r[11] };   // ближняя
}

bool box_visible(const Plane p[5], float x0, float y0, float z0, float x1, float y1, float z1) {
    for (int i = 0; i < 5; i++) {
        float x = p[i].a >= 0 ? x1 : x0, y = p[i].b >= 0 ? y1 : y0, z = p[i].c >= 0 ? z1 : z0;
        if (p[i].a * x + p[i].b * y + p[i].c * z + p[i].d < 0) return false;
    }
    return true;
}
}

void World::draw(ov::Renderer& r, const Mat4& vp, const Vec3& eye) {
    Plane pl[5];
    frustum(vp, pl);
    const int pcx = fdiv(ifloor(eye.x), CHUNK), pcz = fdiv(ifloor(eye.z), CHUNK);
    drawn_ = 0;
    // Круг, а не квадрат (углы всё равно в тумане), от ближних к дальним
    for (int k = 0; k < norder_; k++) {
        int dx = order_[k][0], dz = order_[k][1];
        const Chunk* c = find(pcx + dx, pcz + dz);
        if (!c) continue;
        float x0 = (float)(c->cx * CHUNK), z0 = (float)(c->cz * CHUNK);
        Mat4 model = Mat4::translate(Vec3(x0, 0.0f, z0));
        for (int s = 0; s < SECTIONS; s++) {
            const SectionMesh& m = c->sec[s];
            if (m.mesh.nf <= 0) continue;
            float y0 = (float)(s * SECTION_H);
            float y1 = y0 + SECTION_H, x1 = x0 + CHUNK, z1 = z0 + CHUNK;
            if (!box_visible(pl, x0, y0, z0, x1, y1, z1)) continue;
            // Группа граней видна, только если глаз с её лицевой стороны хотя бы
            // от одной плоскости секции: верх не виден снизу, восток — с запада
            const bool vis[6] = { eye.x > x0, eye.x < x1, eye.y > y0, eye.y < y1, eye.z > z0, eye.z < z1 };
            for (int d = 0; d < 6; d++)
                if (vis[d] && m.dir[d].nf > 0) r.draw(m.dir[d], model);
            drawn_++;
        }
    }
}

// ------------------------------------------------------------ луч
// По клеткам сетки (Amanatides–Woo): от клетки к клетке, всегда через
// ближайшую грань — ни одного блока на пути не пропустить
RayHit World::raycast(const Vec3& o, const Vec3& d, float max_dist) const {
    RayHit h;
    int x = ifloor(o.x), y = ifloor(o.y), z = ifloor(o.z);
    int sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
    const float big = 1e30f;
    float tdx = d.x != 0.0f ? ov::fabs(1.0f / d.x) : big;
    float tdy = d.y != 0.0f ? ov::fabs(1.0f / d.y) : big;
    float tdz = d.z != 0.0f ? ov::fabs(1.0f / d.z) : big;
    float tx = d.x != 0.0f ? ((sx > 0 ? (float)(x + 1) - o.x : o.x - (float)x) * tdx) : big;
    float ty = d.y != 0.0f ? ((sy > 0 ? (float)(y + 1) - o.y : o.y - (float)y) * tdy) : big;
    float tz = d.z != 0.0f ? ((sz > 0 ? (float)(z + 1) - o.z : o.z - (float)z) * tdz) : big;
    int px = x, py = y, pz = z;
    float t = 0.0f;
    while (t <= max_dist) {
        uint8_t b = get(x, y, z);
        if (b != AIR && b != WATER) {
            h.hit = true;
            h.x = x; h.y = y; h.z = z;
            h.px = px; h.py = py; h.pz = pz;
            h.dist = t;
            return h;
        }
        px = x; py = y; pz = z;
        if (tx < ty && tx < tz) { x += sx; t = tx; tx += tdx; }
        else if (ty < tz)       { y += sy; t = ty; ty += tdy; }
        else                    { z += sz; t = tz; tz += tdz; }
    }
    return h;
}
