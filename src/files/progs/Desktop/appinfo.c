#include "appinfo.h"
#include "ovos.h"
#include "prgres.h"

#define APPINFO_MAX 32

static appinfo_t s_info[APPINFO_MAX];
static int       s_count = 0, s_next = 0;
static appinfo_t s_none;

static int same(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const appinfo_t* appinfo(const char* path) {
    if (!path || !path[0]) return &s_none;
    for (int i = 0; i < s_count; i++) if (same(s_info[i].path, path)) return &s_info[i];

    // Новое -- вытесняем по кругу (программ в системе меньше, чем мест)
    appinfo_t* a = &s_info[s_next];
    s_next = (s_next + 1) % APPINFO_MAX;
    if (s_count < APPINFO_MAX) s_count++;
    ov_memset(a, 0, sizeof(*a));
    ov_strlcpy(a->path, path, sizeof(a->path));

    // Запасные имя и класс -- из имени файла: "/desprogs/ovfiles.prg" -> "ovfiles"
    const char* base = path;
    for (const char* p = path; *p; p++) if (*p == '/') base = p + 1;
    int len = 0;
    while (base[len]) len++;
    if (len > 4 && base[len - 4] == '.') len -= 4;
    int n = 0;
    for (; n < len && n < (int)sizeof(a->app) - 1; n++) {
        char c = base[n];
        a->app[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    a->app[n] = '\0';
    for (n = 0; n < len && n < (int)sizeof(a->name) - 1; n++) a->name[n] = base[n];
    a->name[n] = '\0';

    char buf[64];
    if (prg_resource(path, "NAME", buf, sizeof(buf)) > 1) ov_strlcpy(a->name, buf, sizeof(a->name));
    if (prg_resource(path, "APPI", buf, sizeof(buf)) > 1) ov_strlcpy(a->app, buf, sizeof(a->app));
    uint8_t probe[4];
    a->has_icon = prg_resource(path, "ICON", probe, sizeof(probe)) > 0;
    return a;
}
