#ifndef PRGRES_H
#define PRGRES_H
// prgres.h — ресурсы исполняемого файла .prg: значок (ICON, SVG), имя (NAME)
// и класс программы (APPI). Их вшивает tools/elf2prg.py (--icon/--name/--app),
// смещение каталога лежит в последнем поле заголовка DPRG.
//
// Так значок принадлежит программе, а не лежит отдельным файлом в общей папке:
// переложили .prg в другое место -- значок поехал вместе с ним, как у .exe в
// Windows или у пакета приложения в macOS.

#include "ovos.h"

// Прочитать ресурс type ("ICON", "NAME", "APPI") в out. Возвращает размер или
// -1. Текст (NAME/APPI) всегда завершается нулём, если влезает.
static inline int prg_resource(const char* path, const char* type, void* out, int max) {
    int fd = ov_open(path, 0);
    if (fd < 0) return -1;
    uint8_t hdr[64];
    int ret = -1;
    if (ov_read(fd, hdr, 64) == 64 && hdr[0] == 'D' && hdr[1] == 'P' && hdr[2] == 'R' && hdr[3] == 'G') {
        uint64_t off = 0;
        for (int i = 0; i < 8; i++) off |= (uint64_t)hdr[56 + i] << (8 * i);
        uint8_t dir[8 + 12 * 8];
        if (off && ov_lseek(fd, (int64_t)off, OV_SEEK_SET) == (int64_t)off &&
            ov_read(fd, dir, 8) == 8 && dir[0] == 'D' && dir[1] == 'R' && dir[2] == 'E' && dir[3] == 'S') {
            uint32_t n = dir[4] | (dir[5] << 8) | (dir[6] << 16) | ((uint32_t)dir[7] << 24);
            if (n > 8) n = 8;
            if (ov_read(fd, dir + 8, 12 * n) == (int64_t)(12 * n)) {
                for (uint32_t k = 0; k < n; k++) {
                    const uint8_t* e = dir + 8 + 12 * k;
                    if (e[0] != (uint8_t)type[0] || e[1] != (uint8_t)type[1] ||
                        e[2] != (uint8_t)type[2] || e[3] != (uint8_t)type[3]) continue;
                    uint32_t at  = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
                    uint32_t len = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
                    if ((int)len > max) len = (uint32_t)max;
                    if (ov_lseek(fd, at, OV_SEEK_SET) == (int64_t)at && ov_read(fd, out, len) == (int64_t)len) {
                        ret = (int)len;
                        if (type[0] != 'I' && max > 0) ((char*)out)[len < (uint32_t)max ? len : (uint32_t)max - 1] = '\0';
                    }
                    break;
                }
            }
        }
    }
    ov_close(fd);
    return ret;
}

#endif
