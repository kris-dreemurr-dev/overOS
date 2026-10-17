// =============================================================================
// mesher.cpp — построение сеток чанка (см. mesher.h).
// =============================================================================

#include "mesher.h"
#include "blocks.h"

using namespace cfg;
using ov::Vec2;
using ov::Vec3;

void SectionMesh::grow(int vcap, int fcap) {
    delete[] v;
    delete[] f;
    if (vcap > 65532) vcap = 65532;                 // номера вершин в Face — 16 бит
    v = new ov::Vertex[vcap];
    f = new ov::Face[fcap];
    cap_v = vcap;
    cap_f = fcap;
    nv = nf = 0;
    mesh = ov::Mesh();
    for (int d = 0; d < 6; d++) dir[d] = ov::Mesh();
}

void SectionMesh::release() {
    delete[] v;
    delete[] f;
    v = nullptr;
    f = nullptr;
    cap_v = cap_f = nv = nf = 0;
    mesh = ov::Mesh();
    for (int d = 0; d < 6; d++) dir[d] = ov::Mesh();
}

namespace mesher {

namespace {

struct Dir {
    int n[3], u[3], r[3];
    float light;            // грани по сторонам света темнее верха: объём без источников
};
// +X, -X, +Y, -Y, +Z, -Z. r = u x n: обход bl, br, tr, tl — против часовой снаружи
const Dir DIRS[6] = {
    { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, 0.80f },
    { { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, 0.80f },
    { { 0, 1, 0 }, { 0, 0, -1 }, { 1, 0, 0 }, 1.00f },
    { { 0, -1, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, 0.50f },
    { { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 }, 0.68f },
    { { 0, 0, -1 }, { 0, 1, 0 }, { -1, 0, 0 }, 0.68f },
};
const int CORNER[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };   // (знак r, знак u)
const float CORNER_UV[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
const float AO_LEVEL[4] = { 0.50f, 0.68f, 0.84f, 1.0f };

struct Tables {
    bool opaque[256];       // закрывает грань соседа
    bool occl[256];         // затеняет углы
    Tables() {
        for (int i = 0; i < 256; i++) { opaque[i] = false; occl[i] = false; }
        for (int i = 0; i < BLOCK_COUNT; i++) {
            opaque[i] = BLOCKS[i].opaque && i != WATER;
            occl[i] = BLOCKS[i].solid && i != GLASS;
        }
    }
};
// Глобальный объект: строится в ovcxx_init при запуске, а не при первом
// вызове — первый вызов может прийти сразу с нескольких ядер, а статики
// внутри функций у нас без защиты (-fno-threadsafe-statics)
const Tables g_tables;
inline const Tables& tables() { return g_tables; }

inline bool face_visible(uint8_t b, uint8_t n, const Tables& t) {
    if (b == WATER) return n == AIR;                    // вода видна только с воздуха
    if (n == b && !t.opaque[b]) return false;           // листва к листве, стекло к стеклу
    return !t.opaque[n];
}

} // namespace

// Выход одного прямоугольника грани: углы — по плоскости грани, порядок —
// как у одиночной грани (bl, br, tr, tl против часовой снаружи), UV — в
// блоках, чтобы текстура повторялась по каждому блоку прямоугольника.
struct Out {
    SectionMesh& m;
    int nv = 0, nf = 0, need_v = 0, need_f = 0;
    int base_v = 0;                     // начало группы текущего направления
    int start_v[7] = {}, start_f[7] = {};
    explicit Out(SectionMesh& mm) : m(mm) {}

    void begin_dir(int d) { base_v = nv; start_v[d] = nv; start_f[d] = nf; }
    void finish() { start_v[6] = nv; start_f[6] = nf; }

    void quad(const Dir& D, const int P[4][3], const float uv[4][2], const int ao[4], uint16_t tex) {
        need_v += 4;
        need_f += 2;
        if (nv + 4 > m.cap_v || nf + 2 > m.cap_f) return;      // не влезло — посчитали и хватит
        for (int k = 0; k < 4; k++) {
            ov::Vertex& v = m.v[nv + k];
            v.pos = Vec3((float)P[k][0], (float)P[k][1], (float)P[k][2]);
            v.uv = Vec2(uv[k][0], uv[k][1]);
            v.shade = AO_LEVEL[ao[k]] * D.light;
        }
        Vec3 normal((float)D.n[0], (float)D.n[1], (float)D.n[2]);
        uint16_t b0 = (uint16_t)(nv - base_v);   // номера — от начала группы направления
        // Диагональ — между более светлыми углами, иначе тень «ломается»
        bool flip = ao[0] + ao[2] < ao[1] + ao[3];
        static const int A[2][6] = { { 0, 1, 2, 0, 2, 3 }, { 1, 2, 3, 1, 3, 0 } };
        const int* q = A[flip ? 1 : 0];
        for (int k = 0; k < 2; k++) {
            ov::Face& f = m.f[nf + k];
            f.a = (uint16_t)(b0 + q[k * 3 + 0]);
            f.b = (uint16_t)(b0 + q[k * 3 + 1]);
            f.c = (uint16_t)(b0 + q[k * 3 + 2]);
            f.tex = tex;
            f.normal = normal;
        }
        nv += 4;
        nf += 2;
    }
};

// Ячейка маски слоя: какая грань здесь видна
struct Cell {
    uint16_t tex;           // 0xFFFF — грани нет
    uint8_t ao[4];          // затенение углов в порядке bl, br, tr, tl
    bool uniform;           // все углы одинаковые — можно сливать с соседями
    bool used;
};

void build(Chunk& c, const Chunk* const nb[3][3], MeshScratch& s) {
    const Tables& t = tables();

    // Рамка: свой чанк + по клетке соседей; ниже мира — камень, выше — воздух
    for (int y = -1; y <= WORLD_H; y++)
        for (int z = -1; z <= CHUNK; z++)
            for (int x = -1; x <= CHUNK; x++) {
                uint8_t b;
                if (y < 0) b = STONE;
                else if (y >= WORLD_H) b = AIR;
                else {
                    int dx = x < 0 ? -1 : (x >= CHUNK ? 1 : 0);
                    int dz = z < 0 ? -1 : (z >= CHUNK ? 1 : 0);
                    const Chunk* ch = nb[dz + 1][dx + 1];
                    b = ch ? ch->get(x - dx * CHUNK, y, z - dz * CHUNK) : (uint8_t)STONE;
                }
                s.pad[MeshScratch::at(x, y, z)] = b;
            }

    for (int si = 0; si < SECTIONS; si++) {
        SectionMesh& m = c.sec[si];
        if (!m.dirty) continue;
        Out out(m);
        const int lo[3] = { 0, si * SECTION_H, 0 };
        const int dim[3] = { CHUNK, SECTION_H, CHUNK };

        for (int d = 0; d < 6; d++) {
            const Dir& D = DIRS[d];
            out.begin_dir(d);
            // Оси: A — вдоль нормали, B и C — в плоскости грани
            const int A = D.n[0] ? 0 : (D.n[1] ? 1 : 2);
            const int B = (A + 1) % 3, C = (A + 2) % 3;
            const int nb_ = dim[B], nc = dim[C];
            for (int sl = 0; sl < dim[A]; sl++) {
                // 1. Маска слоя: видимые грани и их углы
                Cell mask[CHUNK * CHUNK];
                for (int cc = 0; cc < nc; cc++)
                    for (int bb = 0; bb < nb_; bb++) {
                        Cell& cell = mask[cc * nb_ + bb];
                        cell.tex = 0xFFFF;
                        cell.used = false;
                        int p[3];
                        p[A] = lo[A] + sl; p[B] = lo[B] + bb; p[C] = lo[C] + cc;
                        uint8_t b = s.pad[MeshScratch::at(p[0], p[1], p[2])];
                        if (b == AIR) continue;
                        int ox = p[0] + D.n[0], oy = p[1] + D.n[1], oz = p[2] + D.n[2];
                        if (!face_visible(b, s.pad[MeshScratch::at(ox, oy, oz)], t)) continue;
                        const BlockInfo& bi = block_info(b);
                        cell.tex = d == 2 ? bi.top : (d == 3 ? bi.bottom : bi.side);
                        for (int k = 0; k < 4; k++) {
                            int sr = CORNER[k][0], su = CORNER[k][1];
                            int s1 = t.occl[s.pad[MeshScratch::at(ox + D.r[0] * sr, oy + D.r[1] * sr, oz + D.r[2] * sr)]];
                            int s2 = t.occl[s.pad[MeshScratch::at(ox + D.u[0] * su, oy + D.u[1] * su, oz + D.u[2] * su)]];
                            int c3 = t.occl[s.pad[MeshScratch::at(ox + D.r[0] * sr + D.u[0] * su,
                                                                  oy + D.r[1] * sr + D.u[1] * su,
                                                                  oz + D.r[2] * sr + D.u[2] * su)]];
                            cell.ao[k] = (uint8_t)((s1 && s2) ? 0 : 3 - (s1 + s2 + c3));
                        }
                        cell.uniform = cell.ao[0] == cell.ao[1] && cell.ao[1] == cell.ao[2] && cell.ao[2] == cell.ao[3];
                    }

                // 2. Жадное слияние: прямоугольник растёт по B, затем по C,
                // пока ячейки той же текстуры с тем же ровным затенением
                const int pa = lo[A] + sl + (D.n[A] > 0 ? 1 : 0);   // плоскость грани
                for (int cc = 0; cc < nc; cc++)
                    for (int bb = 0; bb < nb_; bb++) {
                        Cell& c0 = mask[cc * nb_ + bb];
                        if (c0.tex == 0xFFFF || c0.used) continue;
                        int w = 1, h = 1;
                        if (c0.uniform) {
                            auto same = [&](const Cell& o) {
                                return !o.used && o.tex == c0.tex && o.uniform && o.ao[0] == c0.ao[0];
                            };
                            while (bb + w < nb_ && same(mask[cc * nb_ + bb + w])) w++;
                            bool grow = true;
                            while (grow && cc + h < nc) {
                                for (int k = 0; k < w; k++)
                                    if (!same(mask[(cc + h) * nb_ + bb + k])) { grow = false; break; }
                                if (grow) h++;
                            }
                        }
                        for (int j = 0; j < h; j++)
                            for (int k = 0; k < w; k++) mask[(cc + j) * nb_ + bb + k].used = true;

                        // Углы прямоугольника и их место (bl, br, tr, tl) по осям r и u
                        int Pq[4][3];
                        float uv[4][2];
                        int ao[4];
                        const int b0 = lo[B] + bb, b1 = b0 + w, c0i = lo[C] + cc, c1 = c0i + h;
                        const int ext[3] = { A == 0 ? 0 : (B == 0 ? w : h), A == 1 ? 0 : (B == 1 ? w : h),
                                             A == 2 ? 0 : (B == 2 ? w : h) };
                        const int lr = ext[0] * (D.r[0] != 0) + ext[1] * (D.r[1] != 0) + ext[2] * (D.r[2] != 0);
                        const int lu = ext[0] * (D.u[0] != 0) + ext[1] * (D.u[1] != 0) + ext[2] * (D.u[2] != 0);
                        const int corners[4][2] = { { b0, c0i }, { b1, c0i }, { b1, c1 }, { b0, c1 } };
                        // Середина (удвоенная), чтобы сравнивать без дробей
                        int mid2[3];
                        mid2[A] = 2 * pa; mid2[B] = b0 + b1; mid2[C] = c0i + c1;
                        for (int q = 0; q < 4; q++) {
                            int P[3];
                            P[A] = pa; P[B] = corners[q][0]; P[C] = corners[q][1];
                            int dr = 0, du = 0;
                            for (int ax = 0; ax < 3; ax++) {
                                dr += (2 * P[ax] - mid2[ax]) * D.r[ax];
                                du += (2 * P[ax] - mid2[ax]) * D.u[ax];
                            }
                            int sr = dr > 0 ? 1 : -1, su = du > 0 ? 1 : -1;
                            int k = 0;
                            while (CORNER[k][0] != sr || CORNER[k][1] != su) k++;
                            Pq[k][0] = P[0]; Pq[k][1] = P[1]; Pq[k][2] = P[2];
                            uv[k][0] = CORNER_UV[k][0] * (float)lr;
                            uv[k][1] = CORNER_UV[k][1] * (float)lu;
                            ao[k] = c0.ao[k];
                        }
                        out.quad(D, Pq, uv, ao, c0.tex);
                    }
            }
        }

        out.finish();
        // Не влезло — пусть основной поток расширит буфер (выше 65532 вершин
        // расти некуда: тогда берём, сколько влезло)
        if ((out.need_v > m.cap_v || out.need_f > m.cap_f) && m.cap_v < 65532) {
            m.need_v = out.need_v;
            m.need_f = out.need_f;
            m.nv = m.nf = 0;
            m.mesh = ov::Mesh();
            for (int d = 0; d < 6; d++) m.dir[d] = ov::Mesh();
            continue;                                   // останется грязной до расширения
        }
        m.nv = out.nv;
        m.nf = out.nf;
        m.need_v = m.need_f = 0;
        m.mesh.v = m.v;
        m.mesh.nv = out.nv;
        m.mesh.f = m.f;
        m.mesh.nf = out.nf;
        for (int d = 0; d < 6; d++) {
            ov::Mesh& g = m.dir[d];
            g.v = m.v + out.start_v[d];
            g.nv = out.start_v[d + 1] - out.start_v[d];
            g.f = m.f + out.start_f[d];
            g.nf = out.start_f[d + 1] - out.start_f[d];
        }
        m.dirty = false;
    }
}

} // namespace mesher
