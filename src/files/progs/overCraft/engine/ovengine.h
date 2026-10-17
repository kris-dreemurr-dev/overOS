#ifndef OVENGINE_H
#define OVENGINE_H
// =============================================================================
// ovengine.h — интерфейс 3D-движка overOS (библиотека ovengine.ll).
//
// Движок — отдельная библиотека .ll в папке игры. Наружу он отдаёт функции
// oe_* с привязкой C: таблица .ll ищет символы по имени, а имена C++ искажены
// (mangling). Типы ниже общие для движка и игры: оба собираются с этим же
// заголовком и math3d.h, поэтому раскладка структур совпадает.
//
// Игра включает не этот файл напрямую, а ovengine_ll.h: он загружает
// библиотеку, связывает функции и даёт C++-обёртки ov::Renderer.
//
// Кадр (версия 2+) — отложенный и многопоточный:
//     oe_begin(камера)  ->  oe_draw(сетка, матрица) ...  ->  oe_end()
// oe_draw только запоминает сетку; вся работа — в oe_end, на всех ядрах:
// вершины и отсечение по сеткам, раскладка треугольников по полосам экрана,
// растеризация полос (небо, z-буфер, текстура, затенение углов, туман).
// Сетки должны жить до конца oe_end.
//
// Потоки движка — рабочие ядра процессора (ov_worker_start) плюс основной.
// Ими же может пользоваться игра: oe_parallel раздаёт задачи по всем.
// Задачи не должны выделять память и звать систему: на рабочих ядрах доступно
// только ожидание (см. ovos.h, «Рабочие ядра»).
//
// Что движку не принадлежит: сетки, мир, камера, ввод — это дело игры.
// =============================================================================

#include <stdint.h>
#include "math3d.h"

#define OE_VERSION 6

// Коды ошибок: свои -100.., от декодера PNG -1..-10 (libgfx/png.h)
#define OE_ERR_OPEN    -100   // файл не открылся
#define OE_ERR_READ    -101   // файл не прочитался целиком
#define OE_ERR_SIZE    -102   // картинка больше 2048x2048 или не степень двойки
#define OE_ERR_MEM     -103   // в куче движка не хватило места
#define OE_ERR_FULL    -104   // текстур уже OE_MAX_TEXTURES

#define OE_MAX_TEXTURES 64

// Флаги oe_draw_ex
#define OE_DRAW_OVERLAY 1     // поверх мира: своя проекция, z-буфер сброшен (рука от первого лица)
#define OE_MAX_THREADS  17    // основной + до 16 рабочих ядер

namespace ov {

struct Vertex {
    Vec3 pos;
    Vec2 uv;                // (0,0) — левый верхний угол; больше 1 — текстура повторяется
    float shade;            // яркость вершины 0..1 (затенение углов), по грани линейно
};

// Треугольник: вершины против часовой, если смотреть на лицевую сторону
struct Face {
    uint16_t a, b, c;
    uint16_t tex;           // номер текстуры у движка
    Vec3 normal;            // нормаль грани в координатах модели
};

struct Mesh {
    const Vertex* v = nullptr;
    int nv = 0;
    const Face* f = nullptr;
    int nf = 0;
};

// Направленный свет: dir — куда светит, ambient — доля рассеянного (0..1).
// ambient = 1 — освещения по нормали нет, только яркость вершин.
struct Light {
    Vec3 dir = Vec3(-0.4f, -1.0f, -0.6f);
    float ambient = 0.35f;
};

// Всё, что нужно кадру помимо сеток
struct Camera {
    Mat4 view;
    Mat4 proj;
    Light light;
    uint32_t sky_top = 0x6FA8E8;      // небо: градиент сверху вниз
    uint32_t sky_bottom = 0xC8DCF0;
    float fog_start = 1e9f;           // туман к цвету неба, по расстоянию от камеры
    float fog_end = 2e9f;
    // Проекция для OE_DRAW_OVERLAY: сетка задаётся сразу в координатах
    // камеры (матрица модели — от глаза), вида нет. Своя, потому что рука
    // не должна меняться вместе с FOV мира (бег, приближение)
    Mat4 overlay_proj = Mat4::identity();
};

// Сколько стоил последний кадр
struct FrameStats {
    int threads;            // потоков всего (основной + рабочие ядра)
    int submitted;          // треугольников отдано
    int drawn;              // дошло до экрана (после отсечения и задних граней)
    int dropped;            // не влезло в буфер кадра
    float ms_geometry;      // вершины, отсечение, раскладка по полосам
    float ms_raster;        // заполнение пикселей
    float ms_total;
    float thread_busy[OE_MAX_THREADS];   // доля кадра, которую поток работал (0..1)
};

// Задача для oe_parallel: index — номер задачи, thread — номер потока (0 — основной)
typedef void (*JobFn)(void* ctx, int index, int thread);

} // namespace ov

struct oe_renderer;         // непрозрачный: устройство знает только движок

#ifdef OVENGINE_BUILD
// Это объявления для самой библиотеки. Игре они недоступны напрямую:
// ovengine_ll.h подменяет эти имена указателями из таблицы.
extern "C" {
int          oe_version(void);
oe_renderer* oe_create(int max_threads);       // -1 — все свободные ядра
void         oe_destroy(oe_renderer* r);
int          oe_threads(const oe_renderer* r);
// percent — в какой доле разрешения окна рисовать мир (100 — полное, 25..99 —
// меньше, с увеличением в конце кадра: динамическое разрешение для слабого ПК)
int          oe_set_target(oe_renderer* r, uint32_t* fb, int w, int h, int percent);
int          oe_texture_load(oe_renderer* r, const char* png_path);   // номер или OE_ERR_*
int          oe_texture_add(oe_renderer* r, int w, int h, const uint32_t* px);
uint32_t     oe_texture_color(const oe_renderer* r, int tex);         // средний цвет (значки)
void         oe_begin(oe_renderer* r, const ov::Camera* cam);
void         oe_draw(oe_renderer* r, const ov::Mesh* mesh, const ov::Mat4* model);
void         oe_draw_ex(oe_renderer* r, const ov::Mesh* mesh, const ov::Mat4* model, int flags);   // v4
// Пиксели текстуры (0xAARRGGBB) — для значков в интерфейсе игры; v4
const uint32_t* oe_texture_pixels(const oe_renderer* r, int tex, int* w, int* h);
// z-буфер последнего кадра (1/w, 0 — небо; рука — поверх) и его размер — для
// рамки блока и прочего, что игра рисует сама поверх кадра; v5
const float* oe_depth(const oe_renderer* r, int* w, int* h);
// Куча движка (байты): занято, всего, самый большой свободный кусок; v6
void         oe_memory(uint64_t* used, uint64_t* total, uint64_t* largest_free);
void         oe_end(oe_renderer* r);
void         oe_stats(const oe_renderer* r, ov::FrameStats* out);
void         oe_parallel(oe_renderer* r, int count, ov::JobFn fn, void* ctx);
const char*  oe_error_text(int code);
}
#endif

#endif
