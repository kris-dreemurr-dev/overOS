#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>

typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;
    uint16_t total_sectors_short;
    uint8_t  media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_long;
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat16_bpb_t;

typedef struct {
    char     filename[8];
    char     ext[3];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  creation_ms;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t cluster_high;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat_dir_entry_t;

typedef struct {
    char name83[11];
    uint8_t attr;
    uint32_t size;
    uint16_t cluster;
    char clean_name[16];
} __attribute__((packed)) fat16_file_info_t;

int      fat16_mount(int root_mode);
int      fat16_touch(const char* name83);
uint32_t fat16_get_partition_lba(void);
int      fat16_is_mounted(void);
void     fat16_dir(void);
void fat16_go_root(void);
int  fat16_change_dir(const char* name83);
int fat16_write_file(const char* name83, const void* in_buffer, uint32_t bytes_to_write);
int fat16_read_file(const char* name83, void* out_buffer, uint32_t max_bytes);
int fat16_remove_file(const char* name83);
int fat16_make_folder(const char* name83);
int fat16_get_dir_files(fat16_file_info_t* out_list, int max_files);
int fat16_file_exists(const char* name83);


#endif