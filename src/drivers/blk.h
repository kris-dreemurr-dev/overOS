#ifndef BLK_H
#define BLK_H
// blk.h — реестр блочных устройств (дисков). См. blk.c.
//
// Драйвер шины (EHCI, xHCI) нашёл накопитель — регистрирует его здесь и
// получает номер диска. Файловая система и монтирование работают только с
// этим номером и ничего не знают о контроллерах: "диск 1, прочитай сектор".

#include <stdint.h>

#define BLK_MAX 8

#define BLK_BUS_EHCI 1
#define BLK_BUS_XHCI 2

// Что драйвер умеет с накопителем. h — его собственный номер устройства.
// read/write: 0 — успех, <0 — ошибка. present: 1 — на месте (дёшево, по
// возможности без обмена по шине).
typedef struct {
    int (*read)(int h, uint32_t lba, uint32_t count, void* out);
    int (*write)(int h, uint32_t lba, uint32_t count, const void* in);
    int (*present)(int h);
} blk_ops_t;

typedef struct {
    int      used;
    int      gone;              // драйвер сообщил: устройство выдернули
    int      bus;               // BLK_BUS_*
    const blk_ops_t* ops;
    int      h;
    uint16_t vid, pid;
    uint32_t blocks, block_size;
    char     vendor[12];        // из SCSI INQUIRY, без хвостовых пробелов
    char     product[20];
    uint32_t seq;               // порядковый номер регистрации (новый диск — новый номер)
    int      handled;           // монтирование уже разобралось с этим диском
} blk_dev_t;

int        blk_add(const blk_ops_t* ops, int h, int bus, uint16_t vid, uint16_t pid,
                   uint32_t blocks, uint32_t block_size, const char* vendor, const char* product);
void       blk_gone(int disk);          // драйвер: устройства больше нет
void       blk_release(int disk);       // монтирование отпустило пропавший диск
blk_dev_t* blk_get(int disk);           // 0 — нет такого
int        blk_alive(int disk);
uint32_t   blk_size_mb(int disk);
// "ADATA USB Flash Drive": производитель и модель одной строкой
void       blk_name(int disk, char* out, int max);

// 1 — успех, 0 — отказ (так же, как раньше отвечал ehci_msc_read_sectors)
int        blk_read(int disk, uint32_t lba, uint32_t count, void* out);
int        blk_write(int disk, uint32_t lba, uint32_t count, const void* in);

// Сектора, прочитанные и записанные с момента загрузки (диспетчер задач)
extern uint64_t g_blk_rd_sectors, g_blk_wr_sectors;

#endif
