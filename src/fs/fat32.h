#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "fs.h"

// Структура загрузочного сектора (BPB) для FAT32
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;        // В FAT32 всегда 0
    uint16_t total_sectors_short; // В FAT32 всегда 0
    uint8_t  media_type;
    uint16_t sectors_per_fat_16;  // В FAT32 всегда 0
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_long;  // Реальное число секторов тома
    // Специфичные поля FAT32:
    uint32_t sectors_per_fat_32;  // Размер одной FAT-таблицы в секторах
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;        // Стартовый кластер корневого каталога
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

// API драйвера FAT32
int      fat32_mount(int root_mode);
int      fat32_touch(const char* name);
void     fat32_dir(void);
void     fat32_go_root(void);
int      fat32_change_dir(const char* name);
int      fat32_read_file(const char* name, void* out_buffer, uint32_t max_bytes);
int      fat32_write_file(const char* name, const void* in_buffer, uint32_t bytes_to_write);
int      fat32_remove_file(const char* name);
int      fat32_make_folder(const char* name);
int      fat32_file_exists(const char* name);
int      fat32_is_mounted(void);
int      fat32_get_dir_files(fs_file_info_t* out_list, int max_files);
const char* fat32_get_last_dir_name(void);
static int fat32_entry_exists(const char* name);

#endif