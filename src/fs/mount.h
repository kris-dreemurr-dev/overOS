#ifndef MOUNT_H
#define MOUNT_H
// mount.h — тома и точки монтирования (см. mount.c).
//
// Том 0 -- корень (флешка, с которой загрузилась система). Остальные --
// съёмные накопители, подключённые в /media/<метка>, как это делает udisks в
// Linux. Пути внутрь /media/<метка>/... fs.c отправляет на соответствующий том.

#include <stdint.h>
#include "fs.h"

#define MOUNT_MEDIA "/media"

// Состояние корня (sys_storage_state, syscall 132)
#define MOUNT_ROOT_OK       0
#define MOUNT_ROOT_LOST     1
#define MOUNT_ROOT_FOREIGN  2      // больше не возникает: чужая флешка просто монтируется в /media

// Загрузка: найти среди дисков корень (FAT32 с /sys/KERNEL.BIN) и смонтировать
// его томом 0. 1 -- нашёлся.
int  mount_root(void);
int  mount_root_state(void);
int  mount_root_disk(void);

// Поток storaged: опрос USB, подключение и снятие томов, сторож корня
void mount_start(void);

// Путь (абсолютный, нормализованный) -> том и путь внутри тома. Для всего,
// что не внутри /media/<имя>, -- том 0 и тот же путь.
int  mount_route(const char* abs, const char** rest);
// Сколько подключённых томов и их имена -- для списка каталога /media
int  mount_media_entries(fs_file_info_t* out, int max);

// Описание тома для программ (sys_mounts, syscall 134). Совпадает с ov_mount_t.
typedef struct {
    char     name[24];      // имя в /media
    char     label[24];     // метка тома ("" -- нет)
    char     dev[40];       // "ADATA USB Flash Drive"
    char     path[40];      // "/media/NAME" или "/" у корня
    uint32_t size_mb;
    uint32_t flags;         // MOUNT_F_*
} mount_info_t;
#define MOUNT_F_ROOT      1
#define MOUNT_F_REMOVABLE 2

int  mount_list(mount_info_t* out, int max);
// Безопасно извлечь: снять все тома этого накопителя (sys_eject, syscall 135).
// 0 -- готово, -1 -- нет такого, -2 -- это корень.
int  mount_eject(const char* name);

#endif
