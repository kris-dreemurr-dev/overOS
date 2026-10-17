#ifndef OVCRAFT_FILES_H
#define OVCRAFT_FILES_H
// =============================================================================
// files.h — чтение файлов из папки игры (resources/...) целиком.
// Путь — относительно папки, где лежит ovcraft.prg (её знает ovengine_ll.h).
// =============================================================================

#include "ovos.h"
#include "engine/ovengine_ll.h"

// Прочитать файл в buf (до cap-1 байт, в конце ноль). Число байт или -1
inline int res_read(const char* rel, char* buf, int cap) {
    char path[160];
    ov_strlcpy(path, g_oe_dir, sizeof(path));
    int n = 0;
    while (path[n]) n++;
    if (n < (int)sizeof(path) - 1) path[n++] = '/';
    ov_strlcpy(path + n, rel, sizeof(path) - (unsigned)n);
    int fd = ov_open(path, 0);
    if (fd < 0) return -1;
    int got = 0;
    while (got < cap - 1) {
        int64_t k = ov_read(fd, buf + got, (uint64_t)(cap - 1 - got));
        if (k <= 0) break;
        got += (int)k;
    }
    ov_close(fd);
    buf[got] = '\0';
    return got;
}

#endif
