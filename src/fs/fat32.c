#include "fat32.h"
#include "fat16.h"
#include "../drivers/usb/ehci-msc.h"

extern void kputs(const char* str, uint32_t color);
extern void kdebug(const char* str, uint32_t color);
extern void itoa(int n, char* str);
extern int strcmp(const char* s1, const char* s2);
extern void flush_buffer(void);

static fat32_bpb_t g_bpb32;
static uint32_t g_partition_lba = 2048;
static uint32_t g_fat_lba = 0;
static uint32_t g_data_lba = 0;
static int      g_mounted32 = 0;
static uint32_t g_current_cluster32 = 2; 

static uint8_t  sector_buf32[512] __attribute__((aligned(64)));

// Кэш для ускорения чтения FAT-таблицы
static uint32_t g_cached_fat_sec32 = 0xFFFFFFFF;
static uint8_t  g_cached_fat_buf32[512] __attribute__((aligned(64)));

// Буфер сборщика длинных имен (LFN)
static char global_lfn_buf[260];

int fat32_is_mounted(void) { return g_mounted32; }

void fat32_go_root(void) {
    g_current_cluster32 = g_bpb32.root_cluster;
}

int fat32_mount(int root_mode) {
    g_mounted32 = 0;
    g_cached_fat_sec32 = 0xFFFFFFFF;

    if (root_mode) {
        g_partition_lba = 0;
    } else {
        if (!ehci_msc_read_sectors(0, 1, sector_buf32)) return 0;
        uint16_t sig = sector_buf32[510] | (sector_buf32[511] << 8);
        if (sig != 0xAA55) return 0;

        uint32_t fat_lba = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t type = sector_buf32[446 + (i * 16) + 4];
            if (type == 0x0B || type == 0x0C) {
                fat_lba = *(uint32_t*)&sector_buf32[446 + (i * 16) + 8];
                break;
            }
        }
        g_partition_lba = (fat_lba != 0) ? fat_lba : 2048;
    }

    if (!ehci_msc_read_sectors(g_partition_lba, 1, sector_buf32)) return 0;
    for (int i = 0; i < (int)sizeof(fat32_bpb_t); i++) {
        ((uint8_t*)&g_bpb32)[i] = sector_buf32[i];
    }

    if (g_bpb32.bytes_per_sector != 512) return 0;

    g_fat_lba = g_partition_lba + g_bpb32.reserved_sectors;
    uint32_t fat_size = g_bpb32.sectors_per_fat_32;
    g_data_lba = g_fat_lba + (g_bpb32.fat_count * fat_size);
    g_current_cluster32 = g_bpb32.root_cluster;

    g_mounted32 = 1;
    return 1;
}

static uint32_t fat32_get_next_cluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_fat_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    if (fat_sector != g_cached_fat_sec32) {
        if (!ehci_msc_read_sectors(fat_sector, 1, g_cached_fat_buf32)) return 0x0FFFFFFF;
        g_cached_fat_sec32 = fat_sector;
    }
    uint32_t next = *(uint32_t*)&g_cached_fat_buf32[entry_offset];
    return next & 0x0FFFFFFF;
}

static void fat32_set_cluster(uint32_t cluster, uint32_t value) {
    g_cached_fat_sec32 = 0xFFFFFFFF; // Сброс кэша чтения
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_fat_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    for (int f = 0; f < g_bpb32.fat_count; f++) {
        uint32_t target_sec = fat_sector + (f * g_bpb32.sectors_per_fat_32);
        if (!ehci_msc_read_sectors(target_sec, 1, sector_buf32)) return;
        uint32_t old = *(uint32_t*)&sector_buf32[entry_offset];
        *(uint32_t*)&sector_buf32[entry_offset] = (old & 0xF0000000) | (value & 0x0FFFFFFF);
        ehci_msc_write_sectors(target_sec, 1, sector_buf32);
    }
}

static uint32_t fat32_find_free_cluster(void) {
    uint32_t total_fat_sectors = g_bpb32.sectors_per_fat_32;
    for (uint32_t s = 0; s < total_fat_sectors; s++) {
        if (!ehci_msc_read_sectors(g_fat_lba + s, 1, sector_buf32)) return 0;
        uint32_t* entries = (uint32_t*)sector_buf32;
        int start_idx = (s == 0) ? 2 : 0;
        for (int i = start_idx; i < 128; i++) {
            if ((entries[i] & 0x0FFFFFFF) == 0x00000000) {
                return (s * 128 + i);
            }
        }
    }
    return 0;
}

static void format_size(uint32_t size, char* out_buf) {
    char tmp[16];
    if (size < 1024) {
        itoa(size, tmp);
        int i = 0; while(tmp[i]) { out_buf[i] = tmp[i]; i++; }
        out_buf[i++] = ' '; out_buf[i++] = 'B'; out_buf[i] = '\0';
    } else if (size < 1024 * 1024) {
        itoa(size / 1024, tmp);
        int i = 0; while(tmp[i]) { out_buf[i] = tmp[i]; i++; }
        out_buf[i++] = ' '; out_buf[i++] = 'K'; out_buf[i] = '\0';
    } else {
        itoa(size / (1024 * 1024), tmp);
        int i = 0; while(tmp[i]) { out_buf[i] = tmp[i]; i++; }
        out_buf[i++] = ' '; out_buf[i++] = 'M'; out_buf[i] = '\0';
    }
}

typedef struct {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attribute;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t cluster_low;
    uint16_t name3[2];
} __attribute__((packed)) fat_lfn_entry_t;

static void parse_lfn_entry(const fat_lfn_entry_t* lfn) {
    int seq = lfn->order & 0x1F;
    if (seq < 1 || seq > 20) return;
    int idx = (seq - 1) * 13;

    uint16_t chars[13] = {
        lfn->name1[0], lfn->name1[1], lfn->name1[2], lfn->name1[3], lfn->name1[4],
        lfn->name2[0], lfn->name2[1], lfn->name2[2], lfn->name2[3], lfn->name2[4], lfn->name2[5],
        lfn->name3[0], lfn->name3[1]
    };

    for (int i = 0; i < 13; i++) {
        uint16_t c = chars[i];
        if (c == 0x0000 || c == 0xFFFF) {
            global_lfn_buf[idx + i] = '\0';
            return;
        }
        global_lfn_buf[idx + i] = (c < 128) ? (char)c : '_';
    }
}

static int is_valid_83(const char* name) {
    int name_len = 0, ext_len = 0, has_dot = 0;
    int has_lower_name = 0, has_upper_name = 0;
    int has_lower_ext = 0, has_upper_ext = 0;

    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') return 0;
        if (c == '.') {
            if (has_dot) return 0; // Вторая точка требует LFN
            has_dot = 1;
            continue;
        }
        if (!has_dot) {
            name_len++;
            if (c >= 'a' && c <= 'z') has_lower_name = 1;
            if (c >= 'A' && c <= 'Z') has_upper_name = 1;
        } else {
            ext_len++;
            if (c >= 'a' && c <= 'z') has_lower_ext = 1;
            if (c >= 'A' && c <= 'Z') has_upper_ext = 1;
        }
    }

    if (name_len == 0 || name_len > 8 || ext_len > 3) return 0;
    // Смешанный регистр внутри одной части (например, "CoDe") требует LFN
    if (has_lower_name && has_upper_name) return 0;
    if (has_lower_ext && has_upper_ext) return 0;

    return 1;
}

static int strcasecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        int c1 = *s1++;
        int c2 = *s2++;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
    }
    int end1 = *s1;
    int end2 = *s2;
    if (end1 >= 'A' && end1 <= 'Z') end1 += 32;
    if (end2 >= 'A' && end2 <= 'Z') end2 += 32;
    return end1 - end2;
}

// Преобразование строкового имени в короткий формат 8.3 для создания директорийной записи
static void to_short_83(const char* src, char* out83) {
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    int i = 0, d = 0;
    while (src[i] && src[i] != '.' && d < 8) {
        char c = src[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[d++] = c;
    }
    while (src[i] && src[i] != '.') i++;
    if (src[i] == '.') {
        i++;
        d = 8;
        while (src[i] && d < 11) {
            char c = src[i++];
            if (c >= 'a' && c <= 'z') c -= 32;
            out83[d++] = c;
        }
    }
}

static int compare_83(const fat_dir_entry_t* entry, const char* name83) {
    for (int i = 0; i < 8; i++) {
        if (entry->filename[i] != name83[i]) return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (entry->ext[i] != name83[8 + i]) return 0;
    }
    return 1;
}

static char g_last_dir_name[64] = {0};

const char* fat32_get_last_dir_name(void) {
    return g_last_dir_name;
}

void fat32_dir(void) {
    if (!g_mounted32) return;
    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';
    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;

    kputs("\nDirectory contents (FAT32):\n", 0x55FFFF);
    kputs("NAME                 SIZE\n", 0xAAAAAA);
    kputs("--------------------------------\n", 0xAAAAAA);

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) return;
                if ((uint8_t)entry->filename[0] == 0xE5) {
                    global_lfn_buf[0] = '\0';
                    continue;
                }

                if (entry->attributes == 0x0F) {
                    parse_lfn_entry((const fat_lfn_entry_t*)entry);
                    continue;
                }

                if (entry->attributes & 0x08) {
                    global_lfn_buf[0] = '\0';
                    continue;
                }

                char display_name[256];
                if (global_lfn_buf[0] != '\0') {
                    int p = 0;
                    while (global_lfn_buf[p] && p < 255) {
                        display_name[p] = global_lfn_buf[p];
                        p++;
                    }
                    display_name[p] = '\0';
                } else {
                    uint8_t ntres = entry->reserved;
                    int lower_name = (ntres & 0x08);
                    int lower_ext  = (ntres & 0x10);

                    int p = 0;
                    for (int c = 0; c < 8; c++) {
                        if (entry->filename[c] != ' ') {
                            char ch = entry->filename[c];
                            if (lower_name && ch >= 'A' && ch <= 'Z') ch += 32;
                            display_name[p++] = ch;
                        }
                    }
                    if (entry->ext[0] != ' ') {
                        display_name[p++] = '.';
                        for (int c = 0; c < 3; c++) {
                            if (entry->ext[c] != ' ') {
                                char ch = entry->ext[c];
                                if (lower_ext && ch >= 'A' && ch <= 'Z') ch += 32;
                                display_name[p++] = ch;
                            }
                        }
                    }
                    display_name[p] = '\0';
                }

                kputs(display_name, 0xFFFF55);
                int pad = 21;
                for (int l = 0; display_name[l]; l++) pad--;
                while (pad-- > 0) kputs(" ", 0);

                if (entry->attributes & 0x10) {
                    kputs("<DIR>\n", 0x55FF55);
                } else {
                    char sz_str[16];
                    format_size(entry->file_size, sz_str);
                    kputs(sz_str, 0x55FF55);
                    kputs("\n", 0);
                }

                global_lfn_buf[0] = '\0';
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    flush_buffer();
}

int fat32_change_dir(const char* name) {
    if (!name || name[0] == '\0') return 0;

    // 1. Переход в корень диска
    if (strcmp(name, "/") == 0) {
        g_current_cluster32 = g_bpb32.root_cluster;
        g_last_dir_name[0] = '\0';
        return 1;
    }

    // 2. Очистка входной строки от пробелов по краям
    char clean_name[64];
    int len = 0;
    while (name[len] && len < 63) { 
        clean_name[len] = name[len]; 
        len++; 
    }
    while (len > 0 && clean_name[len - 1] == ' ') len--;
    clean_name[len] = '\0';
    if (len == 0) return 0;

    // 3. Переход на уровень выше ".."
    if (strcmp(clean_name, "..") == 0) {
        if (g_current_cluster32 == g_bpb32.root_cluster) return 1;
        uint32_t cluster_lba = g_data_lba + (g_current_cluster32 - 2) * g_bpb32.sectors_per_cluster;
        if (ehci_msc_read_sectors(cluster_lba, 1, sector_buf32)) {
            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf32;
            uint32_t parent = ((uint32_t)entries[1].cluster_high << 16) | entries[1].cluster_low;
            if (parent == 0) parent = g_bpb32.root_cluster;
            g_current_cluster32 = parent;
            g_last_dir_name[0] = '\0';
            return 1;
        }
        return 0;
    }

    char name83[11];
    to_short_83(clean_name, name83);

    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';
    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;

    // 4. Поиск целевой директории
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) break;
                if ((uint8_t)entry->filename[0] == 0xE5) { 
                    global_lfn_buf[0] = '\0'; 
                    continue; 
                }
                if (entry->attributes == 0x0F) { 
                    parse_lfn_entry((const fat_lfn_entry_t*)entry); 
                    continue; 
                }

                // Извлекаем имя с сохранением оригинального регистра
                char filename[256];
                if (global_lfn_buf[0] != '\0') {
                    int p = 0; 
                    while (global_lfn_buf[p]) { filename[p] = global_lfn_buf[p]; p++; }
                    filename[p] = '\0';
                } else {
                    uint8_t ntres = entry->reserved;
                    int lower_name = (ntres & 0x08);
                    int lower_ext  = (ntres & 0x10);

                    int p = 0;
                    for (int c = 0; c < 8; c++) {
                        if (entry->filename[c] != ' ') {
                            char ch = entry->filename[c];
                            if (lower_name && ch >= 'A' && ch <= 'Z') ch += 32;
                            filename[p++] = ch;
                        }
                    }
                    if (entry->ext[0] != ' ') {
                        filename[p++] = '.';
                        for (int c = 0; c < 3; c++) {
                            if (entry->ext[c] != ' ') {
                                char ch = entry->ext[c];
                                if (lower_ext && ch >= 'A' && ch <= 'Z') ch += 32;
                                filename[p++] = ch;
                            }
                        }
                    }
                    filename[p] = '\0';
                }

                char short_name[13];
                int sp = 0;
                for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') short_name[sp++] = entry->filename[c];
                if (entry->ext[0] != ' ') {
                    short_name[sp++] = '.';
                    for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') short_name[sp++] = entry->ext[c];
                }
                short_name[sp] = '\0';

                // Сверяем имя без учёта регистра только для папок
                if ((strcasecmp(filename, clean_name) == 0 || strcasecmp(short_name, clean_name) == 0 || compare_83(entry, name83))
                    && (entry->attributes & 0x10)) {
                    
                    uint32_t target_cluster = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
                    if (target_cluster == 0) target_cluster = g_bpb32.root_cluster;
                    g_current_cluster32 = target_cluster;

                    // Запоминаем настоящее имя папки с диска
                    int p = 0;
                    while (filename[p] && p < 63) {
                        g_last_dir_name[p] = filename[p];
                        p++;
                    }
                    g_last_dir_name[p] = '\0';

                    return 1;
                }

                global_lfn_buf[0] = '\0';
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }

    return 0;
}

int fat32_read_file(const char* name, void* out_buffer, uint32_t max_bytes) {
    if (!g_mounted32) return 0;
    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';
    uint32_t cluster = 0;
    uint32_t file_size = 0;
    uint32_t dir_cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;

    char name83[11];
    to_short_83(name, name83);

    // 1. Поиск записи в каталоге
    while (dir_cluster >= 2 && dir_cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (dir_cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) break;
                if ((uint8_t)entry->filename[0] == 0xE5) { global_lfn_buf[0] = '\0'; continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }

                char filename[256];
                if (global_lfn_buf[0] != '\0') {
                    int p = 0; while (global_lfn_buf[p]) { filename[p] = global_lfn_buf[p]; p++; }
                    filename[p] = '\0';
                } else {
                    int p = 0;
                    for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') filename[p++] = entry->filename[c];
                    if (entry->ext[0] != ' ') {
                        filename[p++] = '.';
                        for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') filename[p++] = entry->ext[c];
                    }
                    filename[p] = '\0';
                }

                char short_name[13];
                int sp = 0;
                for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') short_name[sp++] = entry->filename[c];
                if (entry->ext[0] != ' ') {
                    short_name[sp++] = '.';
                    for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') short_name[sp++] = entry->ext[c];
                }
                short_name[sp] = '\0';

                if (strcasecmp(filename, name) == 0 || strcasecmp(short_name, name) == 0 || compare_83(entry, name83)) {
                    cluster = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
                    file_size = entry->file_size;
                    break;
                }
                global_lfn_buf[0] = '\0';
            }
            if (cluster) break;
        }
        if (cluster) break;
        dir_cluster = fat32_get_next_cluster(dir_cluster);
    }

    if (!cluster || file_size == 0) return 0;

    uint32_t total_to_read = (file_size < max_bytes) ? file_size : max_bytes;
    uint32_t bytes_read = 0;
    uint8_t* out_ptr = (uint8_t*)out_buffer;
    uint32_t bytes_per_cluster = spc * 512;

    // 2. Пакетное чтение с объединением непрерывных цепочек
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && bytes_read < total_to_read) {
        uint32_t run_start_cluster = cluster;
        uint32_t run_cluster_count = 1;

        // Смотрим вперёд по FAT: сколько кластеров лежат физически подряд
        while (bytes_read + (run_cluster_count * bytes_per_cluster) < total_to_read) {
            uint32_t next = fat32_get_next_cluster(run_start_cluster + run_cluster_count - 1);
            if (next == run_start_cluster + run_cluster_count) {
                run_cluster_count++;
            } else {
                break;
            }
        }

        // Вычисляем размер порции и число секторов
        uint32_t run_bytes = run_cluster_count * bytes_per_cluster;
        if (bytes_read + run_bytes > total_to_read) {
            run_bytes = total_to_read - bytes_read;
        }
        uint32_t sectors_to_read = (run_bytes + 511) / 512;

        uint32_t start_lba = g_data_lba + (run_start_cluster - 2) * spc;
        if (!ehci_msc_read_sectors(start_lba, sectors_to_read, out_ptr + bytes_read)) {
            break;
        }

        bytes_read += run_bytes;

        // Переходим к следующему кластеру после считанного блока
        cluster = fat32_get_next_cluster(run_start_cluster + run_cluster_count - 1);
    }

    return bytes_read;
}

int fat32_file_exists(const char* name) {
    if (!g_mounted32) return 0;
    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;
    char name83[11];
    to_short_83(name, name83);
    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) return 0;
                if ((uint8_t)entry->filename[0] == 0xE5 || (entry->attributes & 0x18)) { global_lfn_buf[0] = '\0'; continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }

                char filename[256];
                if (global_lfn_buf[0] != '\0') {
                    int p = 0; while (global_lfn_buf[p]) { filename[p] = global_lfn_buf[p]; p++; }
                    filename[p] = '\0';
                } else {
                    int p = 0;
                    for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') filename[p++] = entry->filename[c];
                    if (entry->ext[0] != ' ') {
                        filename[p++] = '.';
                        for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') filename[p++] = entry->ext[c];
                    }
                    filename[p] = '\0';
                }

                if (strcasecmp(filename, name) == 0 || compare_83(entry, name83)) return 1;
                global_lfn_buf[0] = '\0';
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return 0;
}

static uint8_t lfn_checksum(const char* short_name83) {
    uint8_t sum = 0;
    for (int i = 11; i > 0; i--) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)*short_name83++;
    }
    return sum;
}

static uint8_t calc_ntres(const char* src) {
    uint8_t ntres = 0;
    int has_lower_name = 0, has_upper_name = 0;
    int has_lower_ext = 0, has_upper_ext = 0;
    int in_ext = 0;

    for (int i = 0; src[i]; i++) {
        if (src[i] == '.') {
            in_ext = 1;
            continue;
        }
        if (!in_ext) {
            if (src[i] >= 'a' && src[i] <= 'z') has_lower_name = 1;
            if (src[i] >= 'A' && src[i] <= 'Z') has_upper_name = 1;
        } else {
            if (src[i] >= 'a' && src[i] <= 'z') has_lower_ext = 1;
            if (src[i] >= 'A' && src[i] <= 'Z') has_upper_ext = 1;
        }
    }
    if (has_lower_name && !has_upper_name) ntres |= 0x08;
    if (has_lower_ext && !has_upper_ext)   ntres |= 0x10;
    return ntres;
}

static void fill_lfn_entry(fat_lfn_entry_t* lfn, const char* name, int seq, int total_seq, uint8_t csum) {
    lfn->order = (seq == total_seq) ? (0x40 | seq) : seq;
    lfn->attribute = 0x0F;
    lfn->type = 0x00;
    lfn->checksum = csum;
    lfn->cluster_low = 0x0000;

    int start_char = (seq - 1) * 13;
    int name_len = 0;
    while (name[name_len]) name_len++;

    uint16_t chars[13];
    for (int i = 0; i < 13; i++) {
        int char_idx = start_char + i;
        if (char_idx < name_len) {
            chars[i] = (uint8_t)name[char_idx];
        } else if (char_idx == name_len) {
            chars[i] = 0x0000;
        } else {
            chars[i] = 0xFFFF;
        }
    }

    for (int i = 0; i < 5; i++) lfn->name1[i] = chars[i];
    for (int i = 0; i < 6; i++) lfn->name2[i] = chars[5 + i];
    for (int i = 0; i < 2; i++) lfn->name3[i] = chars[11 + i];
}

static int fat32_entry_exists(const char* name) {
    if (!g_mounted32) return 0;
    char name83[11];
    to_short_83(name, name83);

    char clean[64];
    int clen = 0;
    while (name[clen] && clen < 63) { clean[clen] = name[clen]; clen++; }
    while (clen > 0 && clean[clen - 1] == ' ') clen--;
    clean[clen] = '\0';

    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;
    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) return 0;
                if ((uint8_t)entry->filename[0] == 0xE5 || (entry->attributes & 0x08)) {
                    global_lfn_buf[0] = '\0';
                    continue;
                }
                if (entry->attributes == 0x0F) {
                    parse_lfn_entry((const fat_lfn_entry_t*)entry);
                    continue;
                }

                char filename[256];
                if (global_lfn_buf[0] != '\0') {
                    int p = 0; while (global_lfn_buf[p]) { filename[p] = global_lfn_buf[p]; p++; }
                    filename[p] = '\0';
                } else {
                    int p = 0;
                    for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') filename[p++] = entry->filename[c];
                    if (entry->ext[0] != ' ') {
                        filename[p++] = '.';
                        for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') filename[p++] = entry->ext[c];
                    }
                    filename[p] = '\0';
                }

                char short_name[13];
                int sp = 0;
                for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') short_name[sp++] = entry->filename[c];
                if (entry->ext[0] != ' ') {
                    short_name[sp++] = '.';
                    for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') short_name[sp++] = entry->ext[c];
                }
                short_name[sp] = '\0';

                if (strcasecmp(filename, clean) == 0 || strcasecmp(short_name, clean) == 0 || compare_83(entry, name83)) {
                    global_lfn_buf[0] = '\0';
                    return 1;
                }
                global_lfn_buf[0] = '\0';
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return 0;
}

int fat32_touch(const char* name) {
    if (!g_mounted32) return 0;
    if (fat32_entry_exists(name)) return 0;

    char clean_name[64];
    int len = 0;
    while (name[len] && len < 63) { clean_name[len] = name[len]; len++; }
    while (len > 0 && clean_name[len - 1] == ' ') len--;
    clean_name[len] = '\0';
    if (len == 0) return 0;

    char name83[11];
    to_short_83(clean_name, name83);
    uint8_t ntres = calc_ntres(clean_name);
    uint8_t csum  = lfn_checksum(name83);

    // Если укладывается в 8.3 — LFN-запись не нужна (нужен всего 1 слот)
    int num_lfn = is_valid_83(clean_name) ? 0 : ((len + 12) / 13);
    if (num_lfn > 4) num_lfn = 4;
    int total_entries = num_lfn + 1;

    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;
    uint32_t last_cluster = cluster;
    int written = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        last_cluster = cluster;
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) return 0;

            for (int i = 0; i <= 16 - total_entries; i++) {
                int all_free = 1;
                for (int k = 0; k < total_entries; k++) {
                    fat_dir_entry_t* e = (fat_dir_entry_t*)(sector_buf32 + (i + k) * 32);
                    if (e->filename[0] != 0x00 && (uint8_t)e->filename[0] != 0xE5) {
                        all_free = 0;
                        break;
                    }
                }

                if (all_free) {
                    if (num_lfn > 0) {
                        for (int seq = num_lfn; seq >= 1; seq--) {
                            int slot_idx = i + (num_lfn - seq);
                            fat_lfn_entry_t* lfn_slot = (fat_lfn_entry_t*)(sector_buf32 + slot_idx * 32);
                            fill_lfn_entry(lfn_slot, clean_name, seq, num_lfn, csum);
                        }
                    }

                    fat_dir_entry_t* entry = (fat_dir_entry_t*)(sector_buf32 + (i + num_lfn) * 32);
                    for (int c = 0; c < 8; c++) entry->filename[c] = name83[c];
                    for (int c = 0; c < 3; c++) entry->ext[c] = name83[8 + c];
                    entry->attributes = 0x20;
                    entry->reserved = ntres;
                    entry->creation_ms = 0;
                    entry->creation_time = 0;
                    entry->creation_date = 0x5021;
                    entry->last_access_date = 0x5021;
                    entry->cluster_high = 0;
                    entry->modify_time = 0;
                    entry->modify_date = 0x5021;
                    entry->cluster_low = 0;
                    entry->file_size = 0;

                    ehci_msc_write_sectors(cluster_lba + s, 1, sector_buf32);
                    written = 1;
                    break;
                }
            }
            if (written) break;

            // Если не поместились, закрываем висячий 0x00 в конце сектора маркером 0xE5,
            // чтобы каталог не обрывался для последующих секторов
            fat_dir_entry_t* last_e = (fat_dir_entry_t*)(sector_buf32 + 15 * 32);
            if (last_e->filename[0] == 0x00) {
                last_e->filename[0] = (char)0xE5;
                ehci_msc_write_sectors(cluster_lba + s, 1, sector_buf32);
            }
        }
        if (written) break;
        cluster = fat32_get_next_cluster(cluster);
    }

    if (!written) {
        uint32_t new_dir_cluster = fat32_find_free_cluster();
        if (new_dir_cluster == 0) return 0;
        fat32_set_cluster(last_cluster, new_dir_cluster);
        fat32_set_cluster(new_dir_cluster, 0x0FFFFFFF);

        static uint8_t zero_buf[512] __attribute__((aligned(64)));
        for (int i = 0; i < 512; i++) zero_buf[i] = 0;
        uint32_t new_dir_lba = g_data_lba + (new_dir_cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            ehci_msc_write_sectors(new_dir_lba + s, 1, zero_buf);
        }

        if (num_lfn > 0) {
            for (int seq = num_lfn; seq >= 1; seq--) {
                fat_lfn_entry_t* lfn_slot = (fat_lfn_entry_t*)(zero_buf + (num_lfn - seq) * 32);
                fill_lfn_entry(lfn_slot, clean_name, seq, num_lfn, csum);
            }
        }

        fat_dir_entry_t* entry = (fat_dir_entry_t*)(zero_buf + num_lfn * 32);
        for (int c = 0; c < 8; c++) entry->filename[c] = name83[c];
        for (int c = 0; c < 3; c++) entry->ext[c] = name83[8 + c];
        entry->attributes = 0x20;
        entry->reserved = ntres;
        entry->cluster_high = 0;
        entry->cluster_low = 0;
        entry->file_size = 0;
        ehci_msc_write_sectors(new_dir_lba, 1, zero_buf);
    }
    return 1;
}

int fat32_make_folder(const char* name) {
    if (!g_mounted32) return 0;
    if (fat32_entry_exists(name)) return 0;

    char clean_name[64];
    int len = 0;
    while (name[len] && len < 63) { clean_name[len] = name[len]; len++; }
    while (len > 0 && clean_name[len - 1] == ' ') len--;
    clean_name[len] = '\0';
    if (len == 0) return 0;

    char name83[11];
    to_short_83(clean_name, name83);
    uint8_t ntres = calc_ntres(clean_name);
    uint8_t csum  = lfn_checksum(name83);

    int num_lfn = is_valid_83(clean_name) ? 0 : ((len + 12) / 13);
    if (num_lfn > 4) num_lfn = 4;
    int total_entries = num_lfn + 1;

    uint32_t new_cluster = fat32_find_free_cluster();
    if (new_cluster == 0) return 0;
    fat32_set_cluster(new_cluster, 0x0FFFFFFF);

    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;
    int written = 0;
    uint32_t last_cluster = cluster;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        last_cluster = cluster;
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) return 0;

            for (int i = 0; i <= 16 - total_entries; i++) {
                int all_free = 1;
                for (int k = 0; k < total_entries; k++) {
                    fat_dir_entry_t* e = (fat_dir_entry_t*)(sector_buf32 + (i + k) * 32);
                    if (e->filename[0] != 0x00 && (uint8_t)e->filename[0] != 0xE5) {
                        all_free = 0;
                        break;
                    }
                }

                if (all_free) {
                    if (num_lfn > 0) {
                        for (int seq = num_lfn; seq >= 1; seq--) {
                            int slot_idx = i + (num_lfn - seq);
                            fat_lfn_entry_t* lfn_slot = (fat_lfn_entry_t*)(sector_buf32 + slot_idx * 32);
                            fill_lfn_entry(lfn_slot, clean_name, seq, num_lfn, csum);
                        }
                    }

                    fat_dir_entry_t* entry = (fat_dir_entry_t*)(sector_buf32 + (i + num_lfn) * 32);
                    for (int c = 0; c < 8; c++) entry->filename[c] = name83[c];
                    for (int c = 0; c < 3; c++) entry->ext[c] = name83[8 + c];
                    entry->attributes = 0x10;
                    entry->reserved = ntres;
                    entry->cluster_low = new_cluster & 0xFFFF;
                    entry->cluster_high = (new_cluster >> 16) & 0xFFFF;
                    entry->file_size = 0;

                    ehci_msc_write_sectors(cluster_lba + s, 1, sector_buf32);
                    written = 1;
                    break;
                }
            }
            if (written) break;

            fat_dir_entry_t* last_e = (fat_dir_entry_t*)(sector_buf32 + 15 * 32);
            if (last_e->filename[0] == 0x00) {
                last_e->filename[0] = (char)0xE5;
                ehci_msc_write_sectors(cluster_lba + s, 1, sector_buf32);
            }
        }
        if (written) break;
        cluster = fat32_get_next_cluster(cluster);
    }

    if (!written) {
        uint32_t new_dir_cluster = fat32_find_free_cluster();
        if (new_dir_cluster == 0) return 0;
        fat32_set_cluster(last_cluster, new_dir_cluster);
        fat32_set_cluster(new_dir_cluster, 0x0FFFFFFF);

        static uint8_t zero_buf[512] __attribute__((aligned(64)));
        for (int i = 0; i < 512; i++) zero_buf[i] = 0;
        uint32_t new_dir_lba = g_data_lba + (new_dir_cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            ehci_msc_write_sectors(new_dir_lba + s, 1, zero_buf);
        }

        if (num_lfn > 0) {
            for (int seq = num_lfn; seq >= 1; seq--) {
                fat_lfn_entry_t* lfn_slot = (fat_lfn_entry_t*)(zero_buf + (num_lfn - seq) * 32);
                fill_lfn_entry(lfn_slot, clean_name, seq, num_lfn, csum);
            }
        }

        fat_dir_entry_t* entry = (fat_dir_entry_t*)(zero_buf + num_lfn * 32);
        for (int c = 0; c < 8; c++) entry->filename[c] = name83[c];
        for (int c = 0; c < 3; c++) entry->ext[c] = name83[8 + c];
        entry->attributes = 0x10;
        entry->reserved = ntres;
        entry->cluster_low = new_cluster & 0xFFFF;
        entry->cluster_high = (new_cluster >> 16) & 0xFFFF;
        entry->file_size = 0;
        ehci_msc_write_sectors(new_dir_lba, 1, zero_buf);
    }

    // Инициализация папки записями "." и ".."
    static uint8_t zero_buf[512] __attribute__((aligned(64)));
    for (int i = 0; i < 512; i++) zero_buf[i] = 0;
    uint32_t new_lba = g_data_lba + (new_cluster - 2) * spc;
    for (uint32_t s = 0; s < spc; s++) {
        ehci_msc_write_sectors(new_lba + s, 1, zero_buf);
    }

    if (ehci_msc_read_sectors(new_lba, 1, sector_buf32)) {
        fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf32;

        for (int c = 0; c < 8; c++) entries[0].filename[c] = ' ';
        entries[0].filename[0] = '.';
        for (int c = 0; c < 3; c++) entries[0].ext[c] = ' ';
        entries[0].attributes = 0x10;
        entries[0].cluster_low = new_cluster & 0xFFFF;
        entries[0].cluster_high = (new_cluster >> 16) & 0xFFFF;
        entries[0].file_size = 0;

        for (int c = 0; c < 8; c++) entries[1].filename[c] = ' ';
        entries[1].filename[0] = '.';
        entries[1].filename[1] = '.';
        for (int c = 0; c < 3; c++) entries[1].ext[c] = ' ';
        entries[1].attributes = 0x10;
        uint32_t parent = (g_current_cluster32 == g_bpb32.root_cluster) ? 0 : g_current_cluster32;
        entries[1].cluster_low = parent & 0xFFFF;
        entries[1].cluster_high = (parent >> 16) & 0xFFFF;
        entries[1].file_size = 0;

        ehci_msc_write_sectors(new_lba, 1, sector_buf32);
    }
    return 1;
}

int fat32_remove_file(const char* name) {
    if (!g_mounted32) return 0;
    char name83[11];
    to_short_83(name, name83);

    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;
    uint32_t target_lba = 0;
    int entry_index = -1;
    uint32_t start_cluster = 0;

    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) return 0;
            fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buf32;

            for (int i = 0; i < 16; i++, entry++) {
                if (entry->filename[0] == 0x00) break;
                if ((uint8_t)entry->filename[0] == 0xE5) { global_lfn_buf[0] = '\0'; continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }
                if (entry->attributes & 0x08) { global_lfn_buf[0] = '\0'; continue; }

                char filename[256];
                if (global_lfn_buf[0] != '\0') {
                    int p = 0; while (global_lfn_buf[p]) { filename[p] = global_lfn_buf[p]; p++; }
                    filename[p] = '\0';
                } else {
                    int p = 0;
                    for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') filename[p++] = entry->filename[c];
                    if (entry->ext[0] != ' ') {
                        filename[p++] = '.';
                        for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') filename[p++] = entry->ext[c];
                    }
                    filename[p] = '\0';
                }

                char short_name[13];
                int sp = 0;
                for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') short_name[sp++] = entry->filename[c];
                if (entry->ext[0] != ' ') {
                    short_name[sp++] = '.';
                    for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') short_name[sp++] = entry->ext[c];
                }
                short_name[sp] = '\0';

                if (strcasecmp(filename, name) == 0 || strcasecmp(short_name, name) == 0 || compare_83(entry, name83)) {
                    target_lba = cluster_lba + s;
                    entry_index = i;
                    start_cluster = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
                    break;
                }
                global_lfn_buf[0] = '\0';
            }
            if (entry_index != -1) break;
        }
        if (entry_index != -1) break;
        cluster = fat32_get_next_cluster(cluster);
    }

    if (entry_index == -1) return 0;

    // Освобождаем цепочку кластеров в таблице FAT
    uint32_t cur = start_cluster;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        uint32_t next = fat32_get_next_cluster(cur);
        fat32_set_cluster(cur, 0x00000000);
        cur = next;
    }

    // Помечаем элемент каталога как удалённый (0xE5)
    if (!ehci_msc_read_sectors(target_lba, 1, sector_buf32)) return 0;
    fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf32;
    entries[entry_index].filename[0] = (char)0xE5;
    ehci_msc_write_sectors(target_lba, 1, sector_buf32);
    return 1;
}

int fat32_write_file(const char* name, const void* in_buffer, uint32_t bytes_to_write) {
    if (!g_mounted32) return 0;

    fat32_remove_file(name);
    if (!fat32_touch(name)) return 0;
    if (bytes_to_write == 0) return 1;

    uint32_t spc = g_bpb32.sectors_per_cluster;
    uint32_t bytes_per_cluster = spc * 512;
    uint32_t clusters_needed = (bytes_to_write + bytes_per_cluster - 1) / bytes_per_cluster;

    uint32_t first_cluster = 0;
    uint32_t prev_cluster = 0;
    uint32_t bytes_written = 0;
    const uint8_t* in_ptr = (const uint8_t*)in_buffer;

    for (uint32_t c = 0; c < clusters_needed; c++) {
        uint32_t cur_cluster = fat32_find_free_cluster();
        if (cur_cluster == 0) return 0;

        if (c == 0) first_cluster = cur_cluster;
        else fat32_set_cluster(prev_cluster, cur_cluster);

        fat32_set_cluster(cur_cluster, 0x0FFFFFFF);
        prev_cluster = cur_cluster;

        uint32_t cluster_lba = g_data_lba + (cur_cluster - 2) * spc;

        for (uint32_t s = 0; s < spc; s++) {
            static uint8_t sec_tmp[512] __attribute__((aligned(64)));
            for (int b = 0; b < 512; b++) sec_tmp[b] = 0;

            uint32_t chunk = 512;
            if (bytes_written + chunk > bytes_to_write) {
                chunk = bytes_to_write - bytes_written;
            }

            for (uint32_t b = 0; b < chunk; b++) {
                sec_tmp[b] = in_ptr[bytes_written + b];
            }

            ehci_msc_write_sectors(cluster_lba + s, 1, sec_tmp);
            bytes_written += chunk;
            if (bytes_written >= bytes_to_write) break;
        }
    }

    // Обновляем размер и стартовый кластер файла в записи каталога
    char name83[11];
    to_short_83(name, name83);

    uint32_t dir_cluster = g_current_cluster32;
    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';

    while (dir_cluster >= 2 && dir_cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (dir_cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) return 0;
            fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buf32;

            for (int i = 0; i < 16; i++, entry++) {
                if (entry->filename[0] == 0x00) break;
                if ((uint8_t)entry->filename[0] == 0xE5) { global_lfn_buf[0] = '\0'; continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }

                char filename[256];
                if (global_lfn_buf[0] != '\0') {
                    int p = 0; while (global_lfn_buf[p]) { filename[p] = global_lfn_buf[p]; p++; }
                    filename[p] = '\0';
                } else {
                    int p = 0;
                    for (int c = 0; c < 8; c++) if (entry->filename[c] != ' ') filename[p++] = entry->filename[c];
                    if (entry->ext[0] != ' ') {
                        filename[p++] = '.';
                        for (int c = 0; c < 3; c++) if (entry->ext[c] != ' ') filename[p++] = entry->ext[c];
                    }
                    filename[p] = '\0';
                }

                if (strcasecmp(filename, name) == 0 || compare_83(entry, name83)) {
                    entry->cluster_low = first_cluster & 0xFFFF;
                    entry->cluster_high = (first_cluster >> 16) & 0xFFFF;
                    entry->file_size = bytes_to_write;
                    ehci_msc_write_sectors(cluster_lba + s, 1, sector_buf32);
                    return 1;
                }
                global_lfn_buf[0] = '\0';
            }
        }
        dir_cluster = fat32_get_next_cluster(dir_cluster);
    }
    return 1;
}

int fat32_get_dir_files(fs_file_info_t* out_list, int max_files) {
    if (!g_mounted32) return 0;
    int count = 0;
    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;

    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!ehci_msc_read_sectors(cluster_lba + s, 1, sector_buf32)) return count;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) return count;
                if ((uint8_t)entry->filename[0] == 0xE5) { global_lfn_buf[0] = '\0'; continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }
                if (entry->attributes & 0x08) { global_lfn_buf[0] = '\0'; continue; } // Метка тома

                if (entry->filename[0] == '.') {
                    global_lfn_buf[0] = '\0';
                    continue;
                }

                char display_name[64];
                if (global_lfn_buf[0] != '\0') {
                    int p = 0;
                    while (global_lfn_buf[p] && p < 63) {
                        display_name[p] = global_lfn_buf[p];
                        p++;
                    }
                    display_name[p] = '\0';
                } else {
                    uint8_t ntres = entry->reserved;
                    int lower_name = (ntres & 0x08);
                    int lower_ext  = (ntres & 0x10);

                    int p = 0;
                    for (int c = 0; c < 8; c++) {
                        if (entry->filename[c] != ' ') {
                            char ch = entry->filename[c];
                            if (lower_name && ch >= 'A' && ch <= 'Z') ch += 32;
                            if (p < 60) display_name[p++] = ch;
                        }
                    }
                    if (entry->ext[0] != ' ') {
                        if (p < 60) display_name[p++] = '.';
                        for (int c = 0; c < 3; c++) {
                            if (entry->ext[c] != ' ') {
                                char ch = entry->ext[c];
                                if (lower_ext && ch >= 'A' && ch <= 'Z') ch += 32;
                                if (p < 63) display_name[p++] = ch;
                            }
                        }
                    }
                    display_name[p] = '\0';
                }

                int p = 0;
                while (display_name[p] && p < 63) {
                    out_list[count].name[p] = display_name[p];
                    out_list[count].clean_name[p] = display_name[p];
                    p++;
                }
                out_list[count].name[p] = '\0';
                out_list[count].clean_name[p] = '\0';

                out_list[count].attr = entry->attributes;
                out_list[count].size = entry->file_size;
                out_list[count].cluster = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;

                count++;
                global_lfn_buf[0] = '\0';

                if (count >= max_files) return count;
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return count;
}

// расчет реального объема и занятого места на диске
void fat32_get_stats(uint32_t* out_used_mb, uint32_t* out_total_mb) {
    if (!g_mounted32 || g_bpb32.bytes_per_sector == 0) {
        if (out_used_mb) *out_used_mb = 0;
        if (out_total_mb) *out_total_mb = 0;
        return;
    }

    uint32_t bytes_per_sec = g_bpb32.bytes_per_sector;
    uint32_t sec_per_cluster = g_bpb32.sectors_per_cluster;
    uint32_t cluster_size_bytes = bytes_per_sec * sec_per_cluster;

    // Общее количество секторов тома
    uint32_t total_sectors = g_bpb32.total_sectors_long;
    if (total_sectors == 0) {
        total_sectors = g_bpb32.total_sectors_short;
    }

    // Сектора области данных = Всего секторов - Зарезервированные - FAT-таблицы
    uint32_t fat_sectors_total = g_bpb32.fat_count * g_bpb32.sectors_per_fat_32;
    uint32_t non_data_sectors = g_bpb32.reserved_sectors + fat_sectors_total;
    uint32_t data_sectors = (total_sectors > non_data_sectors) ? (total_sectors - non_data_sectors) : 0;

    // Общее количество доступных кластеров данных (начиная с кластера 2)
    uint32_t total_clusters = data_sectors / sec_per_cluster;

    // Считаем занятые кластеры по таблице FAT (1 сектор FAT = 128 записей)
    uint32_t total_fat_sectors = g_bpb32.sectors_per_fat_32;
    uint32_t occupied_clusters = 0;

    for (uint32_t s = 0; s < total_fat_sectors; s++) {
        if (!ehci_msc_read_sectors(g_fat_lba + s, 1, sector_buf32)) break;
        uint32_t* entries = (uint32_t*)sector_buf32;

        int start_idx = (s == 0) ? 2 : 0;
        for (int i = start_idx; i < 128; i++) {
            uint32_t cluster_idx = s * 128 + i;
            if (cluster_idx >= total_clusters + 2) break;

            // Если элемент FAT != 0x00000000, кластер распределен (занят)
            if ((entries[i] & 0x0FFFFFFF) != 0x00000000) {
                occupied_clusters++;
            }
        }
        if (s * 128 >= total_clusters + 2) break;
    }

    // Переводим в мегабайты (MiB)
    uint64_t total_bytes = (uint64_t)total_clusters * cluster_size_bytes;
    uint64_t used_bytes  = (uint64_t)occupied_clusters * cluster_size_bytes;

    if (out_total_mb) *out_total_mb = (uint32_t)(total_bytes / (1024 * 1024));
    if (out_used_mb)  *out_used_mb  = (uint32_t)(used_bytes / (1024 * 1024));
}