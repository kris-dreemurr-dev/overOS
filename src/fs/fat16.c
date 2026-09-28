#include "fat16.h"
#include "../drivers/usb/ehci-msc.h"

extern void kdebug(const char* str, uint32_t color);
extern void itoa(int n, char* str);
extern void hex_str(uint32_t val, char* out);
extern int strcmp(const char* s1, const char* s2);
extern void flush_buffer(void);

static fat16_bpb_t g_bpb;
static uint32_t g_partition_lba = 2048;
static uint32_t g_fat_lba = 0;
static uint32_t g_root_lba = 0;
static uint32_t g_data_lba = 0;
static uint32_t g_root_sectors = 0;
static int      g_mounted = 0;

static uint16_t g_current_cluster = 0;
static uint8_t sector_buffer[512] __attribute__((aligned(64)));

int fat16_is_mounted(void) { return g_mounted; }

void fat16_go_root(void) {
    g_current_cluster = 0;
}

int fat16_mount(int root_mode) {
    g_mounted = 0;
    g_current_cluster = 0;

    if (root_mode) {
        g_partition_lba = 0;
    } else {
        if (!ehci_msc_read_sectors(0, 1, sector_buffer)) return 0;
        uint16_t sig = sector_buffer[510] | (sector_buffer[511] << 8);
        if (sig != 0xAA55) return 0;

        // Ищем раздел FAT16 (типы 0x04, 0x06, 0x0E) среди 4 записей MBR
        uint32_t fat_lba = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t type = sector_buffer[446 + (i * 16) + 4];
            if (type == 0x04 || type == 0x06 || type == 0x0E) {
                fat_lba = *(uint32_t*)&sector_buffer[446 + (i * 16) + 8];
                break;
            }
        }

        g_partition_lba = (fat_lba != 0) ? fat_lba : 2048;
    }

    if (!ehci_msc_read_sectors(g_partition_lba, 1, sector_buffer)) return 0;
    for (int i = 0; i < (int)sizeof(fat16_bpb_t); i++) {
        ((uint8_t*)&g_bpb)[i] = sector_buffer[i];
    }

    if (g_bpb.bytes_per_sector != 512) return 0;

    g_root_sectors = ((g_bpb.root_entries * 32) + 511) / 512;
    g_fat_lba  = g_partition_lba + g_bpb.reserved_sectors;
    g_root_lba = g_fat_lba + (g_bpb.fat_count * g_bpb.sectors_per_fat);
    g_data_lba = g_root_lba + g_root_sectors;

    g_mounted = 1;
    return 1;
}

// Читает i-й сектор текущего каталога (корень или кластер подпапки)
static int fat16_read_current_dir_sector(uint32_t sector_idx, uint8_t* buf) {
    if (g_current_cluster == 0) {
        if (sector_idx >= g_root_sectors) return 0;
        return ehci_msc_read_sectors(g_root_lba + sector_idx, 1, buf);
    } else {
        uint32_t cluster_lba = g_data_lba + (g_current_cluster - 2) * g_bpb.sectors_per_cluster;
        if (sector_idx >= g_bpb.sectors_per_cluster) return 0;
        return ehci_msc_read_sectors(cluster_lba + sector_idx, 1, buf);
    }
}

// Записывает i-й сектор текущего каталога обратно на диск
static int fat16_write_current_dir_sector(uint32_t sector_idx, const uint8_t* buf) {
    if (g_current_cluster == 0) {
        if (sector_idx >= g_root_sectors) return 0;
        return ehci_msc_write_sectors(g_root_lba + sector_idx, 1, buf);
    } else {
        uint32_t cluster_lba = g_data_lba + (g_current_cluster - 2) * g_bpb.sectors_per_cluster;
        if (sector_idx >= g_bpb.sectors_per_cluster) return 0;
        return ehci_msc_write_sectors(cluster_lba + sector_idx, 1, buf);
    }
}

// Безопасное и точное сравнение 11-байтного имени 8.3
static int compare_83(const fat_dir_entry_t* entry, const char* name83) {
    for (int i = 0; i < 8; i++) {
        if (entry->filename[i] != name83[i]) return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (entry->ext[i] != name83[8 + i]) return 0;
    }
    return 1;
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
        out_buf[i++] = ' '; out_buf[i++] = 'K'; out_buf[i++] = 'B'; out_buf[i] = '\0';
    } else {
        itoa(size / (1024 * 1024), tmp);
        int i = 0; while(tmp[i]) { out_buf[i] = tmp[i]; i++; }
        out_buf[i++] = ' '; out_buf[i++] = 'M'; out_buf[i++] = 'B'; out_buf[i] = '\0';
    }
}

void fat16_dir(void) {
    if (!g_mounted) { kdebug("[-] Not mounted!\n", 0xFF5555); return; }

    kdebug("\nDirectory contents:\n", 0x55FFFF);
    kdebug("NAME    EXT   TYPE      SIZE   \n", 0xAAAAAA);
    kdebug("-------------------------------\n", 0xAAAAAA);

    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;
    
    for (uint32_t s = 0; s < max_sectors; s++) {
        if (!fat16_read_current_dir_sector(s, sector_buffer)) break;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00) return;
            if ((uint8_t)entry->filename[0] == 0xE5 || entry->attributes == 0x0F || (entry->attributes & 0x08)) continue;

            char name[9], ext[4];
            for (int c = 0; c < 8; c++) name[c] = entry->filename[c];
            name[8] = '\0';
            for (int c = 0; c < 3; c++) ext[c] = entry->ext[c];
            ext[3] = '\0';

            kdebug(name, 0xFFFF55); kdebug(" ", 0);
            kdebug(ext, 0xFFFF55); kdebug("   ", 0);

            if (entry->attributes & 0x10) {
                kdebug("<DIR>     ", 0x55FF55);
            } else {
                char sz_str[16];
                format_size(entry->file_size, sz_str);
                kdebug(sz_str, 0x55FF55);
                int pad = 10; for(int l=0; sz_str[l]; l++) pad--;
                while(pad-- > 0) kdebug(" ", 0);
            }

            char buf[16];
            itoa(entry->cluster_low, buf);
            kdebug(buf, 0xAAAAAA);
            kdebug("\n", 0);
        }
    }
}

int fat16_change_dir(const char* name83) {
    if (strcmp(name83, "/") == 0) {
        g_current_cluster = 0;
        return 1;
    }

    if (strcmp(name83, "..") == 0) {
        if (g_current_cluster == 0) return 1;
        // Читаем первый сектор текущего кластера подпапки, чтобы забрать запись '..' (индекс 1)
        uint32_t cluster_lba = g_data_lba + (g_current_cluster - 2) * g_bpb.sectors_per_cluster;
        if (ehci_msc_read_sectors(cluster_lba, 1, sector_buffer)) {
            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buffer;
            g_current_cluster = entries[1].cluster_low; // Родительский кластер (0 для корня)
            return 1;
        }
        return 0;
    }

    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;
    
    for (uint32_t s = 0; s < max_sectors; s++) {
        if (!fat16_read_current_dir_sector(s, sector_buffer)) break;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00) break;
            if ((uint8_t)entry->filename[0] == 0xE5 || entry->attributes == 0x0F) continue;

            if (compare_83(entry, name83) && (entry->attributes & 0x10)) {
                g_current_cluster = entry->cluster_low;
                return 1;
            }
        }
    }
    return 0;
}

static uint16_t fat16_find_free_cluster(void) {
    for (uint32_t s = 0; s < g_bpb.sectors_per_fat; s++) {
        if (!ehci_msc_read_sectors(g_fat_lba + s, 1, sector_buffer)) return 0;
        uint16_t* fat_entries = (uint16_t*)sector_buffer;
        int start_idx = (s == 0) ? 2 : 0;
        for (int i = start_idx; i < 256; i++) {
            if (fat_entries[i] == 0x0000) return (uint16_t)(s * 256 + i);
        }
    }
    return 0;
}

static uint32_t g_cached_fat_sec = 0xFFFFFFFF;
static uint8_t  g_cached_fat_buf[512] __attribute__((aligned(64)));

static void fat16_set_cluster(uint16_t cluster, uint16_t value) {
    g_cached_fat_sec = 0xFFFFFFFF; // Сбрасываем кэш при любой записи в FAT!
    uint32_t fat_offset = cluster * 2;
    uint32_t fat_sector = g_fat_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    for (int f = 0; f < g_bpb.fat_count; f++) {
        uint32_t target_sec = fat_sector + (f * g_bpb.sectors_per_fat);
        ehci_msc_read_sectors(target_sec, 1, sector_buffer);
        *(uint16_t*)&sector_buffer[entry_offset] = value;
        ehci_msc_write_sectors(target_sec, 1, sector_buffer);
    }
}

int fat16_touch(const char* name83) {
    if (!g_mounted) return 0;
    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;

    for (uint32_t s = 0; s < max_sectors; s++) {
        if (!fat16_read_current_dir_sector(s, sector_buffer)) break;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00 || (uint8_t)entry->filename[0] == 0xE5) {
                for (int c = 0; c < 8; c++) entry->filename[c] = name83[c];
                for (int c = 0; c < 3; c++) entry->ext[c] = name83[8 + c];
                entry->attributes = 0x20;
                entry->cluster_low = 0;
                entry->file_size = 0;
                fat16_write_current_dir_sector(s, sector_buffer);
                return 1;
            }
        }
    }
    return 0;
}

int fat16_make_folder(const char* name83) {
    if (!g_mounted) return 0;
    uint16_t new_cluster = fat16_find_free_cluster();
    if (new_cluster == 0) return 0;
    fat16_set_cluster(new_cluster, 0xFFFF);

    int written = 0;
    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;

    for (uint32_t s = 0; s < max_sectors; s++) {
        if (!fat16_read_current_dir_sector(s, sector_buffer)) break;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00 || (uint8_t)entry->filename[0] == 0xE5) {
                for (int c = 0; c < 8; c++) entry->filename[c] = name83[c];
                for (int c = 0; c < 3; c++) entry->ext[c] = name83[8 + c];
                entry->attributes = 0x10;
                entry->cluster_low = new_cluster;
                entry->file_size = 0;
                fat16_write_current_dir_sector(s, sector_buffer);
                written = 1;
                break;
            }
        }
        if (written) break;
    }
    if (!written) return 0;

    // Инициализация содержимого нового кластера папки (записи "." и "..")
    static uint8_t zero_buf[512] __attribute__((aligned(64)));
    for (int i = 0; i < 512; i++) zero_buf[i] = 0;
    uint32_t cluster_lba = g_data_lba + (new_cluster - 2) * g_bpb.sectors_per_cluster;
    
    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
        ehci_msc_write_sectors(cluster_lba + s, 1, zero_buf);
    }

    if (ehci_msc_read_sectors(cluster_lba, 1, sector_buffer)) {
        fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buffer;
        
        // Запись "." (ссылка на себя)
        for (int c = 0; c < 8; c++) entries[0].filename[c] = ' ';
        entries[0].filename[0] = '.';
        for (int c = 0; c < 3; c++) entries[0].ext[c] = ' ';
        entries[0].attributes = 0x10;
        entries[0].cluster_low = new_cluster;
        entries[0].file_size = 0;

        // Запись ".." (ссылка на родителя)
        for (int c = 0; c < 8; c++) entries[1].filename[c] = ' ';
        entries[1].filename[0] = '.';
        entries[1].filename[1] = '.';
        for (int c = 0; c < 3; c++) entries[1].ext[c] = ' ';
        entries[1].attributes = 0x10;
        entries[1].cluster_low = g_current_cluster;
        entries[1].file_size = 0;

        ehci_msc_write_sectors(cluster_lba, 1, sector_buffer);
    }

    return 1;
}

int fat16_remove_file(const char* name83) {
    if (!g_mounted) return 0;
    uint32_t target_sector = 0;
    int entry_index = -1;
    uint16_t start_cluster = 0;
    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;

    for (uint32_t s = 0; s < max_sectors; s++) {
        uint32_t current_sec_idx = s;
        if (!fat16_read_current_dir_sector(s, sector_buffer)) break;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00) break;
            if ((uint8_t)entry->filename[0] == 0xE5 || entry->attributes == 0x0F || (entry->attributes & 0x08)) continue;

            if (compare_83(entry, name83)) {
                target_sector = current_sec_idx;
                entry_index = i;
                start_cluster = entry->cluster_low;
                break;
            }
        }
        if (entry_index != -1) break;
    }

    if (entry_index == -1) return 0;

    uint16_t cluster = start_cluster;
    while (cluster >= 0x0002 && cluster < 0xFFF8) {
        uint32_t fat_offset = cluster * 2;
        uint32_t fat_sector = g_fat_lba + (fat_offset / 512);
        uint32_t entry_offset = fat_offset % 512;

        if (!ehci_msc_read_sectors(fat_sector, 1, sector_buffer)) break;
        uint16_t next_cluster = *(uint16_t*)&sector_buffer[entry_offset];
        fat16_set_cluster(cluster, 0x0000);
        cluster = next_cluster;
    }

    if (!fat16_read_current_dir_sector(target_sector, sector_buffer)) return 0;
    fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buffer;
    entries[entry_index].filename[0] = (char)0xE5;
    fat16_write_current_dir_sector(target_sector, sector_buffer);
    return 1;
}

int fat16_read_file(const char* name83, void* out_buffer, uint32_t max_bytes) {
    if (!g_mounted) return 0;
    g_cached_fat_sec = 0xFFFFFFFF; // Свежий кэш FAT перед чтением[cite: 19]

    uint16_t cluster = 0;
    uint32_t file_size = 0;
    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;

    // 1. Поиск файла в каталоге
    for (uint32_t s = 0; s < max_sectors; s++) {
        if (!fat16_read_current_dir_sector(s, sector_buffer)) return 0;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00) break;
            if ((uint8_t)entry->filename[0] == 0xE5 || entry->attributes == 0x0F) continue;

            if (compare_83(entry, name83)) {
                cluster = entry->cluster_low;
                file_size = entry->file_size;
                break;
            }
        }
        if (cluster) break;
    }

    if (!cluster && file_size > 0) return 0;
    if (file_size == 0) return 0;

    uint32_t bytes_read = 0;
    uint8_t* out_ptr = (uint8_t*)out_buffer;
    uint32_t bytes_per_cluster = g_bpb.sectors_per_cluster * 512;

    // 2. Блочное чтение данных по кластерам
    while (cluster >= 0x0002 && cluster < 0xFFF8 && bytes_read < file_size && bytes_read < max_bytes) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * g_bpb.sectors_per_cluster;

        uint32_t bytes_left_file = file_size - bytes_read;
        uint32_t bytes_left_buf  = max_bytes - bytes_read;
        uint32_t bytes_to_read   = bytes_per_cluster;
        if (bytes_to_read > bytes_left_file) bytes_to_read = bytes_left_file;
        if (bytes_to_read > bytes_left_buf)  bytes_to_read = bytes_left_buf;

        uint32_t sectors_to_read = (bytes_to_read + 511) / 512;

        if (sectors_to_read > 0) {
            // Читаем весь кластер (или остаток файла) ОДНИМ пакетным запросом прямо в память!
            if (!ehci_msc_read_sectors(cluster_lba, sectors_to_read, out_ptr + bytes_read)) {
                return 0;
            }
            bytes_read += bytes_to_read;

            // Индикатор прогресса (точки) каждые 512 КБ[cite: 19]
            if ((bytes_read / (512 * 1024)) != ((bytes_read - bytes_to_read) / (512 * 1024))) {
                //kdebug(".", 0x55FF55);
                //flush_buffer();
            }
        }

        if (bytes_read >= file_size || bytes_read >= max_bytes) break;

        // 3. Быстрый переход к следующему кластеру через кэш FAT
        uint32_t fat_offset = cluster * 2;
        uint32_t fat_sector = g_fat_lba + (fat_offset / 512);
        uint32_t entry_offset = fat_offset % 512;

        if (fat_sector != g_cached_fat_sec) {
            if (!ehci_msc_read_sectors(fat_sector, 1, g_cached_fat_buf)) return 0;
            g_cached_fat_sec = fat_sector;
        }
        cluster = *(uint16_t*)&g_cached_fat_buf[entry_offset];
    }
    return bytes_read;
}

int fat16_get_dir_files(fat16_file_info_t* out_list, int max_files) {
    if (!g_mounted) return 0;
    int count = 0;
    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;

    for (uint32_t s = 0; s < max_sectors; s++) {
        if (!fat16_read_current_dir_sector(s, sector_buffer)) break;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00) return count;
            if ((uint8_t)entry->filename[0] == 0xE5 || entry->attributes == 0x0F) continue;
            if (entry->attributes & 0x18) continue; // Пропускаем папки и метки тома

            for (int c = 0; c < 8; c++) out_list[count].name83[c] = entry->filename[c];
            for (int c = 0; c < 3; c++) out_list[count].name83[8 + c] = entry->ext[c];
            out_list[count].name83[11] = '\0';
            out_list[count].size = entry->file_size;

            int p = 0;
            for (int c = 0; c < 8; c++) {
                if (entry->filename[c] != ' ') out_list[count].clean_name[p++] = entry->filename[c];
            }
            if (entry->ext[0] != ' ') {
                out_list[count].clean_name[p++] = '.';
                for (int c = 0; c < 3; c++) {
                    if (entry->ext[c] != ' ') out_list[count].clean_name[p++] = entry->ext[c];
                }
            }
            out_list[count].clean_name[p] = '\0';

            count++;
            if (count >= max_files) return count;
        }
    }
    return count;
}

int fat16_write_file(const char* name83, const void* in_buffer, uint32_t bytes_to_write) {
    if (!g_mounted) return 0;

    fat16_remove_file(name83);
    if (!fat16_touch(name83)) return 0;
    if (bytes_to_write == 0) return 1;

    uint32_t bytes_per_cluster = g_bpb.sectors_per_cluster * 512;
    uint32_t clusters_needed = (bytes_to_write + bytes_per_cluster - 1) / bytes_per_cluster;

    uint16_t first_cluster = 0;
    uint16_t prev_cluster = 0;
    uint32_t bytes_written = 0;
    const uint8_t* in_ptr = (const uint8_t*)in_buffer;

    for (uint32_t c = 0; c < clusters_needed; c++) {
        uint16_t cur_cluster = fat16_find_free_cluster();
        if (cur_cluster == 0) return 0;

        if (c == 0) first_cluster = cur_cluster;
        else fat16_set_cluster(prev_cluster, cur_cluster);

        fat16_set_cluster(cur_cluster, 0xFFFF);
        prev_cluster = cur_cluster;

        uint32_t cluster_lba = g_data_lba + (cur_cluster - 2) * g_bpb.sectors_per_cluster;

        for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
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

    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;
    for (uint32_t s = 0; s < max_sectors; s++) {
        if (!fat16_read_current_dir_sector(s, sector_buffer)) break;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00) break;
            if (compare_83(entry, name83)) {
                entry->cluster_low = first_cluster;
                entry->file_size = bytes_to_write;
                fat16_write_current_dir_sector(s, sector_buffer);
                return 1;
            }
        }
    }
    return 1;
}

int fat16_file_exists(const char* name83) {
    if (!g_mounted) return 0;
    uint32_t max_sectors = (g_current_cluster == 0) ? g_root_sectors : g_bpb.sectors_per_cluster;

    for (uint32_t s = 0; s < max_sectors; s++) {
        if (!fat16_read_current_dir_sector(s, sector_buffer)) break;
        fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buffer;

        for (int i = 0; i < 16; i++, entry++) {
            if (entry->filename[0] == 0x00) return 0;
            if ((uint8_t)entry->filename[0] == 0xE5 || entry->attributes == 0x0F || (entry->attributes & 0x18)) continue;
            if (compare_83(entry, name83)) return 1;
        }
    }
    return 0;
}