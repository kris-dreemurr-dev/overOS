#ifndef OVENGINE_LL_H
#define OVENGINE_LL_H
// =============================================================================
// ovengine_ll.h — подключение ovengine.ll к игре.
//
// Библиотека лежит не в /lib, а в папке игры, рядом с её .prg: путь игра
// узнаёт сама (oe_game_dir) и передаёт в oe_bind. Дальше вызовы oe_* идут
// через таблицу указателей, а для C++-кода есть обёртка ov::Renderer:
//
//     char dir[96];
//     oe_game_dir(dir, sizeof(dir));
//     if (oe_bind(dir) < 0) ov_exit(1);
//     ov::Renderer r;                                          // берёт рабочие ядра
//     int grass = r.loadTexture("resources/grass_top.png");   // от папки игры
// =============================================================================

#include "ovos.h"
#include "ovengine.h"

typedef struct {
    int lib;
    int          (*version)(void);
    oe_renderer* (*create)(int);
    void         (*destroy)(oe_renderer*);
    int          (*threads)(const oe_renderer*);
    int          (*set_target)(oe_renderer*, uint32_t*, int, int, int);
    int          (*texture_load)(oe_renderer*, const char*);
    int          (*texture_add)(oe_renderer*, int, int, const uint32_t*);
    uint32_t     (*texture_color)(const oe_renderer*, int);
    void         (*begin)(oe_renderer*, const ov::Camera*);
    void         (*draw)(oe_renderer*, const ov::Mesh*, const ov::Mat4*);
    void         (*draw_ex)(oe_renderer*, const ov::Mesh*, const ov::Mat4*, int);
    const uint32_t* (*texture_pixels)(const oe_renderer*, int, int*, int*);
    const float* (*depth)(const oe_renderer*, int*, int*);
    void         (*memory)(uint64_t*, uint64_t*, uint64_t*);
    void         (*end)(oe_renderer*);
    void         (*stats)(const oe_renderer*, ov::FrameStats*);
    void         (*parallel)(oe_renderer*, int, ov::JobFn, void*);
    const char*  (*error_text)(int);
} oe_api_t;

// Одна таблица на всю игру (слабые определения сливаются при компоновке,
// как g_ui в libui_ll.h): oe_bind в main заполняет её для всех файлов
__attribute__((weak)) oe_api_t g_oe;
__attribute__((weak)) char g_oe_dir[96];

// Папка, где лежит исполняемый файл игры: /desprogs/overCraft/ovcraft.prg ->
// /desprogs/overCraft. 0 — получилось.
static inline int oe_game_dir(char* out, int max) {
    char exe[96];
    if (ov_proc_exe(ov_getpid(), exe, sizeof(exe)) != 0) return -1;
    int last = -1;
    for (int i = 0; exe[i]; i++) if (exe[i] == '/') last = i;
    if (last <= 0) return -1;
    int n = last < max - 1 ? last : max - 1;
    for (int i = 0; i < n; i++) out[i] = exe[i];
    out[n] = '\0';
    return 0;
}

// Загрузить <dir>/ovengine.ll. 0 — готово, -1 — нет файла, -2 — нет символа,
// -3 — версия библиотеки не та, что у заголовка
static inline int oe_bind(const char* dir) {
    for (unsigned i = 0; i < sizeof(g_oe); i++) ((char*)&g_oe)[i] = 0;
    ov_strlcpy(g_oe_dir, dir, sizeof(g_oe_dir));
    char path[128];
    ov_strlcpy(path, dir, sizeof(path));
    int n = 0;
    while (path[n]) n++;
    ov_strlcpy(path + n, "/ovengine.ll", sizeof(path) - (unsigned)n);
    int h = ov_lib_open(path);
    if (h < 0) return -1;
    g_oe.lib = h;
    OV_BIND(h, g_oe.version, "oe_version");
    OV_BIND(h, g_oe.create, "oe_create");
    OV_BIND(h, g_oe.destroy, "oe_destroy");
    OV_BIND(h, g_oe.threads, "oe_threads");
    OV_BIND(h, g_oe.set_target, "oe_set_target");
    OV_BIND(h, g_oe.texture_load, "oe_texture_load");
    OV_BIND(h, g_oe.texture_add, "oe_texture_add");
    OV_BIND(h, g_oe.texture_color, "oe_texture_color");
    OV_BIND(h, g_oe.begin, "oe_begin");
    OV_BIND(h, g_oe.draw, "oe_draw");
    OV_BIND(h, g_oe.draw_ex, "oe_draw_ex");
    OV_BIND(h, g_oe.texture_pixels, "oe_texture_pixels");
    OV_BIND(h, g_oe.depth, "oe_depth");
    OV_BIND(h, g_oe.memory, "oe_memory");
    OV_BIND(h, g_oe.end, "oe_end");
    OV_BIND(h, g_oe.stats, "oe_stats");
    OV_BIND(h, g_oe.parallel, "oe_parallel");
    OV_BIND(h, g_oe.error_text, "oe_error_text");
    const void* const* p = (const void* const*)&g_oe.version;
    for (unsigned i = 0; i < (sizeof(g_oe) - sizeof(void*)) / sizeof(void*); i++) if (!p[i]) return -2;
    if (g_oe.version() != OE_VERSION) return -3;
    return 0;
}

namespace ov {

// Обёртка над oe_*: создаётся после oe_bind, путь к текстурам — от папки игры.
//     r.begin(cam); for (...) r.draw(mesh, model); r.end();
class Renderer {
public:
    // max_threads: сколько рабочих ядер взять (-1 — все свободные, 0 — без них)
    explicit Renderer(int max_threads = -1) : r_(g_oe.create ? g_oe.create(max_threads) : nullptr) {}
    ~Renderer() { if (r_) g_oe.destroy(r_); }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool ok() const { return r_ != nullptr; }
    int threads() const { return g_oe.threads(r_); }
    // percent < 100 — мир в меньшем разрешении (интерфейс поверх рисуется в полном)
    bool setTarget(uint32_t* fb, int w, int h, int percent = 100) { return g_oe.set_target(r_, fb, w, h, percent) != 0; }

    // Номер текстуры или OE_ERR_* / PNG_ERR_* (текст — errorText)
    int loadTexture(const char* rel_path) {
        char path[160];
        ov_strlcpy(path, g_oe_dir, sizeof(path));
        int n = 0;
        while (path[n]) n++;
        if (n < (int)sizeof(path) - 1) path[n++] = '/';
        ov_strlcpy(path + n, rel_path, sizeof(path) - (unsigned)n);
        return g_oe.texture_load(r_, path);
    }
    int addTexture(int w, int h, const uint32_t* px) { return g_oe.texture_add(r_, w, h, px); }
    uint32_t textureColor(int tex) const { return g_oe.texture_color(r_, tex); }

    void begin(const Camera& c) { g_oe.begin(r_, &c); }
    void draw(const Mesh& m, const Mat4& model) { g_oe.draw(r_, &m, &model); }
    // Поверх мира (рука, предмет в руке): model — от глаза, проекция — Camera::overlay_proj
    void drawOverlay(const Mesh& m, const Mat4& model) { g_oe.draw_ex(r_, &m, &model, OE_DRAW_OVERLAY); }
    // Пиксели текстуры 0xAARRGGBB (значки блоков); nullptr — нет такой
    const uint32_t* texturePixels(int tex, int* w, int* h) const { return g_oe.texture_pixels(r_, tex, w, h); }
    // z-буфер прошлого oe_end (1/w; 0 — небо), размер — внутренний, с учётом процента
    const float* depth(int* w, int* h) const { return g_oe.depth(r_, w, h); }
    // Куча движка: занято / всего / самый большой свободный кусок (F3)
    static void memory(uint64_t* used, uint64_t* total, uint64_t* largest) { g_oe.memory(used, total, largest); }
    void end() { g_oe.end(r_); }
    FrameStats stats() const { FrameStats s; g_oe.stats(r_, &s); return s; }

    // fn(ctx, 0..count-1, поток) на всех потоках движка; возвращается, когда всё готово
    void parallel(int count, JobFn fn, void* ctx) { g_oe.parallel(r_, count, fn, ctx); }

    static const char* errorText(int code) { return g_oe.error_text(code); }

private:
    oe_renderer* r_;
};

} // namespace ov

#endif
