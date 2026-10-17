#include "fat32.h"
#include "../drivers/blk.h"

extern void kputs(const char* str, uint32_t color);
extern void kdebug(const char* str, uint32_t color);
extern void itoa(int n, char* str);
extern int strcmp(const char* s1, const char* s2);
extern void flush_buffer(void);

// ---------------------------------------------------------------------------
// Тома. Раньше всё состояние драйвера (BPB, начало раздела, текущий кластер,
// кэш сектора FAT) было в глобальных переменных, и том мог быть ровно один --
// корневой. Теперь это запись таблицы, а глобальные имена ниже -- макросы на
// поля ВЫБРАННОГО тома (fat32_select). Сам код драйвера от этого не изменился:
// он по-прежнему работает "с томом", просто том теперь любой. Выбирает том
// fs.c перед каждой операцией, под fs_lock.
// ---------------------------------------------------------------------------
typedef struct {
    int         disk;              // номер диска в drivers/blk.c, -1 -- диска нет
    fat32_bpb_t bpb;
    uint32_t    partition_lba;
    uint32_t    fat_lba;
    uint32_t    data_lba;
    int         mounted;
    uint32_t    current_cluster;
    uint32_t    cached_fat_sec;
    int         fsinfo_done;       // подсказка FSInfo уже помечена "неизвестно"
    uint8_t     cached_fat_buf[512] __attribute__((aligned(64)));
} fat32_vol_t;

static fat32_vol_t  s_vols[FAT32_MAX_VOLS];
static fat32_vol_t* V = &s_vols[0];

#define g_bpb32              (V->bpb)
#define g_partition_lba      (V->partition_lba)
#define g_fat_lba            (V->fat_lba)
#define g_data_lba           (V->data_lba)
#define g_mounted32          (V->mounted)
#define g_current_cluster32  (V->current_cluster)
#define g_cached_fat_sec32   (V->cached_fat_sec)
#define g_cached_fat_buf32   (V->cached_fat_buf)
// Секторы читаются с диска выбранного тома
#define usb_blk_read_sectors(lba, n, buf)  blk_read(V->disk, (lba), (n), (buf))
#define usb_blk_write_sectors(lba, n, buf) blk_write(V->disk, (lba), (n), (buf))

static int      fat32_find_entry_in_dir(uint32_t dir_cluster, const char* name, fat_dir_entry_t* out_entry);
static uint32_t fat32_find_dir_cluster_by_path(const char* path);

static uint8_t  sector_buf32[512] __attribute__((aligned(64)));

// Буфер сборщика длинных имен (LFN)
static char global_lfn_buf[260];

// Корневой том смонтирован (том 0): так спрашивают загрузчик и fs_is_mounted
int fat32_is_mounted(void) { return s_vols[0].mounted; }

void fat32_select(int vol) {
    if (vol < 0 || vol >= FAT32_MAX_VOLS) vol = 0;
    V = &s_vols[vol];
}

int fat32_vol_mounted(int vol) {
    return vol >= 0 && vol < FAT32_MAX_VOLS && s_vols[vol].mounted;
}

int fat32_vol_disk(int vol) {
    return (vol >= 0 && vol < FAT32_MAX_VOLS) ? s_vols[vol].disk : -1;
}

void fat32_go_root(void) {
    g_current_cluster32 = g_bpb32.root_cluster;
}

// Смонтировать FAT32 с сектора lba диска disk как том vol (и выбрать его).
// Раздел ищет вызывающий (fs/mount.c разбирает MBR и GPT).
int fat32_mount_vol(int vol, int disk, uint32_t lba) {
    fat32_select(vol);
    V->mounted = 0;
    V->disk = disk;
    V->cached_fat_sec = 0xFFFFFFFF;
    V->fsinfo_done = 0;
    V->partition_lba = lba;

    if (!usb_blk_read_sectors(lba, 1, sector_buf32)) return 0;
    for (int i = 0; i < (int)sizeof(fat32_bpb_t); i++) ((uint8_t*)&g_bpb32)[i] = sector_buf32[i];
    if (g_bpb32.bytes_per_sector != 512 || !g_bpb32.sectors_per_cluster || !g_bpb32.fat_count ||
        !g_bpb32.sectors_per_fat_32 || g_bpb32.root_cluster < 2) return 0;

    g_fat_lba = lba + g_bpb32.reserved_sectors;
    g_data_lba = g_fat_lba + (g_bpb32.fat_count * g_bpb32.sectors_per_fat_32);
    g_current_cluster32 = g_bpb32.root_cluster;
    V->mounted = 1;
    return 1;
}

void fat32_unmount_vol(int vol) {
    if (vol < 0 || vol >= FAT32_MAX_VOLS) return;
    s_vols[vol].mounted = 0;
    s_vols[vol].disk = -1;
    s_vols[vol].cached_fat_sec = 0xFFFFFFFF;
}

// Диск тома пропал: том остаётся "смонтированным" (корень ждёт свою флешку
// обратно, и пути вроде /sys должны отвечать "нет", а не "нет такого тома"),
// но ни одно чтение до диска больше не доходит.
void fat32_detach_disk(int vol) {
    if (vol < 0 || vol >= FAT32_MAX_VOLS) return;
    s_vols[vol].disk = -1;
    s_vols[vol].cached_fat_sec = 0xFFFFFFFF;
}

uint32_t fat32_vol_serial(int vol) {
    if (!fat32_vol_mounted(vol)) return 0;
    return s_vols[vol].bpb.volume_id;
}

// Метка тома. Windows и mkfs пишут её в двух местах: в BPB и записью с
// атрибутом 0x08 в корневом каталоге, причём при переименовании меняется
// только вторая -- её и берём первой. "NO NAME" -- значит метки нет.
void fat32_vol_label(int vol, char* out, int max) {
    out[0] = '\0';
    if (!fat32_vol_mounted(vol) || max < 2) return;
    fat32_vol_t* saved = V;
    fat32_select(vol);
    char lab[12];
    int found = 0;
    uint32_t lba = g_data_lba + (g_bpb32.root_cluster - 2) * g_bpb32.sectors_per_cluster;
    if (usb_blk_read_sectors(lba, 1, sector_buf32)) {
        for (int i = 0; i < 16; i++) {
            fat_dir_entry_t* e = (fat_dir_entry_t*)(sector_buf32 + i * 32);
            if (e->filename[0] == 0) break;
            if ((uint8_t)e->filename[0] == 0xE5) continue;
            if ((e->attributes & 0x0F) == 0x0F) continue;          // запись LFN
            if (e->attributes & 0x08) {
                for (int k = 0; k < 11; k++) lab[k] = (k < 8) ? e->filename[k] : e->ext[k - 8];
                found = 1;
                break;
            }
        }
    }
    if (!found) for (int k = 0; k < 11; k++) lab[k] = g_bpb32.volume_label[k];
    lab[11] = '\0';
    int n = 11;
    while (n > 0 && (lab[n - 1] == ' ' || lab[n - 1] == 0)) n--;
    lab[n] = '\0';
    V = saved;
    if (!strcmp(lab, "NO NAME")) return;
    int p = 0;
    for (int k = 0; k < n && p < max - 1; k++) out[p++] = lab[k];
    out[p] = '\0';
}

static uint32_t fat32_get_next_cluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_fat_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    if (fat_sector != g_cached_fat_sec32) {
        if (!usb_blk_read_sectors(fat_sector, 1, g_cached_fat_buf32)) return 0x0FFFFFFF;
        g_cached_fat_sec32 = fat_sector;
    }
    uint32_t next = *(uint32_t*)&g_cached_fat_buf32[entry_offset];
    return next & 0x0FFFFFFF;
}

// FSInfo хранит подсказку "сколько кластеров свободно". Мы её не ведём, и
// после первой же записи она врёт (fsck: "Free cluster summary wrong"). На
// своей флешке это безобидно, а чужую потом вставят в Windows, которая этой
// подсказке верит. Спецификация разрешает значение 0xFFFFFFFF -- "неизвестно,
// посчитай сам": ставим его один раз на монтирование, перед первым изменением FAT.
static void fat32_fsinfo_invalidate(void) {
    // свой буфер: вызывающий может держать в sector_buf32 сектор каталога
    static uint8_t fsinfo_buf[512] __attribute__((aligned(64)));
    if (V->fsinfo_done || !g_bpb32.fs_info || g_bpb32.fs_info == 0xFFFF) return;
    V->fsinfo_done = 1;
    uint32_t lba = g_partition_lba + g_bpb32.fs_info;
    if (!usb_blk_read_sectors(lba, 1, fsinfo_buf)) return;
    if (*(uint32_t*)&fsinfo_buf[0] != 0x41615252 || *(uint32_t*)&fsinfo_buf[484] != 0x61417272) return;
    *(uint32_t*)&fsinfo_buf[488] = 0xFFFFFFFF;
    *(uint32_t*)&fsinfo_buf[492] = 0xFFFFFFFF;
    usb_blk_write_sectors(lba, 1, fsinfo_buf);
}

static void fat32_set_cluster(uint32_t cluster, uint32_t value) {
    fat32_fsinfo_invalidate();
    g_cached_fat_sec32 = 0xFFFFFFFF; // Сброс кэша чтения
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_fat_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    for (int f = 0; f < g_bpb32.fat_count; f++) {
        uint32_t target_sec = fat_sector + (f * g_bpb32.sectors_per_fat_32);
        if (!usb_blk_read_sectors(target_sec, 1, sector_buf32)) return;
        uint32_t old = *(uint32_t*)&sector_buf32[entry_offset];
        *(uint32_t*)&sector_buf32[entry_offset] = (old & 0xF0000000) | (value & 0x0FFFFFFF);
        usb_blk_write_sectors(target_sec, 1, sector_buf32);
    }
}

static uint32_t fat32_find_free_cluster(void) {
    uint32_t total_fat_sectors = g_bpb32.sectors_per_fat_32;
    for (uint32_t s = 0; s < total_fat_sectors; s++) {
        if (!usb_blk_read_sectors(g_fat_lba + s, 1, sector_buf32)) return 0;
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
    int i = 0;
    if (size < 1024) {
        itoa(size, tmp);
        while (tmp[i]) { out_buf[i] = tmp[i]; i++; }
        out_buf[i++] = ' '; out_buf[i++] = 'B'; out_buf[i] = '\0';
    } else if (size < 1024 * 1024) {
        itoa(size / 1024, tmp);
        while (tmp[i]) { out_buf[i] = tmp[i]; i++; }
        out_buf[i++] = ' '; out_buf[i++] = 'K'; out_buf[i++] = 'B'; out_buf[i] = '\0';
    } else {
        itoa(size / (1024 * 1024), tmp);
        while (tmp[i]) { out_buf[i] = tmp[i]; i++; }
        out_buf[i++] = ' '; out_buf[i++] = 'M'; out_buf[i++] = 'B'; out_buf[i] = '\0';
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

static uint8_t g_lfn_checksum = 0;   // контрольная сумма 8.3-имени из LFN-записей
static void lfn_reset(void);

// Контрольная сумма 8.3-имени (спецификация FAT): LFN относится к записи, только если она совпадает
static uint8_t lfn_checksum_83(const uint8_t* name11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

// Длинное имя от удалённого файла ("сирота") не должно приклеиться к чужой записи,
// занявшей его слот, — иначе /etc/locale.conf читается как содержимое другого файла
static void lfn_validate(const fat_dir_entry_t* e) {
    if (global_lfn_buf[0] != '\0' && lfn_checksum_83((const uint8_t*)e->filename) != g_lfn_checksum)
        lfn_reset();
}

// Длинное имя в FAT32 хранится в UTF-16, а система работает в UTF-8 (консоль
// переводит UTF-8 в CP866 при выводе). Поэтому сначала собираем имя в кодовых
// единицах UTF-16 по их позициям, а в UTF-8 переводим, когда имя собрано
// целиком: длина в байтах и в символах разная, по месту переводить нельзя.
static uint16_t g_lfn_u16[256];
static int      g_lfn_u16_len = 0;

static void lfn_reset(void) {
    g_lfn_u16_len = 0;
    global_lfn_buf[0] = '\0';
}

// UTF-16 -> UTF-8 (кириллица, латиница и прочая двухбайтная часть BMP)
static void lfn_to_utf8(void) {
    int o = 0;
    for (int i = 0; i < g_lfn_u16_len && o < 250; i++) {
        uint16_t c = g_lfn_u16[i];
        if (!c) break;
        if (c < 0x80) {
            global_lfn_buf[o++] = (char)c;
        } else if (c < 0x800) {
            global_lfn_buf[o++] = (char)(0xC0 | (c >> 6));
            global_lfn_buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            global_lfn_buf[o++] = (char)(0xE0 | (c >> 12));
            global_lfn_buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            global_lfn_buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    global_lfn_buf[o] = '\0';
}

static void parse_lfn_entry(const fat_lfn_entry_t* lfn) {
    g_lfn_checksum = lfn->checksum;
    int seq = lfn->order & 0x1F;
    if (seq < 1 || seq > 19) return;
    int idx = (seq - 1) * 13;

    uint16_t chars[13] = {
        lfn->name1[0], lfn->name1[1], lfn->name1[2], lfn->name1[3], lfn->name1[4],
        lfn->name2[0], lfn->name2[1], lfn->name2[2], lfn->name2[3], lfn->name2[4], lfn->name2[5],
        lfn->name3[0], lfn->name3[1]
    };

    for (int i = 0; i < 13; i++) {
        uint16_t c = chars[i];
        int pos = idx + i;
        if (pos >= 255) break;
        if (c == 0x0000 || c == 0xFFFF) {
            g_lfn_u16[pos] = 0;
            if (pos > g_lfn_u16_len) g_lfn_u16_len = pos;
            break;
        }
        g_lfn_u16[pos] = c;
        if (pos + 1 > g_lfn_u16_len) g_lfn_u16_len = pos + 1;
    }
    lfn_to_utf8();
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
// Байт, недопустимый в коротком имени (в том числе любой байт UTF-8 выше ASCII),
// заменяется подчёркиванием: настоящее имя всё равно хранится в записях LFN.
static char short_char(unsigned char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (char)c;
    if (c == '$' || c == '%' || c == '-' || c == '_' || c == '@' || c == '~' ||
        c == '`' || c == '!' || c == '(' || c == ')' || c == '{' || c == '}' ||
        c == '^' || c == '#' || c == '&') return (char)c;
    return '_';
}

static void to_short_83(const char* src, char* out83) {
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    int i = 0, d = 0;
    while (src[i] && src[i] != '.' && d < 8) out83[d++] = short_char((unsigned char)src[i++]);
    while (src[i] && src[i] != '.') i++;
    if (src[i] == '.') {
        i++;
        d = 8;
        while (src[i] && d < 11) out83[d++] = short_char((unsigned char)src[i++]);
    }
    if (out83[0] == ' ') out83[0] = '_';
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
    g_lfn_u16_len = 0;
    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;

    kputs("\nDirectory contents:\n", 0x55FFFF);
    kputs("NAME                 SIZE\n", 0xAAAAAA);
    kputs("--------------------------------\n", 0xAAAAAA);

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) return;
                if ((uint8_t)entry->filename[0] == 0xE5) { lfn_reset(); continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }
                if (entry->attributes & 0x08) { lfn_reset(); continue; }

                char display_name[256];
                lfn_validate(entry);
                if (global_lfn_buf[0] != '\0') {
                    int p = 0; while (global_lfn_buf[p]) { display_name[p] = global_lfn_buf[p]; p++; }
                    display_name[p] = '\0';
                } else {
                    uint8_t ntres = entry->reserved;
                    int lower_name = (ntres & 0x08), lower_ext = (ntres & 0x10);

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
                    int i = 0;
                    if (entry->file_size < 1024) {
                        itoa(entry->file_size, sz_str);
                        while (sz_str[i]) i++;
                        sz_str[i++] = ' '; sz_str[i++] = 'B'; sz_str[i] = '\0';
                    } else {
                        itoa(entry->file_size / 1024, sz_str);
                        while (sz_str[i]) i++;
                        sz_str[i++] = ' '; sz_str[i++] = 'K'; sz_str[i] = '\0';
                    }
                    kputs(sz_str, 0x55FF55);
                    kputs("\n", 0);
                }
                lfn_reset();
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
        if (usb_blk_read_sectors(cluster_lba, 1, sector_buf32)) {
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
    g_lfn_u16_len = 0;
    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;

    // 4. Поиск целевой директории
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
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
                lfn_validate(entry);
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

                lfn_reset();
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }

    return 0;
}

static int fat32_resolve_path_to_entry(const char* path, fat_dir_entry_t* out_entry) {
    if (!path || !path[0]) return 0;

    // Ищем последний слэш
    const char* last_slash = 0;
    for (const char* s = path; *s; s++) {
        if (*s == '/') last_slash = s;
    }

    uint32_t dir_cluster = g_current_cluster32;
    const char* fname = path;

    if (last_slash) {
        if (last_slash == path) {
            // Файл лежит прямо в корне: "/file.prg"
            dir_cluster = g_bpb32.root_cluster;
        } else {
            // Файл во вложенной папке: "/desprogs/ovfiles.prg" или "desprogs/ovfiles.prg"
            char dir_path[256];
            int dlen = (int)(last_slash - path);
            if (dlen >= 255) dlen = 255;
            for (int i = 0; i < dlen; i++) dir_path[i] = path[i];
            dir_path[dlen] = '\0';

            dir_cluster = fat32_find_dir_cluster_by_path(dir_path);
            if (dir_cluster < 2) return 0;
        }
        fname = last_slash + 1;
    }

    if (!fname[0]) return 0;
    return fat32_find_entry_in_dir(dir_cluster, fname, out_entry);
}


// Чтение с произвольного смещения. Целые сектора идут прямо в буфер вызывающего,
// а неполные (в начале и в конце) — через sector_buf32: раньше последний сектор читался
// целиком прямо в буфер и затирал до 511 байт за его концом.
int fat32_read_at(const char* name, uint32_t offset, void* out_buffer, uint32_t max_bytes) {
    if (!g_mounted32 || !out_buffer || max_bytes == 0) return 0;

    fat_dir_entry_t entry;
    if (!fat32_resolve_path_to_entry(name, &entry)) return 0;
    if (entry.attributes & 0x10) return 0; // Папку читать как файл нельзя

    uint32_t cluster = ((uint32_t)entry.cluster_high << 16) | entry.cluster_low;
    uint32_t file_size = entry.file_size;
    if (!cluster || offset >= file_size) return 0;

    uint32_t total = file_size - offset;
    if (total > max_bytes) total = max_bytes;

    uint32_t spc = g_bpb32.sectors_per_cluster;
    uint32_t bpc = spc * 512;

    // Пропускаем кластеры до смещения
    for (uint32_t skip = offset / bpc; skip > 0; skip--) {
        cluster = fat32_get_next_cluster(cluster);
        if (cluster < 2 || cluster >= 0x0FFFFFF8) return 0;
    }
    uint32_t in_cluster = offset % bpc;

    uint8_t* out = (uint8_t*)out_buffer;
    uint32_t done = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8 && done < total) {
        // Пакет подряд идущих кластеров — одним запросом к флешке
        uint32_t run_start = cluster;
        uint32_t run_count = 1;
        while (done + run_count * bpc - in_cluster < total) {
            uint32_t next = fat32_get_next_cluster(run_start + run_count - 1);
            if (next != run_start + run_count) break;
            run_count++;
        }

        uint32_t run_bytes = run_count * bpc - in_cluster;
        if (run_bytes > total - done) run_bytes = total - done;

        uint32_t lba = g_data_lba + (run_start - 2) * spc + in_cluster / 512;
        uint32_t sec_off = in_cluster % 512;
        uint32_t left = run_bytes;

        if (sec_off) {                                   // хвост сектора в начале
            if (!usb_blk_read_sectors(lba, 1, sector_buf32)) return (int)done;
            uint32_t n = 512 - sec_off;
            if (n > left) n = left;
            for (uint32_t i = 0; i < n; i++) out[done + i] = sector_buf32[sec_off + i];
            done += n; left -= n; lba++;
        }
        uint32_t full = left / 512;
        if (full) {                                      // целые сектора — напрямую
            if (!usb_blk_read_sectors(lba, full, out + done)) return (int)done;
            done += full * 512; left -= full * 512; lba += full;
        }
        if (left) {                                      // начало сектора в конце
            if (!usb_blk_read_sectors(lba, 1, sector_buf32)) return (int)done;
            for (uint32_t i = 0; i < left; i++) out[done + i] = sector_buf32[i];
            done += left;
        }

        cluster = fat32_get_next_cluster(run_start + run_count - 1);
        in_cluster = 0;
    }

    return (int)done;
}

int fat32_read_file(const char* name, void* out_buffer, uint32_t max_bytes) {
    return fat32_read_at(name, 0, out_buffer, max_bytes);
}

// Размер файла (0 — нет файла или это папка)
uint32_t fat32_file_size(const char* name) {
    if (!g_mounted32) return 0;
    fat_dir_entry_t entry;
    if (!fat32_resolve_path_to_entry(name, &entry)) return 0;
    if (entry.attributes & 0x10) return 0;
    return entry.file_size;
}

int fat32_file_exists(const char* name) {
    if (!g_mounted32) return 0;
    fat_dir_entry_t entry;
    if (!fat32_resolve_path_to_entry(name, &entry)) return 0;
    return !(entry.attributes & 0x08); // Исключаем метку тома
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

// UTF-8 -> UTF-16 для записи длинного имени. Возвращает число кодовых единиц.
static int utf8_to_utf16(const char* src, uint16_t* out, int max) {
    int n = 0;
    const uint8_t* p = (const uint8_t*)src;
    while (*p && n < max) {
        uint32_t c;
        if (*p < 0x80) c = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { c = ((uint32_t)(*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            c = ((uint32_t)(*p & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else { c = '_'; p++; }                       // суррогаты и битые байты
        if (c > 0xFFFF) c = '_';
        out[n++] = (uint16_t)c;
    }
    return n;
}

static void fill_lfn_entry(fat_lfn_entry_t* lfn, const uint16_t* name, int name_len,
                           int seq, int total_seq, uint8_t csum) {
    lfn->order = (seq == total_seq) ? (0x40 | seq) : seq;
    lfn->attribute = 0x0F;
    lfn->type = 0x00;
    lfn->checksum = csum;
    lfn->cluster_low = 0x0000;

    int start_char = (seq - 1) * 13;

    uint16_t chars[13];
    for (int i = 0; i < 13; i++) {
        int char_idx = start_char + i;
        if (char_idx < name_len) {
            chars[i] = name[char_idx];
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
    g_lfn_u16_len = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) return 0;
                if ((uint8_t)entry->filename[0] == 0xE5 || (entry->attributes & 0x08)) {
                    lfn_reset();
                    continue;
                }
                if (entry->attributes == 0x0F) {
                    parse_lfn_entry((const fat_lfn_entry_t*)entry);
                    continue;
                }

                char filename[256];
                lfn_validate(entry);
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
                    lfn_reset();
                    return 1;
                }
                lfn_reset();
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return 0;
}

static uint32_t fat32_split_path(const char* path, char* out_filename, int max_fn_len) {
    if (!path || !path[0]) return 0;

    const char* last_slash = 0;
    for (const char* s = path; *s; s++) {
        if (*s == '/') last_slash = s;
    }

    if (!last_slash) {
        int i = 0;
        while (path[i] && i < max_fn_len - 1) { out_filename[i] = path[i]; i++; }
        out_filename[i] = '\0';
        return g_current_cluster32;
    }

    const char* fname = last_slash + 1;
    int i = 0;
    while (fname[i] && i < max_fn_len - 1) { out_filename[i] = fname[i]; i++; }
    out_filename[i] = '\0';

    if (last_slash == path) {
        return g_bpb32.root_cluster;
    }

    char dir_path[256];
    int dlen = (int)(last_slash - path);
    if (dlen >= 255) dlen = 255;
    for (int k = 0; k < dlen; k++) dir_path[k] = path[k];
    dir_path[dlen] = '\0';

    return fat32_find_dir_cluster_by_path(dir_path);
}

int fat32_touch(const char* name) {
    if (!g_mounted32) return 0;

    char filename_only[64];
    uint32_t target_dir_cluster = fat32_split_path(name, filename_only, sizeof(filename_only));
    if (target_dir_cluster < 2) return 0; // Папка не найдена

    // Временно подменяем текущий кластер на кластер целевой папки
    uint32_t saved_cluster = g_current_cluster32;
    g_current_cluster32 = target_dir_cluster;

    if (fat32_entry_exists(filename_only)) {
        g_current_cluster32 = saved_cluster;
        return 0;
    }

    char clean_name[64];
    int len = 0;
    while (filename_only[len] && len < 63) { clean_name[len] = filename_only[len]; len++; }
    while (len > 0 && clean_name[len - 1] == ' ') len--;
    clean_name[len] = '\0';
    if (len == 0) {
        g_current_cluster32 = saved_cluster;
        return 0;
    }

    char name83[11];
    to_short_83(clean_name, name83);
    uint8_t ntres = calc_ntres(clean_name);
    uint8_t csum  = lfn_checksum(name83);

    // Длина считается в кодовых единицах UTF-16, а не в байтах: у кириллицы
    // один символ занимает два байта UTF-8, и по байтам записей выходило вдвое
    // больше, чем нужно
    uint16_t name16[256];
    int len16 = utf8_to_utf16(clean_name, name16, 255);
    name16[len16] = 0;
    int num_lfn = is_valid_83(clean_name) ? 0 : ((len16 + 12) / 13);
    if (num_lfn > 19) num_lfn = 19;
    int total_entries = num_lfn + 1;

    uint32_t cluster = g_current_cluster32;
    uint32_t spc = g_bpb32.sectors_per_cluster;
    uint32_t last_cluster = cluster;
    int written = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        last_cluster = cluster;
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) {
                g_current_cluster32 = saved_cluster;
                return 0;
            }

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
                            fill_lfn_entry(lfn_slot, name16, len16, seq, num_lfn, csum);
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

                    usb_blk_write_sectors(cluster_lba + s, 1, sector_buf32);
                    written = 1;
                    break;
                }
            }
            if (written) break;

            fat_dir_entry_t* last_e = (fat_dir_entry_t*)(sector_buf32 + 15 * 32);
            if (last_e->filename[0] == 0x00) {
                last_e->filename[0] = (char)0xE5;
                usb_blk_write_sectors(cluster_lba + s, 1, sector_buf32);
            }
        }
        if (written) break;
        cluster = fat32_get_next_cluster(cluster);
    }

    if (!written) {
        uint32_t new_dir_cluster = fat32_find_free_cluster();
        if (new_dir_cluster == 0) {
            g_current_cluster32 = saved_cluster;
            return 0;
        }
        fat32_set_cluster(last_cluster, new_dir_cluster);
        fat32_set_cluster(new_dir_cluster, 0x0FFFFFFF);

        static uint8_t zero_buf[512] __attribute__((aligned(64)));
        for (int i = 0; i < 512; i++) zero_buf[i] = 0;
        uint32_t new_dir_lba = g_data_lba + (new_dir_cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            usb_blk_write_sectors(new_dir_lba + s, 1, zero_buf);
        }

        if (num_lfn > 0) {
            for (int seq = num_lfn; seq >= 1; seq--) {
                fat_lfn_entry_t* lfn_slot = (fat_lfn_entry_t*)(zero_buf + (num_lfn - seq) * 32);
                fill_lfn_entry(lfn_slot, name16, len16, seq, num_lfn, csum);
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
        usb_blk_write_sectors(new_dir_lba, 1, zero_buf);
    }

    g_current_cluster32 = saved_cluster; // Восстанавливаем рабочую директорию
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

    // Длина считается в кодовых единицах UTF-16, а не в байтах: у кириллицы
    // один символ занимает два байта UTF-8, и по байтам записей выходило вдвое
    // больше, чем нужно
    uint16_t name16[256];
    int len16 = utf8_to_utf16(clean_name, name16, 255);
    name16[len16] = 0;
    int num_lfn = is_valid_83(clean_name) ? 0 : ((len16 + 12) / 13);
    if (num_lfn > 19) num_lfn = 19;
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
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) return 0;

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
                            fill_lfn_entry(lfn_slot, name16, len16, seq, num_lfn, csum);
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

                    usb_blk_write_sectors(cluster_lba + s, 1, sector_buf32);
                    written = 1;
                    break;
                }
            }
            if (written) break;

            fat_dir_entry_t* last_e = (fat_dir_entry_t*)(sector_buf32 + 15 * 32);
            if (last_e->filename[0] == 0x00) {
                last_e->filename[0] = (char)0xE5;
                usb_blk_write_sectors(cluster_lba + s, 1, sector_buf32);
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
            usb_blk_write_sectors(new_dir_lba + s, 1, zero_buf);
        }

        if (num_lfn > 0) {
            for (int seq = num_lfn; seq >= 1; seq--) {
                fat_lfn_entry_t* lfn_slot = (fat_lfn_entry_t*)(zero_buf + (num_lfn - seq) * 32);
                fill_lfn_entry(lfn_slot, name16, len16, seq, num_lfn, csum);
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
        usb_blk_write_sectors(new_dir_lba, 1, zero_buf);
    }

    // Инициализация папки записями "." и ".."
    static uint8_t zero_buf[512] __attribute__((aligned(64)));
    for (int i = 0; i < 512; i++) zero_buf[i] = 0;
    uint32_t new_lba = g_data_lba + (new_cluster - 2) * spc;
    for (uint32_t s = 0; s < spc; s++) {
        usb_blk_write_sectors(new_lba + s, 1, zero_buf);
    }

    if (usb_blk_read_sectors(new_lba, 1, sector_buf32)) {
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

        usb_blk_write_sectors(new_lba, 1, sector_buf32);
    }
    return 1;
}

int fat32_remove_file(const char* name) {
    if (!g_mounted32) return 0;

    // 1. Извлекаем кластер каталога и короткое имя файла
    char filename_only[64];
    uint32_t dir_cluster = fat32_split_path(name, filename_only, sizeof(filename_only));
    if (dir_cluster < 2) return 0;

    char name83[11];
    to_short_83(filename_only, name83);

    uint32_t cluster = dir_cluster;
    uint32_t spc = g_bpb32.sectors_per_cluster;
    uint32_t target_lba = 0;
    int entry_index = -1;
    uint32_t start_cluster = 0;

    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';
    g_lfn_u16_len = 0;

    // 2. Поиск файла строго внутри целевого каталога
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) return 0;
            fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buf32;

            for (int i = 0; i < 16; i++, entry++) {
                if (entry->filename[0] == 0x00) break;
                if ((uint8_t)entry->filename[0] == 0xE5) { lfn_reset(); continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }
                if (entry->attributes & 0x08) { lfn_reset(); continue; }

                char filename[256];
                lfn_validate(entry);
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

                if (strcasecmp(filename, filename_only) == 0 || strcasecmp(short_name, filename_only) == 0 || compare_83(entry, name83)) {
                    target_lba = cluster_lba + s;
                    entry_index = i;
                    start_cluster = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
                    break;
                }
                lfn_reset();
            }
            if (entry_index != -1) break;
        }
        if (entry_index != -1) break;
        cluster = fat32_get_next_cluster(cluster);
    }

    if (entry_index == -1) return 0;

    // 3. Освобождаем цепочку кластеров в таблице FAT
    uint32_t cur = start_cluster;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        uint32_t next = fat32_get_next_cluster(cur);
        fat32_set_cluster(cur, 0x00000000);
        cur = next;
    }

    // 4. Помечаем элемент каталога как удалённый (0xE5) — вместе с его LFN-записями перед ним
    if (!usb_blk_read_sectors(target_lba, 1, sector_buf32)) return 0;
    fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf32;
    entries[entry_index].filename[0] = (char)0xE5;
    int k = entry_index - 1;
    while (k >= 0 && entries[k].attributes == 0x0F && (uint8_t)entries[k].filename[0] != 0xE5) {
        entries[k].filename[0] = (char)0xE5;
        k--;
    }
    usb_blk_write_sectors(target_lba, 1, sector_buf32);

    // LFN-цепочка могла начаться в предыдущем секторе того же кластера
    if (k < 0 && target_lba > g_data_lba) {
        uint32_t prev_lba = target_lba - 1;
        uint32_t rel = target_lba - g_data_lba;
        if (rel % spc != 0 && usb_blk_read_sectors(prev_lba, 1, sector_buf32)) {
            fat_dir_entry_t* prev = (fat_dir_entry_t*)sector_buf32;
            int changed = 0;
            for (int j = 15; j >= 0 && prev[j].attributes == 0x0F && (uint8_t)prev[j].filename[0] != 0xE5; j--) {
                prev[j].filename[0] = (char)0xE5;
                changed = 1;
            }
            if (changed) usb_blk_write_sectors(prev_lba, 1, sector_buf32);
        }
    }
    return 1;
}

int fat32_write_file(const char* name, const void* in_buffer, uint32_t bytes_to_write) {
    if (!g_mounted32) return 0;

    // 1. Проверяем валидность целевой директории
    char filename_only[64];
    uint32_t target_dir_cluster = fat32_split_path(name, filename_only, sizeof(filename_only));
    if (target_dir_cluster < 2) return 0;

    // 2. Удаляем старый файл по полному пути и создаем пустой файл
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

    // 3. Аллокация кластеров и запись тела файла
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

            usb_blk_write_sectors(cluster_lba + s, 1, sec_tmp);
            bytes_written += chunk;
            if (bytes_written >= bytes_to_write) break;
        }
    }

    // 4. Поиск записи каталога строго в целевой папке и обновление размера/кластера
    char name83[11];
    to_short_83(filename_only, name83);

    uint32_t dir_cluster = target_dir_cluster;
    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';
    g_lfn_u16_len = 0;

    while (dir_cluster >= 2 && dir_cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (dir_cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) return 0;
            fat_dir_entry_t* entry = (fat_dir_entry_t*)sector_buf32;

            for (int i = 0; i < 16; i++, entry++) {
                if (entry->filename[0] == 0x00) break;
                if ((uint8_t)entry->filename[0] == 0xE5) { lfn_reset(); continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }

                char filename[256];
                lfn_validate(entry);
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

                if (strcasecmp(filename, filename_only) == 0 || compare_83(entry, name83)) {
                    entry->cluster_low = first_cluster & 0xFFFF;
                    entry->cluster_high = (first_cluster >> 16) & 0xFFFF;
                    entry->file_size = bytes_to_write;
                    usb_blk_write_sectors(cluster_lba + s, 1, sector_buf32);
                    return 1;
                }
                lfn_reset();
            }
        }
        dir_cluster = fat32_get_next_cluster(dir_cluster);
    }
    return 1;
}

// Ищет файл/папку name строго внутри кластера dir_cluster
static int fat32_find_entry_in_dir(uint32_t dir_cluster, const char* name, fat_dir_entry_t* out_entry) {
    if (!g_mounted32 || dir_cluster < 2) return 0;

    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';
    g_lfn_u16_len = 0;
    uint32_t cluster = dir_cluster;
    uint32_t spc = g_bpb32.sectors_per_cluster;

    char name83[11];
    to_short_83(name, name83);

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) break;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) return 0;
                if ((uint8_t)entry->filename[0] == 0xE5) { lfn_reset(); continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }

                char filename[256];
                lfn_validate(entry);
                if (global_lfn_buf[0] != '\0') {
                    int p = 0; while (global_lfn_buf[p]) { filename[p] = global_lfn_buf[p]; p++; }
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

                if (strcasecmp(filename, name) == 0 || strcasecmp(short_name, name) == 0 || compare_83(entry, name83)) {
                    if (out_entry) *out_entry = *entry;
                    lfn_reset();
                    return 1;
                }
                lfn_reset();
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return 0;
}

static uint32_t fat32_walk_segments(uint32_t start_cluster, const char* path) {
    uint32_t cluster = start_cluster;
    const char* p = path;
    char seg[64];

    while (*p) {
        int si = 0;
        while (*p && *p != '/' && si < 63) seg[si++] = *p++;
        seg[si] = '\0';
        while (*p == '/') p++;

        if (si == 0 || strcmp(seg, ".") == 0) continue;

        if (strcmp(seg, "..") == 0) {
            if (cluster == g_bpb32.root_cluster) continue;
            uint32_t cluster_lba = g_data_lba + (cluster - 2) * g_bpb32.sectors_per_cluster;
            if (!usb_blk_read_sectors(cluster_lba, 1, sector_buf32)) return 0;
            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf32;
            uint32_t parent = ((uint32_t)entries[1].cluster_high << 16) | entries[1].cluster_low;
            cluster = (parent == 0) ? g_bpb32.root_cluster : parent;
            continue;
        }

        fat_dir_entry_t entry;
        if (!fat32_find_entry_in_dir(cluster, seg, &entry)) return 0;
        if (!(entry.attributes & 0x10)) return 0; // Не директория

        uint32_t next = ((uint32_t)entry.cluster_high << 16) | entry.cluster_low;
        cluster = (next == 0) ? g_bpb32.root_cluster : next;
    }
    return cluster;
}

static uint32_t fat32_find_dir_cluster_by_path(const char* path) {
    if (!path || !path[0]) return g_current_cluster32;

    // 1. Путь от корня
    if (path[0] == '/') {
        const char* p = path;
        while (*p == '/') p++;
        if (!*p) return g_bpb32.root_cluster;
        return fat32_walk_segments(g_bpb32.root_cluster, p);
    }

    // 2. Относительный путь: сначала ищем в текущей папке
    uint32_t target = fat32_walk_segments(g_current_cluster32, path);
    if (target >= 2) return target;

    // 3. Фоллбэк: если не найдено в текущей — проверяем от корня тома
    return fat32_walk_segments(g_bpb32.root_cluster, path);
}

// Существует ли каталог по пути (абсолютному или относительному текущему)
int fat32_dir_exists(const char* path) {
    if (!g_mounted32) return 0;
    if (!path || !path[0]) return 1;
    if (path[0] == '/' && path[1] == '\0') return 1;      // корень есть всегда
    return fat32_find_dir_cluster_by_path(path) >= 2;
}

// Чтение списка файлов любой папки (если path == NULL или "", читается текущая папка)
int fat32_get_dir_files(const char* path, fs_file_info_t* out_list, int max_files) {
    if (!g_mounted32) return 0;

    uint32_t cluster = g_current_cluster32;
    if (path && path[0]) {
        uint32_t target = fat32_find_dir_cluster_by_path(path);
        if (target >= 2) cluster = target;
        else return 0;
    }

    int count = 0;
    uint32_t spc = g_bpb32.sectors_per_cluster;
    for (int i = 0; i < 260; i++) global_lfn_buf[i] = '\0';
    g_lfn_u16_len = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = g_data_lba + (cluster - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            if (!usb_blk_read_sectors(cluster_lba + s, 1, sector_buf32)) return count;
            uint8_t* ptr = sector_buf32;

            for (int i = 0; i < 16; i++, ptr += 32) {
                fat_dir_entry_t* entry = (fat_dir_entry_t*)ptr;
                if (entry->filename[0] == 0x00) return count;
                if ((uint8_t)entry->filename[0] == 0xE5) { lfn_reset(); continue; }
                if (entry->attributes == 0x0F) { parse_lfn_entry((const fat_lfn_entry_t*)entry); continue; }
                if (entry->attributes & 0x08) { lfn_reset(); continue; }

                if (entry->filename[0] == '.') {
                    lfn_reset();
                    continue;
                }

                char display_name[64];
                lfn_validate(entry);
                if (global_lfn_buf[0] != '\0') {
                    int p = 0;
                    while (global_lfn_buf[p] && p < 63) { display_name[p] = global_lfn_buf[p]; p++; }
                    display_name[p] = '\0';
                } else {
                    uint8_t ntres = entry->reserved;
                    int lower_name = (ntres & 0x08), lower_ext = (ntres & 0x10);

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
                lfn_reset();

                if (count >= max_files) return count;
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return count;
}

// расчет реального объема и занятого места на диске
// Размер тома без обхода FAT. Занятое место считается чтением всей таблицы
// (сотни секторов с флешки), и на загрузке это лишние секунды — там нужен
// только размер, он лежит в BPB.
uint32_t fat32_total_mb(void) {
    fat32_select(0);
    if (!g_mounted32 || g_bpb32.bytes_per_sector == 0) return 0;
    uint32_t total_sectors = g_bpb32.total_sectors_long;
    if (total_sectors == 0) total_sectors = g_bpb32.total_sectors_short;
    uint32_t non_data = g_bpb32.reserved_sectors + g_bpb32.fat_count * g_bpb32.sectors_per_fat_32;
    uint32_t data_sectors = (total_sectors > non_data) ? (total_sectors - non_data) : 0;
    return (uint32_t)(((uint64_t)data_sectors * g_bpb32.bytes_per_sector) / (1024 * 1024));
}

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
        if (!usb_blk_read_sectors(g_fat_lba + s, 1, sector_buf32)) break;
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