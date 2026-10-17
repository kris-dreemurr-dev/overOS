// =============================================================================
// blk.c — реестр блочных устройств.
//
// Раньше файловая система звала драйвер флешки напрямую, а "какой из двух
// контроллеров нашёл флешку" решал промежуточный usbblk.c, который знал всё и
// обо всех: о EHCI, о xHCI и о сторожевом потоке. Флешка при этом была ровно
// одна. Теперь слоёв три и каждый знает только соседа снизу:
//
//   ehci-msc.c / xhci-msc.c   нашли накопитель → blk_add(), пропал → blk_gone()
//   blk.c (этот файл)         номер диска → его драйвер; больше ничего
//   fs/mount.c, fat32.c       тома и точки монтирования поверх номеров дисков
//
// Здесь нет ни монтирования, ни политики: только таблица и диспетчеризация.
// =============================================================================

#include "blk.h"

static blk_dev_t s_blk[BLK_MAX];
static uint32_t  s_seq = 0;

uint64_t g_blk_rd_sectors = 0, g_blk_wr_sectors = 0;

static void copy_trim(char* dst, const char* src, int max) {
    int n = 0;
    if (src) while (src[n] && n < max - 1) { dst[n] = src[n]; n++; }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

int blk_add(const blk_ops_t* ops, int h, int bus, uint16_t vid, uint16_t pid,
            uint32_t blocks, uint32_t block_size, const char* vendor, const char* product) {
    for (int i = 0; i < BLK_MAX; i++) {
        blk_dev_t* d = &s_blk[i];
        if (d->used) continue;
        for (int k = 0; k < (int)sizeof(*d); k++) ((uint8_t*)d)[k] = 0;
        d->ops = ops; d->h = h; d->bus = bus;
        d->vid = vid; d->pid = pid;
        d->blocks = blocks;
        d->block_size = block_size ? block_size : 512;
        copy_trim(d->vendor, vendor, sizeof(d->vendor));
        copy_trim(d->product, product, sizeof(d->product));
        d->seq = ++s_seq;
        d->used = 1;
        return i;
    }
    return -1;
}

// Устройство пропало. Запись остаётся, пока монтирование не разберётся:
// снимет тома с этого диска и отпустит его (blk_release). Иначе номер диска
// успел бы достаться следующей флешке, а том ещё смотрел бы в него.
void blk_gone(int disk) {
    if (disk < 0 || disk >= BLK_MAX || !s_blk[disk].used) return;
    s_blk[disk].gone = 1;
}

void blk_release(int disk) {
    if (disk < 0 || disk >= BLK_MAX || !s_blk[disk].used || !s_blk[disk].gone) return;
    s_blk[disk].used = 0;
}

blk_dev_t* blk_get(int disk) {
    if (disk < 0 || disk >= BLK_MAX || !s_blk[disk].used) return 0;
    return &s_blk[disk];
}

int blk_alive(int disk) {
    blk_dev_t* d = blk_get(disk);
    return d && !d->gone;
}

uint32_t blk_size_mb(int disk) {
    blk_dev_t* d = blk_get(disk);
    if (!d) return 0;
    return (uint32_t)(((uint64_t)d->blocks * d->block_size) / (1024 * 1024));
}

void blk_name(int disk, char* out, int max) {
    blk_dev_t* d = blk_get(disk);
    int p = 0;
    out[0] = '\0';
    if (!d || max < 2) return;
    // Многие флешки пишут производителя и в модели ("ADATA" + "ADATA USB Flash
    // Drive" -- так и выглядит уведомление KDE). Повтор не дублируем.
    int dup = 1;
    for (int i = 0; d->vendor[i]; i++) if (d->vendor[i] != d->product[i]) { dup = 0; break; }
    if (!dup || !d->vendor[0]) {
        for (int i = 0; d->vendor[i] && p < max - 1; i++) out[p++] = d->vendor[i];
        if (p && d->product[0] && p < max - 1) out[p++] = ' ';
    }
    for (int i = 0; d->product[i] && p < max - 1; i++) out[p++] = d->product[i];
    if (!p) {
        const char* s = "USB-накопитель";
        while (*s && p < max - 1) out[p++] = *s++;
    }
    out[p] = '\0';
}

int blk_read(int disk, uint32_t lba, uint32_t count, void* out) {
    blk_dev_t* d = blk_get(disk);
    if (!d || d->gone || !count || !out) return 0;
    if (d->ops->read(d->h, lba, count, out) == 0) {
        g_blk_rd_sectors += count;
        return 1;
    }
    if (!d->ops->present(d->h)) d->gone = 1;
    return 0;
}

int blk_write(int disk, uint32_t lba, uint32_t count, const void* in) {
    blk_dev_t* d = blk_get(disk);
    if (!d || d->gone || !count || !in) return 0;
    if (d->ops->write(d->h, lba, count, in) == 0) {
        g_blk_wr_sectors += count;
        return 1;
    }
    if (!d->ops->present(d->h)) d->gone = 1;
    return 0;
}
