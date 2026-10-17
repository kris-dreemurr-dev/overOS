#include "fs.h"
#include "fat32.h"
#include "mount.h"

extern void sched_yield(void);

static volatile int g_fs_lock = 0;

void fs_lock(void) {
    while (__sync_lock_test_and_set(&g_fs_lock, 1)) sched_yield();
}

void fs_unlock(void) {
    __sync_lock_release(&g_fs_lock);
}


// ----------------------------------------------------------------------------
// Текущий каталог — у каждого свой
//
// Раньше позиция FAT-драйвера была состоянием на всю систему: оболочка TTY
// двигала её командой cd, а при Alt+F1..F6 ядро заново проходило путь нового
// TTY. Программа в фоновом TTY, работавшая с относительным именем, попадала не
// в свой каталог. Теперь позиция драйвера — деталь ОДНОЙ операции: перед каждой
// мы переходим в каталог вызывающего (оболочка своего TTY или процесс), а
// g_fat_dir помнит, где драйвер уже стоит, чтобы не ходить по каталогам зря.
// ----------------------------------------------------------------------------
extern const char* fs_caller_cwd(void);          // ядро: tty_cwd() или task_t.cwd процесса
extern void        fs_caller_set_cwd(const char* abs_path);

// Где сейчас стоит драйвер на каждом томе ("" — неизвестно)
static char g_fat_dirs[FAT32_MAX_VOLS][128];
#define g_fat_dir (g_fat_dirs[0])

static int fs_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Проход по абсолютному пути напрямую драйвером (без синхронизации — её делаем мы)
static void fat_goto(const char* path) {
    fat32_go_root();
    if (!path) return;
    char comp[64];
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        int n = 0;
        while (*p && *p != '/') {
            if (n < (int)sizeof(comp) - 1) comp[n++] = *p;
            p++;
        }
        comp[n] = '\0';
        if (n > 0) fat32_change_dir(comp);
    }
}

static void fs_sync_cwd(void) {
    fat32_select(0);
    const char* want = fs_caller_cwd();
    if (!want || !want[0]) want = "/";
    if (g_fat_dir[0] && fs_streq(g_fat_dir, want)) return;
    fat_goto(want);
    int i = 0;
    while (want[i] && i < (int)sizeof(g_fat_dir) - 1) { g_fat_dir[i] = want[i]; i++; }
    g_fat_dir[i] = '\0';
}

// Драйвер сдвинули вручную (cd, go_root, монтирование) — кэш больше не верен
static void fs_dir_cache_drop(void) { g_fat_dir[0] = '\0'; }

// Запоминаем, где стоит драйвер
static void fs_dir_cache_set(const char* path) {
    int i = 0;
    while (path[i] && i < (int)sizeof(g_fat_dir) - 1) { g_fat_dir[i] = path[i]; i++; }
    g_fat_dir[i] = '\0';
}

// Монтирование теперь дело fs/mount.c: корень ищется среди всех дисков
int fs_mount(int root_mode) {
    (void)root_mode;
    return mount_root();
}

// Том перемонтирован или снят: позиция драйвера на нём больше не верна
void fs_volume_changed(int vol) {
    if (vol >= 0 && vol < FAT32_MAX_VOLS) g_fat_dirs[vol][0] = '\0';
}

// ----------------------------------------------------------------------------
// Точки монтирования
//
// Путь внутри /media/<имя> ведёт на другой том. Для таких путей fs.c сам
// собирает абсолютный путь (каталог вызывающего + имя, с "." и ".."),
// выбирает том и отдаёт драйверу путь уже ВНУТРИ тома. Всё остальное идёт на
// корень ровно так же, как раньше, -- со всеми привычками старого кода
// (относительные имена от каталога вызывающего), чтобы ничего не сломать.
// ----------------------------------------------------------------------------
static int fs_starts(const char* s, const char* p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

// Внутри ли путь точки монтирования (или это сам /media)
static int in_media(const char* abs) {
    return fs_starts(abs, MOUNT_MEDIA) && (abs[6] == '\0' || abs[6] == '/');
}

// Абсолютный нормализованный путь: base + name, без "." и "..", без двойных '/'
static void fs_abs(const char* name, char* out, int max) {
    char tmp[256];
    int n = 0;
    if (name[0] != '/') {
        const char* base = fs_caller_cwd();
        if (!base || !base[0]) base = "/";
        while (base[n] && n < (int)sizeof(tmp) - 2) { tmp[n] = base[n]; n++; }
        tmp[n++] = '/';
    }
    for (int i = 0; name[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = name[i];
    tmp[n] = '\0';

    int o = 0;
    out[o++] = '/';
    const char* p = tmp;
    while (*p) {
        while (*p == '/') p++;
        const char* seg = p;
        int len = 0;
        while (p[len] && p[len] != '/') len++;
        p += len;
        if (!len || (len == 1 && seg[0] == '.')) continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;
            if (o > 1) o--;
            continue;
        }
        if (o > 1 && o < max - 1) out[o++] = '/';
        for (int k = 0; k < len && o < max - 1; k++) out[o++] = seg[k];
    }
    out[o] = '\0';
}

// Куда идёт операция с именем name. 0 -- по-старому, на корень (драйвер уже
// стоит в каталоге вызывающего, имя отдаётся как есть). Иначе -- номер тома,
// а в *path -- путь внутри него. Каталог вызывающего внутри /media, а имя
// уводит наружу ("../../etc") -- тоже том 0, но с готовым абсолютным путём.
static char s_route_buf[256];
static int fs_route(const char* name, const char** path) {
    if (!name) { *path = ""; return 0; }
    const char* cwd = fs_caller_cwd();
    int cwd_media = cwd && in_media(cwd);
    if (!cwd_media && !(name[0] == '/' && in_media(name))) { *path = name; return 0; }
    fs_abs(name, s_route_buf, sizeof(s_route_buf));
    const char* rest;
    int vol = mount_route(s_route_buf, &rest);
    *path = vol ? rest : s_route_buf;
    return vol ? vol : -1;                     // -1: корень, но путь уже абсолютный
}

// Выбрать том перед вызовом драйвера. Для тома 0 по-старому -- ещё и
// перейти в каталог вызывающего.
static void fs_enter(int vol) {
    if (vol <= 0) {
        fat32_select(0);
        if (vol == 0) fs_sync_cwd();
        return;
    }
    fat32_select(vol);
}

// Разбить абсолютный путь на каталог и последний элемент ("/a/b/c" -> "/a/b", "c")
static const char* split_leaf(const char* p, char* dir, int max) {
    const char* last = 0;
    for (const char* c = p; *c; c++) if (*c == '/') last = c;
    int n = 0;
    if (last) for (const char* c = p; c < last && n < max - 1; c++) dir[n++] = *c;
    if (n == 0) dir[n++] = '/';
    dir[n] = '\0';
    return last ? last + 1 : p;
}

// Создание файла/каталога по пути: сами переходим в каталог-родитель, потому что
// fat32_make_folder умеет только имя внутри текущего каталога (раньше из-за этого
// "mkdir a/b" молча создавал мусор, а ядро обходило путь руками в user_mode.c).
static int fs_create_entry(const char* path, int is_dir) {
    if (!path || !path[0]) return 0;

    const char* rp;
    int vol = fs_route(path, &rp);
    if (vol != 0) {                    // на съёмный том или абсолютным путём из /media
        char dir[256];
        const char* leaf = split_leaf(rp, dir, sizeof(dir));
        if (!leaf[0] || (vol < 0 && in_media(rp))) return 0;   // в самом /media создавать нечего
        fs_enter(vol);
        fat_goto(dir);
        int r = is_dir ? fat32_make_folder(leaf) : fat32_touch(leaf);
        fs_volume_changed(vol < 0 ? 0 : vol);
        return r;
    }

    const char* last_slash = 0;
    for (const char* c = path; *c; c++) if (*c == '/') last_slash = c;

    if (!last_slash) {                 // просто имя — в каталоге вызывающего
        fs_sync_cwd();
        return is_dir ? fat32_make_folder(path) : fat32_touch(path);
    }

    char dir[128];
    int n = 0;
    if (path[0] != '/') {              // относительный: дописываем к каталогу вызывающего
        const char* base = fs_caller_cwd();
        if (!base || !base[0]) base = "/";
        while (base[n] && n < (int)sizeof(dir) - 2) { dir[n] = base[n]; n++; }
        if (n == 0 || dir[n - 1] != '/') dir[n++] = '/';
    }
    for (const char* c = path; c < last_slash && n < (int)sizeof(dir) - 1; c++) dir[n++] = *c;
    dir[n] = '\0';
    if (n == 0) { dir[0] = '/'; dir[1] = '\0'; }

    fat32_select(0);
    fat_goto(dir);
    int r = is_dir ? fat32_make_folder(last_slash + 1) : fat32_touch(last_slash + 1);
    fs_dir_cache_drop();
    return r;
}

int fs_touch(const char* name) {
    return fs_create_entry(name, 0);
}

// Листинг текущего каталога в консоли ядра
void fs_dir(void) {
    const char* cwd = fs_caller_cwd();
    if (cwd && in_media(cwd)) {
        const char* rest;
        int vol = mount_route(cwd, &rest);
        fat32_select(vol);
        fat_goto(vol ? rest : cwd);
        fat32_dir();
        fs_volume_changed(vol);
        return;
    }
    fs_sync_cwd();
    fat32_dir();
}

void fs_go_root(void) {
    fat32_select(0);
    fat32_go_root();
    fs_caller_set_cwd("/");
    fs_dir_cache_set("/");
}

// cd — смена каталога ВЫЗЫВАЮЩЕГО, а не только позиции драйвера: иначе код,
// который «заходит в папку и читает файл по имени» (модули .sys, оболочка),
// на следующем же вызове возвращался бы обратно.
int fs_change_dir(const char* name) {
    const char* rp;
    int vol = fs_route(name, &rp);
    if (vol != 0) {
        // Путь уже собран целиком (s_route_buf): проверяем, что каталог есть
        char target[128];
        int n = 0;
        while (s_route_buf[n] && n < (int)sizeof(target) - 1) { target[n] = s_route_buf[n]; n++; }
        target[n] = '\0';
        int ok;
        if (vol > 0) { fat32_select(vol); ok = (rp[0] == '/' && !rp[1]) || fat32_dir_exists(rp); }
        else if (in_media(target) && !target[6]) ok = 1;        // сам /media есть всегда
        else { fat32_select(0); ok = fat32_dir_exists(target); }
        if (!ok) return 0;
        fs_caller_set_cwd(target);
        fs_dir_cache_drop();
        return 1;
    }

    fs_sync_cwd();
    if (!fat32_change_dir(name)) return 0;

    const char* base = fs_caller_cwd();
    if (!base || !base[0]) base = "/";

    char next[128];
    int n = 0;
    while (base[n] && n < (int)sizeof(next) - 1) { next[n] = base[n]; n++; }
    next[n] = '\0';

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {        // на уровень вверх
        while (n > 1 && next[n - 1] != '/') next[--n] = '\0';
        if (n > 1 && next[n - 1] == '/') next[--n] = '\0';
    } else if (!(name[0] == '.' && name[1] == '\0')) {               // вход в подкаталог
        const char* real = fat32_get_last_dir_name();
        const char* leaf = (real && real[0]) ? real : name;
        if (n == 0 || next[n - 1] != '/') next[n++] = '/';
        for (int i = 0; leaf[i] && n < (int)sizeof(next) - 1; i++) next[n++] = leaf[i];
        next[n] = '\0';
    }
    if (n == 0) { next[0] = '/'; next[1] = '\0'; }

    fs_caller_set_cwd(next);
    fs_dir_cache_set(next);
    return 1;
}

int fs_read_file(const char* name, void* out_buffer, uint32_t max_bytes) {
    const char* p; int vol = fs_route(name, &p);
    fs_enter(vol);
    return fat32_read_file(p, out_buffer, max_bytes);
}

int fs_write_file(const char* name, const void* in_buffer, uint32_t bytes_to_write) {
    const char* p; int vol = fs_route(name, &p);
    if (vol < 0 && in_media(p)) return 0;
    fs_enter(vol);
    return fat32_write_file(p, in_buffer, bytes_to_write);
}

int fs_remove_file(const char* name) {
    const char* p; int vol = fs_route(name, &p);
    if (vol < 0 && in_media(p)) return 0;
    if (vol > 0 && p[0] == '/' && !p[1]) return 0;               // корень тома не удаляется
    fs_enter(vol);
    return fat32_remove_file(p);
}

int fs_make_folder(const char* name) {
    return fs_create_entry(name, 1);
}

// Есть ли такой каталог (путь считается от каталога вызывающего, если он относительный)
int fs_dir_exists(const char* path) {
    const char* p; int vol = fs_route(path, &p);
    if (vol < 0 && in_media(p) && !p[6]) return 1;               // /media
    if (vol > 0 && p[0] == '/' && !p[1]) return 1;               // корень тома
    fs_enter(vol);
    return fat32_dir_exists(p);
}

int fs_file_exists(const char* name) {
    const char* p; int vol = fs_route(name, &p);
    fs_enter(vol);
    return fat32_file_exists(p);
}

int fs_is_mounted(void) {
    return fat32_is_mounted();
}

int fs_get_dir_files(const char* path, fs_file_info_t* out_list, int max_files) {
    const char* q = (path && path[0]) ? path : ".";
    const char* p;
    int vol = fs_route(q, &p);
    if (vol == 0) {
        fs_sync_cwd();
        return fat32_get_dir_files(path, out_list, max_files);
    }
    fs_enter(vol);
    if (vol < 0 && in_media(p) && !p[6]) {
        // /media: что лежит на диске (обычно пусто) плюс подключённые тома
        int n = fat32_get_dir_files(p, out_list, max_files);
        if (n < 0) n = 0;
        return n + mount_media_entries(out_list + n, max_files - n);
    }
    return fat32_get_dir_files(p, out_list, max_files);
}

const char* fs_get_last_dir_name(void) {
    return fat32_get_last_dir_name();
}

void fs_get_stats(uint32_t* out_used_mb, uint32_t* out_total_mb) {
    fat32_select(0);
    fat32_get_stats(out_used_mb, out_total_mb);
}
// Переход драйвера в каталог по абсолютному пути (fs_lock уже взят).
// Нужен, чтобы вернуть FAT в текущий каталог оболочки после операций, начинавшихся с корня.
void fs_goto_abs(const char* path) {
    fat32_select(0);
    fat_goto(path);
    fs_dir_cache_drop();
}

int fs_read_at(const char* name, uint32_t offset, void* out_buffer, uint32_t max_bytes) {
    const char* p; int vol = fs_route(name, &p);
    fs_enter(vol);
    return fat32_read_at(p, offset, out_buffer, max_bytes);
}

uint32_t fs_file_size(const char* name) {
    const char* p; int vol = fs_route(name, &p);
    fs_enter(vol);
    return fat32_file_size(p);
}
