#ifndef FS_H
#define FS_H

#include <stdint.h>

// ==================== [ FS SWITCH ] ====================
// 0 - использовать проверенный драйвер FAT16
// 1 - использовать новый драйвер FAT32
#define USE_FAT32 1
// =======================================================

typedef struct {
    char name[32];      // Поддержка длинных имен (LFN)
    uint8_t attr;
    uint32_t size;
    uint32_t cluster;
    char clean_name[64];
} __attribute__((packed)) fs_file_info_t;

// Универсальный API файловой системы
int      fs_mount(int root_mode);
int      fs_touch(const char* name);
void     fs_dir(void);
void     fs_go_root(void);
int      fs_change_dir(const char* name);
int      fs_read_file(const char* name, void* out_buffer, uint32_t max_bytes);
int      fs_write_file(const char* name, const void* in_buffer, uint32_t bytes_to_write);
int      fs_remove_file(const char* name);
int      fs_make_folder(const char* name);
int      fs_file_exists(const char* name);
int      fs_is_mounted(void);
int      fs_get_dir_files(fs_file_info_t* out_list, int max_files);
const char* fs_get_last_dir_name(void);

#endif