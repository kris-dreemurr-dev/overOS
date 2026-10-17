#ifndef FS_H
#define FS_H

#include <stdint.h>

typedef struct {
    char name[32];      // Поддержка длинных имен (LFN)
    uint8_t attr;
    uint32_t size;
    uint32_t cluster;
    char clean_name[64];
} __attribute__((packed)) fs_file_info_t;

// Универсальный API файловой системы
int      fs_mount(int root_mode);
// Том перемонтирован или снят (fs/mount.c): забыть, где на нём стоял драйвер
void     fs_volume_changed(int vol);
int      fs_touch(const char* name);
void     fs_dir(void);
void     fs_go_root(void);
int      fs_change_dir(const char* name);
int      fs_read_file(const char* name, void* out_buffer, uint32_t max_bytes);
int      fs_write_file(const char* name, const void* in_buffer, uint32_t bytes_to_write);
int      fs_remove_file(const char* name);
int      fs_make_folder(const char* name);
int      fs_file_exists(const char* name);
int      fs_dir_exists(const char* path);
int      fs_is_mounted(void);
int      fs_get_dir_files(const char* path, fs_file_info_t* out_list, int max_files);
const char* fs_get_last_dir_name(void);
void fs_get_stats(uint32_t* out_used_mb, uint32_t* out_total_mb);
void fs_goto_abs(const char* path);
int      fs_read_at(const char* name, uint32_t offset, void* out_buffer, uint32_t max_bytes);
uint32_t fs_file_size(const char* name);

// Все fs_*/fat32_* вызовы делят одно состояние драйвера (позиция в каталоге,
// буфер сектора): конкурентный доступ из разных задач/прерываний его портит.
// Берётся на время одной fs_* операции или короткой последовательности
// (например fs_go_root + цепочка fs_change_dir при переключении TTY).
void fs_lock(void);
void fs_unlock(void);

#endif