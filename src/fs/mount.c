// =============================================================================
// mount.c — тома и точки монтирования. Ближе всего к udisks в Linux.
//
// Снизу -- реестр дисков (drivers/blk.c): драйверы USB регистрируют там
// найденные накопители и отмечают пропавшие. Здесь решается, что с ними делать:
//   * при загрузке: какой диск -- корень (FAT32, на котором есть /sys/KERNEL.BIN);
//   * потом, в потоке storaged: новый диск -- разобрать таблицу разделов
//     (MBR, GPT или флешка без таблицы), подключить каждый FAT32 в
//     /media/<метка> и сказать рабочему столу "обнаружено устройство";
//   * диск пропал -- снять его тома; если это корень -- ждать ту же флешку
//     обратно (по VID:PID, размеру и серийному номеру тома) и перемонтировать.
//
// Сам драйвер USB о файловой системе не знает, а файловая система -- о USB.
// =============================================================================

#include "mount.h"
#include "fat32.h"
#include "fs.h"
#include "../drivers/blk.h"
#include "../drivers/usb/usb.h"
#include "../kernel/notify.h"
#include "../kernel/sched.h"
#include "../kernel/tty.h"

extern void kputs(const char* str, uint32_t color);
extern void flush_buffer(void);
extern int  strcmp(const char* a, const char* b);
extern void fs_volume_changed(int vol);

// ------------------------------------------------------------------ тома
typedef struct {
    int      used;
    int      disk;
    uint32_t lba;
    uint32_t serial;
    char     name[24];          // каталог в /media
    char     label[24];
} mnt_t;

static mnt_t s_mnt[FAT32_MAX_VOLS];       // [0] -- корень

// Корень и его опознание
static volatile int s_root_state = MOUNT_ROOT_OK;
static uint16_t s_root_vid, s_root_pid;
static uint32_t s_root_blocks, s_root_lba, s_root_serial;

// По дискам: извлекли ли безопасно и какую карточку "подключено" показали
static uint8_t  s_ejected[BLK_MAX];
static uint32_t s_note_id[BLK_MAX];

#define NOTE_ROOT 1                       // постоянный id карточки про корень

static int s_streq(const char* a, const char* b) { return strcmp(a, b) == 0; }

static void s_copy(char* d, const char* s, int max) {
    int i = 0;
    if (s) while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void s_cat(char* d, const char* s, int max) {
    int i = 0;
    while (d[i]) i++;
    if (s) while (*s && i < max - 1) d[i++] = *s++;
    d[i] = '\0';
}

// Сообщение в консоль -- только если на экране консоль, а не рабочий стол:
// поверх рабочего стола текст ядра рисовать нельзя, там будет уведомление.
static void console_note(const char* msg, uint32_t color) {
    tty_t* t = tty_get(tty_get_active_id());
    if (t && t->gfx_mode) return;
    kputs(msg, color);
    flush_buffer();
}

// ------------------------------------------------------- разделы
#define FS_NONE  0
#define FS_FAT32 1
#define FS_EXFAT 2
#define FS_NTFS  3
#define FS_FAT16 4

static const char* fs_name(int fs) {
    switch (fs) {
        case FS_FAT32: return "FAT32";
        case FS_EXFAT: return "exFAT";
        case FS_NTFS:  return "NTFS";
        case FS_FAT16: return "FAT16";
        default:       return "неизвестная";
    }
}

typedef struct { uint32_t lba; int fs; } part_t;
#define PART_MAX 4

static uint8_t s_sec[512] __attribute__((aligned(64)));

static int mem_eq(const uint8_t* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}

// Что лежит в первом секторе раздела. Только по сигнатурам загрузочного
// сектора: строка в поле fs_type у FAT32 (смещение 82), имя OEM у exFAT и NTFS.
static int fs_of_sector(const uint8_t* s) {
    if (s[510] != 0x55 || s[511] != 0xAA) return FS_NONE;
    if (mem_eq(s + 3, "EXFAT   ", 8)) return FS_EXFAT;
    if (mem_eq(s + 3, "NTFS    ", 8)) return FS_NTFS;
    uint16_t bps = (uint16_t)(s[11] | (s[12] << 8));
    if (bps != 512 || (s[0] != 0xEB && s[0] != 0xE9)) return FS_NONE;
    if (mem_eq(s + 82, "FAT32   ", 8)) return FS_FAT32;
    if (mem_eq(s + 54, "FAT16   ", 8) || mem_eq(s + 54, "FAT12   ", 8)) return FS_FAT16;
    return FS_NONE;
}

static int fs_at(int disk, uint32_t lba) {
    if (!blk_read(disk, lba, 1, s_sec)) return FS_NONE;
    return fs_of_sector(s_sec);
}

// GPT: заголовок в секторе 1, записи по 128 байт начиная с указанного LBA
static int gpt_scan(int disk, part_t* out, int max) {
    if (!blk_read(disk, 1, 1, s_sec) || !mem_eq(s_sec, "EFI PART", 8)) return 0;
    uint32_t ent_lba = s_sec[72] | (s_sec[73] << 8) | (s_sec[74] << 16) | ((uint32_t)s_sec[75] << 24);
    uint32_t count   = s_sec[80] | (s_sec[81] << 8) | (s_sec[82] << 16) | ((uint32_t)s_sec[83] << 24);
    uint32_t esize   = s_sec[84] | (s_sec[85] << 8) | (s_sec[86] << 16) | ((uint32_t)s_sec[87] << 24);
    if (esize < 128 || esize > 512 || count > 128) return 0;
    uint32_t first[PART_MAX];
    int n = 0;
    uint32_t per = 512 / esize;
    for (uint32_t k = 0; k < count && n < max; k++) {
        if (k % per == 0 && !blk_read(disk, ent_lba + k / per, 1, s_sec)) break;
        const uint8_t* e = s_sec + (k % per) * esize;
        int empty = 1;
        for (int i = 0; i < 16; i++) if (e[i]) { empty = 0; break; }
        if (empty) continue;
        first[n++] = e[32] | (e[33] << 8) | (e[34] << 16) | ((uint32_t)e[35] << 24);
    }
    for (int i = 0; i < n; i++) { out[i].lba = first[i]; out[i].fs = fs_at(disk, first[i]); }
    return n;
}

// Разделы диска с их файловыми системами. Флешка бывает и без таблицы
// разделов (весь носитель -- один том, "superfloppy"), так её форматируют
// некоторые фотоаппараты и mkfs без fdisk.
static int part_scan(int disk, part_t* out, int max) {
    if (!blk_read(disk, 0, 1, s_sec)) return 0;
    int whole = fs_of_sector(s_sec);
    if (whole != FS_NONE) { out[0].lba = 0; out[0].fs = whole; return 1; }
    if (s_sec[510] != 0x55 || s_sec[511] != 0xAA) return 0;

    uint32_t lba[4];
    uint8_t  type[4];
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = s_sec + 446 + i * 16;
        type[i] = e[4];
        lba[i] = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
    }
    for (int i = 0; i < 4; i++) if (type[i] == 0xEE) return gpt_scan(disk, out, max);

    int n = 0;
    for (int i = 0; i < 4 && n < max; i++) {
        // 0x7F -- служебный раздел нашего загрузчика (stage2), расширенные
        // разделы (0x05/0x0F) не разбираем
        if (!type[i] || type[i] == 0x7F || type[i] == 0x05 || type[i] == 0x0F || !lba[i]) continue;
        out[n].lba = lba[i];
        out[n].fs = fs_at(disk, lba[i]);
        n++;
    }
    return n;
}

// ------------------------------------------------------- имена в /media
static int name_ok_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.';
}

static int name_taken(int self, const char* n) {
    for (int v = 1; v < FAT32_MAX_VOLS; v++)
        if (v != self && s_mnt[v].used && s_streq(s_mnt[v].name, n)) return 1;
    return 0;
}

// Имя каталога: метка тома (пробелы -> '_'), без метки -- серийный номер
// "ABCD-1234", как его показывает Linux. Занято -- дописываем _2, _3...
static void make_name(int vol, char* out, int max) {
    char base[24];
    int p = 0;
    for (int i = 0; s_mnt[vol].label[i] && p < 20; i++) {
        char c = s_mnt[vol].label[i];
        base[p++] = name_ok_char(c) ? c : '_';
    }
    base[p] = '\0';
    if (!p) {
        const char* h = "0123456789ABCDEF";
        uint32_t s = s_mnt[vol].serial;
        for (int i = 0; i < 9; i++) {
            if (i == 4) { base[p++] = '-'; continue; }
            int sh = 28 - 4 * (i > 4 ? i - 1 : i);
            base[p++] = h[(s >> sh) & 0xF];
        }
        base[p] = '\0';
    }
    s_copy(out, base, max);
    for (int k = 2; name_taken(vol, out) && k < 10; k++) {
        s_copy(out, base, max);
        char suf[3] = { '_', (char)('0' + k), 0 };
        s_cat(out, suf, max);
    }
}

// ------------------------------------------------------- корень
int mount_root_state(void) { return s_root_state; }
int mount_root_disk(void)  { return s_mnt[0].used ? s_mnt[0].disk : -1; }

static void remember_root(int disk, uint32_t lba) {
    blk_dev_t* d = blk_get(disk);
    s_mnt[0].used = 1;
    s_mnt[0].disk = disk;
    s_mnt[0].lba = lba;
    s_mnt[0].serial = fat32_vol_serial(0);
    s_mnt[0].name[0] = '\0';
    fat32_vol_label(0, s_mnt[0].label, sizeof(s_mnt[0].label));
    s_root_vid = d->vid; s_root_pid = d->pid; s_root_blocks = d->blocks;
    s_root_lba = lba;
    s_root_serial = s_mnt[0].serial;
    d->handled = 1;
}

int mount_root(void) {
    int fb_disk = -1;
    uint32_t fb_lba = 0;
    // Перебираем диски в порядке появления: корень -- тот FAT32, на котором
    // лежит ядро. Если две флешки с FAT32, вторая не станет корнем по ошибке.
    for (int pass = 0; pass < BLK_MAX; pass++) {
        int disk = -1;
        uint32_t best = 0xFFFFFFFF;
        for (int i = 0; i < BLK_MAX; i++) {       // pass-й по возрасту диск
            blk_dev_t* d = blk_get(i);
            if (!d || d->gone || d->handled) continue;
            if (d->seq < best) { best = d->seq; disk = i; }
        }
        if (disk < 0) break;
        blk_get(disk)->handled = 2;               // временно: чтобы взять следующий

        part_t parts[PART_MAX];
        int n = part_scan(disk, parts, PART_MAX);
        for (int k = 0; k < n; k++) {
            if (parts[k].fs != FS_FAT32) continue;
            fs_lock();
            int ok = fat32_mount_vol(0, disk, parts[k].lba);
            int has_kernel = ok && fat32_file_exists("/sys/KERNEL.BIN");
            fs_unlock();
            if (has_kernel) {
                remember_root(disk, parts[k].lba);
                for (int i = 0; i < BLK_MAX; i++) { blk_dev_t* d = blk_get(i); if (d && d->handled == 2) d->handled = 0; }
                fs_volume_changed(0);
                return 1;
            }
            if (ok && fb_disk < 0) { fb_disk = disk; fb_lba = parts[k].lba; }
        }
    }
    for (int i = 0; i < BLK_MAX; i++) { blk_dev_t* d = blk_get(i); if (d && d->handled == 2) d->handled = 0; }

    // Ядра нигде нет (так бывает с тестовым образом) -- корнем становится
    // первый найденный FAT32, как и раньше
    if (fb_disk >= 0) {
        fs_lock();
        int ok = fat32_mount_vol(0, fb_disk, fb_lba);
        fs_unlock();
        if (ok) { remember_root(fb_disk, fb_lba); fs_volume_changed(0); return 1; }
    }
    fs_lock();
    fat32_unmount_vol(0);
    fs_unlock();
    return 0;
}

// Та же ли это флешка, что была корнем: VID:PID и размер у диска, а у тома на
// прежнем месте -- серийный номер (его пишет mkfs, у двух флешек он разный)
static int is_root_again(int disk) {
    blk_dev_t* d = blk_get(disk);
    if (!d || d->vid != s_root_vid || d->pid != s_root_pid || d->blocks != s_root_blocks) return 0;
    if (!blk_read(disk, s_root_lba, 1, s_sec) || fs_of_sector(s_sec) != FS_FAT32) return 0;
    uint32_t serial = s_sec[67] | (s_sec[68] << 8) | (s_sec[69] << 16) | ((uint32_t)s_sec[70] << 24);
    return serial == s_root_serial;
}

static void root_lost(void) {
    if (s_root_state != MOUNT_ROOT_OK) return;
    s_root_state = MOUNT_ROOT_LOST;
    fs_lock();
    fat32_detach_disk(0);                      // ни одно чтение до шины больше не дойдёт
    fs_unlock();
    s_mnt[0].disk = -1;

    knotify_t n;
    knotify_fill(&n, "dialog-warning", "Флешка с системой отключена",
                 "Вставьте её обратно — работа продолжится. Пока её нет, файлы не открываются и не сохраняются.");
    n.id = NOTE_ROOT;
    n.urgency = KNOTIFY_CRITICAL;
    knotify_post(&n);
    console_note("\n[usb] Флешка с системой отключена. Вставьте её обратно -- работа продолжится.\n", 0xFFFF55);
}

static int root_back(int disk) {
    fs_lock();
    int ok = fat32_mount_vol(0, disk, s_root_lba);
    fs_unlock();
    if (!ok) return 0;
    s_mnt[0].disk = disk;
    blk_get(disk)->handled = 1;
    s_root_state = MOUNT_ROOT_OK;
    fs_volume_changed(0);

    knotify_t n;
    knotify_fill(&n, "drive-removable", "Флешка с системой снова на месте",
                 "Корень перемонтирован, можно работать дальше. Что сохранялось в момент отключения, стоит проверить.");
    n.id = NOTE_ROOT;
    n.timeout_s = 8;
    knotify_post(&n);
    console_note("[usb] Флешка с системой снова на месте, корень перемонтирован.\n", 0x55FF55);
    return 1;
}

// ------------------------------------------------------- съёмные тома
static void attach_disk(int disk) {
    blk_dev_t* d = blk_get(disk);
    d->handled = 1;
    s_ejected[disk] = 0;

    char dev[40];
    blk_name(disk, dev, sizeof(dev));

    part_t parts[PART_MAX];
    int n = part_scan(disk, parts, PART_MAX);
    int mounted = 0, first_vol = -1, unsupported = FS_NONE;
    for (int k = 0; k < n; k++) {
        if (parts[k].fs != FS_FAT32) { if (parts[k].fs != FS_NONE) unsupported = parts[k].fs; continue; }
        int vol = -1;
        for (int v = 1; v < FAT32_MAX_VOLS; v++) if (!s_mnt[v].used) { vol = v; break; }
        if (vol < 0) break;
        fs_lock();
        int ok = fat32_mount_vol(vol, disk, parts[k].lba);
        if (ok) {
            s_mnt[vol].used = 1;
            s_mnt[vol].disk = disk;
            s_mnt[vol].lba = parts[k].lba;
            s_mnt[vol].serial = fat32_vol_serial(vol);
            fat32_vol_label(vol, s_mnt[vol].label, sizeof(s_mnt[vol].label));
            make_name(vol, s_mnt[vol].name, sizeof(s_mnt[vol].name));
        }
        fat32_select(0);
        fs_unlock();
        if (!ok) continue;
        fs_volume_changed(vol);
        if (first_vol < 0) first_vol = vol;
        mounted++;
    }

    knotify_t note;
    char body[200];
    if (mounted) {
        s_copy(body, "Устройство ", sizeof(body));
        s_cat(body, dev, sizeof(body));
        s_cat(body, " подключено.", sizeof(body));
        knotify_fill(&note, "drive-removable", "Обнаружено устройство USB", body);
        char cmd[64];
        s_copy(cmd, "open:" MOUNT_MEDIA "/", sizeof(cmd));
        s_cat(cmd, s_mnt[first_vol].name, sizeof(cmd));
        knotify_action(&note, 0, "Открыть", cmd);
        s_copy(cmd, "eject:", sizeof(cmd));
        s_cat(cmd, s_mnt[first_vol].name, sizeof(cmd));
        knotify_action(&note, 1, "Извлечь", cmd);
        console_note("[usb] Подключено: ", 0x55FF55);
        console_note(dev, 0xFFFFFF);
        console_note(" -> " MOUNT_MEDIA "/", 0xAAAAAA);
        console_note(s_mnt[first_vol].name, 0xFFFFFF);
        console_note("\n", 0);
    } else {
        s_copy(body, dev, sizeof(body));
        if (unsupported != FS_NONE) {
            s_cat(body, ": файловая система ", sizeof(body));
            s_cat(body, fs_name(unsupported), sizeof(body));
            s_cat(body, " пока не поддерживается.", sizeof(body));
        } else {
            s_cat(body, ": не найдено ни одного раздела FAT32.", sizeof(body));
        }
        knotify_fill(&note, "drive-removable", "Обнаружено устройство USB", body);
    }
    s_note_id[disk] = knotify_post(&note);
}

// Снять тома диска. ejected -- снимает пользователь ("Извлечь"), иначе диск
// уже выдернули.
static void detach_disk(int disk, int ejected) {
    char label[24] = "";
    int had = 0;
    fs_lock();
    for (int v = 1; v < FAT32_MAX_VOLS; v++) {
        if (!s_mnt[v].used || s_mnt[v].disk != disk) continue;
        if (!had) s_copy(label, s_mnt[v].label[0] ? s_mnt[v].label : s_mnt[v].name, sizeof(label));
        fat32_unmount_vol(v);
        s_mnt[v].used = 0;
        had = 1;
    }
    fat32_select(0);
    fs_unlock();
    for (int v = 1; v < FAT32_MAX_VOLS; v++) fs_volume_changed(v);
    if (!had) return;

    knotify_t n;
    char body[200];
    if (ejected) {
        s_copy(body, "«", sizeof(body));
        s_cat(body, label, sizeof(body));
        s_cat(body, "» отключено, флешку можно вынимать.", sizeof(body));
        knotify_fill(&n, "drive-removable", "Устройство можно извлечь", body);
        n.id = s_note_id[disk];               // вместо карточки "подключено"
    } else {
        s_copy(body, "«", sizeof(body));
        s_cat(body, label, sizeof(body));
        s_cat(body, "» вынули без извлечения. Если на неё что-то записывалось, проверьте файлы.", sizeof(body));
        knotify_fill(&n, "dialog-warning", "Устройство отключено", body);
        n.id = s_note_id[disk];
    }
    knotify_post(&n);
}

static int name_eq_ci(const char* a, const char* b) {
    while (*a && *b) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

int mount_eject(const char* name) {
    for (int v = 1; v < FAT32_MAX_VOLS; v++) {
        if (!s_mnt[v].used || !name_eq_ci(s_mnt[v].name, name)) continue;
        int disk = s_mnt[v].disk;
        s_ejected[disk] = 1;
        detach_disk(disk, 1);
        return 0;
    }
    if (name && (s_streq(name, "/") || (s_mnt[0].used && s_streq(name, s_mnt[0].label)))) return -2;
    return -1;
}

// ------------------------------------------------------- поток storaged
static void process_disks(void) {
    for (int i = 0; i < BLK_MAX; i++) {
        blk_dev_t* d = blk_get(i);
        if (!d) continue;
        if (d->gone) {
            if (s_mnt[0].used && s_mnt[0].disk == i) root_lost();
            else if (!s_ejected[i]) detach_disk(i, 0);
            else detach_disk(i, 1);           // уже снят -- тут ничего не останется
            s_ejected[i] = 0;
            s_note_id[i] = 0;
            blk_release(i);
            continue;
        }
        if (d->handled) continue;
        if (s_root_state == MOUNT_ROOT_LOST && is_root_again(i) && root_back(i)) continue;
        attach_disk(i);
    }
}

static void storaged_thread(void) {
    for (;;) {
        sched_sleep_ms(500);
        usb_poll();
        process_disks();
    }
}

void mount_start(void) {
    process_disks();                          // флешки, найденные при загрузке
    task_create_kernel(storaged_thread, "storaged");
}

// ------------------------------------------------------- пути
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int mount_route(const char* abs, const char** rest) {
    *rest = abs;
    const char* m = MOUNT_MEDIA "/";
    int i = 0;
    while (m[i] && abs[i] == m[i]) i++;
    if (m[i]) return 0;
    const char* nm = abs + i;
    int len = 0;
    while (nm[len] && nm[len] != '/') len++;
    if (!len) return 0;
    for (int v = 1; v < FAT32_MAX_VOLS; v++) {
        if (!s_mnt[v].used) continue;
        // Без учёта регистра, как и сам FAT: пути в системе приводятся к
        // нижнему регистру (perm_normalize), а имя тома -- "TESTSTICK"
        int k = 0;
        while (k < len && lc(s_mnt[v].name[k]) == lc(nm[k])) k++;
        if (k != len || s_mnt[v].name[len]) continue;
        *rest = nm[len] ? nm + len : "/";
        return v;
    }
    return 0;
}

int mount_media_entries(fs_file_info_t* out, int max) {
    int n = 0;
    for (int v = 1; v < FAT32_MAX_VOLS && n < max; v++) {
        if (!s_mnt[v].used) continue;
        fs_file_info_t* e = &out[n++];
        for (int k = 0; k < (int)sizeof(*e); k++) ((uint8_t*)e)[k] = 0;
        s_copy(e->name, s_mnt[v].name, sizeof(e->name));
        s_copy(e->clean_name, s_mnt[v].name, sizeof(e->clean_name));
        e->attr = 0x10;
    }
    return n;
}

int mount_list(mount_info_t* out, int max) {
    int n = 0;
    for (int v = 0; v < FAT32_MAX_VOLS && n < max; v++) {
        if (!s_mnt[v].used) continue;
        mount_info_t* m = &out[n++];
        for (int k = 0; k < (int)sizeof(*m); k++) ((uint8_t*)m)[k] = 0;
        s_copy(m->name, v ? s_mnt[v].name : "/", sizeof(m->name));
        s_copy(m->label, s_mnt[v].label, sizeof(m->label));
        if (s_mnt[v].disk >= 0) blk_name(s_mnt[v].disk, m->dev, sizeof(m->dev));
        if (v) { s_copy(m->path, MOUNT_MEDIA "/", sizeof(m->path)); s_cat(m->path, s_mnt[v].name, sizeof(m->path)); }
        else   s_copy(m->path, "/", sizeof(m->path));
        m->size_mb = s_mnt[v].disk >= 0 ? blk_size_mb(s_mnt[v].disk) : 0;
        m->flags = v ? MOUNT_F_REMOVABLE : MOUNT_F_ROOT;
    }
    return n;
}
