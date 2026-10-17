// =============================================================================
// model.cpp — разбор .mdl и сетки коробок (см. model.h).
// =============================================================================

#include "model.h"
#include "files.h"

using ov::Vec2;
using ov::Vec3;
using ov::Mat4;

namespace {

// Числа без libc: целые и с точкой, со знаком
bool read_num(const char*& s, float& out) {
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;
    if ((*s < '0' || *s > '9') && *s != '.') return false;
    float v = 0.0f;
    while (*s >= '0' && *s <= '9') v = v * 10.0f + (float)(*s++ - '0');
    if (*s == '.') {
        s++;
        float k = 0.1f;
        while (*s >= '0' && *s <= '9') { v += (float)(*s++ - '0') * k; k *= 0.1f; }
    }
    out = neg ? -v : v;
    return true;
}

bool read_word(const char*& s, char* out, int max) {
    while (*s == ' ' || *s == '\t') s++;
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '#') {
        if (n < max - 1) out[n++] = *s;
        s++;
    }
    out[n] = '\0';
    return n > 0;
}

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Освещение граней «запечено» в вершины: верх светлее, низ темнее
const float SHADE_TOP = 1.0f, SHADE_BOTTOM = 0.5f, SHADE_FRONT = 0.82f, SHADE_SIDE = 0.66f;

} // namespace

// Коробка: 6 граней по 4 вершины. Углы каждой грани — bl, br, tr, tl, как их
// видно снаружи (против часовой), и прямоугольник развёртки (u, v, w, h):
// левый верхний угол картинки — у tl
bool Model::add_box(const float c[3], const float s[3], int u, int v) {
    if (nv_ + 24 > cap_) return false;
    Part& p = parts_[nparts_ - 1];
    const float x0 = c[0] - p.pivot.x, y0 = c[1] - p.pivot.y, z0 = c[2] - p.pivot.z;
    const float x1 = x0 + s[0], y1 = y0 + s[1], z1 = z0 + s[2];
    const int w = (int)s[0], h = (int)s[1], d = (int)s[2];
    struct F { Vec3 q[4]; int ru, rv, rw, rh; float shade; Vec3 n; };
    const F faces[6] = {
        // перед (-Z): смотрящему справа — -X
        { { Vec3(x1, y0, z0), Vec3(x0, y0, z0), Vec3(x0, y1, z0), Vec3(x1, y1, z0) }, u + d, v + d, w, h, SHADE_FRONT, Vec3(0, 0, -1) },
        // зад (+Z)
        { { Vec3(x0, y0, z1), Vec3(x1, y0, z1), Vec3(x1, y1, z1), Vec3(x0, y1, z1) }, u + 2 * d + w, v + d, w, h, SHADE_FRONT, Vec3(0, 0, 1) },
        // правый бок модели (+X): смотрящему справа — перед
        { { Vec3(x1, y0, z1), Vec3(x1, y0, z0), Vec3(x1, y1, z0), Vec3(x1, y1, z1) }, u, v + d, d, h, SHADE_SIDE, Vec3(1, 0, 0) },
        // левый бок (-X)
        { { Vec3(x0, y0, z0), Vec3(x0, y0, z1), Vec3(x0, y1, z1), Vec3(x0, y1, z0) }, u + d + w, v + d, d, h, SHADE_SIDE, Vec3(-1, 0, 0) },
        // верх: низ картинки — к переду
        { { Vec3(x1, y1, z0), Vec3(x0, y1, z0), Vec3(x0, y1, z1), Vec3(x1, y1, z1) }, u + d, v, w, d, SHADE_TOP, Vec3(0, 1, 0) },
        // низ: верх картинки — к переду
        { { Vec3(x1, y0, z1), Vec3(x0, y0, z1), Vec3(x0, y0, z0), Vec3(x1, y0, z0) }, u + d + w, v, w, d, SHADE_BOTTOM, Vec3(0, -1, 0) },
    };
    // Чуть внутрь прямоугольника: выборка ближайшего текселя на самой
    // границе иначе берёт соседний кусок развёртки
    const float e = 0.02f;
    for (int k = 0; k < 6; k++) {
        const F& f = faces[k];
        float u0 = ((float)f.ru + e) / (float)tw_, u1 = ((float)(f.ru + f.rw) - e) / (float)tw_;
        float v0 = ((float)f.rv + e) / (float)th_, v1 = ((float)(f.rv + f.rh) - e) / (float)th_;
        const Vec2 uv[4] = { Vec2(u0, v1), Vec2(u1, v1), Vec2(u1, v0), Vec2(u0, v0) };
        int base = nv_;
        for (int i = 0; i < 4; i++) {
            ov::Vertex& vx = v_[nv_++];
            vx.pos = f.q[i];
            vx.uv = uv[i];
            vx.shade = f.shade;
        }
        ov::Face t0, t1;
        t0.a = (uint16_t)base; t0.b = (uint16_t)(base + 1); t0.c = (uint16_t)(base + 2);
        t1.a = (uint16_t)base; t1.b = (uint16_t)(base + 2); t1.c = (uint16_t)(base + 3);
        t0.normal = t1.normal = f.n;
        t0.tex = t1.tex = (uint16_t)tex_;
        f_[nf_] = t0; f_[nf_ + 1] = t1;
        t0.tex = t1.tex = (uint16_t)tex_hurt_;
        fh_[nf_] = t0; fh_[nf_ + 1] = t1;
        nf_ += 2;
    }
    return true;
}

bool Model::load(const char* rel, int tex, int tex_hurt) {
    static char buf[8192];
    err_ = nullptr;
    if (res_read(rel, buf, sizeof(buf)) < 0) { err_ = "нет файла модели"; return false; }
    tex_ = tex;
    tex_hurt_ = tex_hurt;
    nparts_ = 0;
    // Места — с запасом на 32 коробки: модели маленькие
    const int cap = 32 * 24;
    delete[] v_; delete[] f_; delete[] fh_;
    v_ = new ov::Vertex[cap];
    f_ = new ov::Face[cap / 2];
    fh_ = new ov::Face[cap / 2];
    cap_ = cap;
    nv_ = nf_ = 0;
    int part_v = 0, part_f = 0;

    auto close_part = [&]() {
        if (!nparts_) return;
        Part& p = parts_[nparts_ - 1];
        p.mesh.v = v_ + part_v; p.mesh.nv = nv_ - part_v;
        p.mesh.f = f_ + part_f; p.mesh.nf = nf_ - part_f;
        p.mesh_hurt = p.mesh;
        p.mesh_hurt.f = fh_ + part_f;
        // Номера вершин в гранях — от начала части
        for (int i = part_f; i < nf_; i++) {
            f_[i].a -= (uint16_t)part_v; f_[i].b -= (uint16_t)part_v; f_[i].c -= (uint16_t)part_v;
            fh_[i].a -= (uint16_t)part_v; fh_[i].b -= (uint16_t)part_v; fh_[i].c -= (uint16_t)part_v;
        }
    };

    const char* s = buf;
    while (*s) {
        const char* line = s;
        while (*s && *s != '\n') s++;
        if (*s) s++;
        const char* q = line;
        char word[16];
        if (!read_word(q, word, sizeof(word)) || word[0] == '#') continue;
        if (same(word, "texture")) {
            float w, h;
            if (!read_num(q, w) || !read_num(q, h)) { err_ = "texture: нужны ширина и высота"; return false; }
            tw_ = (int)w; th_ = (int)h;
        } else if (same(word, "part")) {
            close_part();
            if (nparts_ >= MAX_PARTS) { err_ = "слишком много частей"; return false; }
            Part& p = parts_[nparts_++];
            if (!read_word(q, p.name, sizeof(p.name))) { err_ = "part: нет имени"; return false; }
            float x, y, z;
            if (!read_num(q, x) || !read_num(q, y) || !read_num(q, z)) { err_ = "part: нужна точка вращения"; return false; }
            p.pivot = Vec3(x, y, z);
            part_v = nv_;
            part_f = nf_;
        } else if (same(word, "box")) {
            if (!nparts_) { err_ = "box до первой part"; return false; }
            float c[3], sz[3], u, v;
            for (int i = 0; i < 3; i++) if (!read_num(q, c[i])) { err_ = "box: нужен угол"; return false; }
            for (int i = 0; i < 3; i++) if (!read_num(q, sz[i])) { err_ = "box: нужен размер"; return false; }
            if (!read_num(q, u) || !read_num(q, v)) { err_ = "box: нужно начало развёртки"; return false; }
            if (!add_box(c, sz, (int)u, (int)v)) { err_ = "слишком много коробок"; return false; }
        } else {
            err_ = "непонятная строка";
            return false;
        }
    }
    close_part();
    if (!nparts_) { err_ = "в модели нет частей"; return false; }
    return true;
}

int Model::find(const char* name) const {
    for (int i = 0; i < nparts_; i++) if (same(parts_[i].name, name)) return i;
    return -1;
}

Mat4 Model::world_matrix(const Vec3& pos, float yaw, float scale) {
    // yaw 0 — смотрит на -Z, как и игрок; пиксели модели -> блоки
    return Mat4::translate(pos) * Mat4::rotateY(-yaw) * Mat4::scale(scale / 16.0f);
}

Mat4 Model::part_matrix(const Mat4& base, const Pose& p, int i) const {
    const Part& pt = parts_[i];
    Vec3 at = pt.pivot + p.off[i];
    return base * Mat4::translate(at) * Mat4::rotateY(p.rot[i].y) * Mat4::rotateX(p.rot[i].x) * Mat4::rotateZ(p.rot[i].z);
}

void Model::draw(ov::Renderer& r, const Mat4& base, const Pose& p, bool hurt) const {
    for (int i = 0; i < nparts_; i++) r.draw(hurt ? parts_[i].mesh_hurt : parts_[i].mesh, part_matrix(base, p, i));
}
