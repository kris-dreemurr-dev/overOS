#ifndef OVCRAFT_MODEL_H
#define OVCRAFT_MODEL_H
// =============================================================================
// model.h — модели мобов и игрока из файлов .mdl (resources/models/).
//
// Формат — текст, строка за строкой (# — комментарий):
//     texture W H              размер развёртки в пикселях
//     part ИМЯ px py pz        новая часть и точка, вокруг которой она вращается
//     box x y z  w h d  u v    коробка части: угол, размер, начало развёртки
// Единицы — пиксели модели (16 = блок), y = 0 — ступни, перед смотрит на -Z.
// Развёртка коробки — как в Minecraft:
//     [   d   ][ верх w ][ низ w ]
//     [право d][перед w][лево d][зад w]     (высота второй полосы — h)
//
// Сетка каждой части строится один раз при загрузке, в координатах от её
// точки вращения. Двигает части Pose: поворот (и сдвиг) на каждую часть —
// так у сетки ничего не пересчитывается, кадр только задаёт матрицы.
// У каждой части две сетки: обычная и «красная» (урон) — та же геометрия
// с другой текстурой.
// =============================================================================

#include "engine/ovengine_ll.h"

class Model {
public:
    static const int MAX_PARTS = 12;

    struct Part {
        char name[16];
        ov::Vec3 pivot;                 // пиксели модели
        ov::Mesh mesh, mesh_hurt;
    };

    // Повороты частей (радианы: x — кивок, y — поворот, z — наклон вбок) и сдвиги
    struct Pose {
        ov::Vec3 rot[MAX_PARTS];
        ov::Vec3 off[MAX_PARTS];        // пиксели
    };

    // tex / tex_hurt — номера текстур у движка. false — файла нет или он с ошибкой
    bool load(const char* rel_path, int tex, int tex_hurt);
    int find(const char* name) const;   // номер части или -1
    int parts() const { return nparts_; }
    const Part& part(int i) const { return parts_[i]; }

    // Матрица части: base — модель целиком в пикселях (см. world_matrix)
    ov::Mat4 part_matrix(const ov::Mat4& base, const Pose& p, int i) const;
    // Модель целиком: стоит в pos, смотрит по yaw, scale — как в entities.h
    static ov::Mat4 world_matrix(const ov::Vec3& pos, float yaw, float scale);

    void draw(ov::Renderer& r, const ov::Mat4& base, const Pose& p, bool hurt) const;

    const char* error() const { return err_; }

private:
    bool add_box(const float c[3], const float s[3], int u, int v);

    Part parts_[MAX_PARTS];
    int nparts_ = 0;
    int tw_ = 64, th_ = 64;
    int tex_ = 0, tex_hurt_ = 0;
    // Все вершины и грани модели в одном куске; части ссылаются на свои отрезки
    ov::Vertex* v_ = nullptr;
    ov::Face* f_ = nullptr;
    ov::Face* fh_ = nullptr;
    int nv_ = 0, nf_ = 0, cap_ = 0;
    const char* err_ = nullptr;
};

#endif
