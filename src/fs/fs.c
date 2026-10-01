#include "fs.h"

extern void sched_yield(void);

static volatile int g_fs_lock = 0;

void fs_lock(void) {
    while (__sync_lock_test_and_set(&g_fs_lock, 1)) sched_yield();
}

void fs_unlock(void) {
    __sync_lock_release(&g_fs_lock);
}

#if USE_FAT32
#include "fat32.h"
#else
#include "fat16.h"
#endif

int fs_mount(int root_mode) {
#if USE_FAT32
    return fat32_mount(root_mode);
#else
    return fat16_mount(root_mode);
#endif
}

int fs_touch(const char* name) {
#if USE_FAT32
    return fat32_touch(name);
#else
    return fat16_touch(name);
#endif
}

void fs_dir(void) {
#if USE_FAT32
    fat32_dir();
#else
    fat16_dir();
#endif
}

void fs_go_root(void) {
#if USE_FAT32
    fat32_go_root();
#else
    fat16_go_root();
#endif
}

int fs_change_dir(const char* name) {
#if USE_FAT32
    return fat32_change_dir(name);
#else
    return fat16_change_dir(name);
#endif
}

int fs_read_file(const char* name, void* out_buffer, uint32_t max_bytes) {
#if USE_FAT32
    return fat32_read_file(name, out_buffer, max_bytes);
#else
    return fat16_read_file(name, out_buffer, max_bytes);
#endif
}

int fs_write_file(const char* name, const void* in_buffer, uint32_t bytes_to_write) {
#if USE_FAT32
    return fat32_write_file(name, in_buffer, bytes_to_write);
#else
    return fat16_write_file(name, in_buffer, bytes_to_write);
#endif
}

int fs_remove_file(const char* name) {
#if USE_FAT32
    return fat32_remove_file(name);
#else
    return fat16_remove_file(name);
#endif
}

int fs_make_folder(const char* name) {
#if USE_FAT32
    return fat32_make_folder(name);
#else
    return fat16_make_folder(name);
#endif
}

int fs_file_exists(const char* name) {
#if USE_FAT32
    return fat32_file_exists(name);
#else
    return fat16_file_exists(name);
#endif
}

int fs_is_mounted(void) {
#if USE_FAT32
    return fat32_is_mounted();
#else
    return fat16_is_mounted();
#endif
}

int fs_get_dir_files(fs_file_info_t* out_list, int max_files) {
#if USE_FAT32
    return fat32_get_dir_files(out_list, max_files);
#else
    return fat16_get_dir_files((fat16_file_info_t*)out_list, max_files);
#endif
}

const char* fs_get_last_dir_name(void) {
#if USE_FAT32
    return fat32_get_last_dir_name();
#else
    return "";
#endif
}