#ifndef XHCI_PRIV_H
#define XHCI_PRIV_H
// xhci-priv.h — внутренности драйвера xHCI, общие для xhci.c (контроллер,
// кольца, перечисление устройств) и xhci-msc.c (накопители). Снаружи драйвера
// этот файл не включают: остальная система видит только xhci.h.

#include <stdint.h>

// ------------------------------------------------------------------ регистры
// Capability (только чтение, смещения от BAR0)
#define XCAP_CAPLENGTH   0x00   // 8 бит: где начинаются операционные регистры
#define XCAP_HCIVERSION  0x02
#define XCAP_HCSPARAMS1  0x04   // слоты, прерывания, порты
#define XCAP_HCSPARAMS2  0x08   // scratchpad
#define XCAP_HCCPARAMS1  0x10   // размер контекста, адрес расширенных возможностей
#define XCAP_DBOFF       0x14   // смещение дверных звонков
#define XCAP_RTSOFF      0x18   // смещение регистров времени выполнения

// Operational (смещения от BAR0 + CAPLENGTH)
#define XOP_USBCMD       0x00
#define XOP_USBSTS       0x04
#define XOP_PAGESIZE     0x08
#define XOP_DNCTRL       0x14
#define XOP_CRCR         0x18   // 64 бита: кольцо команд
#define XOP_DCBAAP       0x30   // 64 бита: массив контекстов устройств
#define XOP_CONFIG       0x38
#define XOP_PORTSC(n)    (0x400 + (n) * 0x10)

#define USBCMD_RS        (1u << 0)   // Run/Stop
#define USBCMD_HCRST     (1u << 1)   // сброс контроллера
#define USBCMD_INTE      (1u << 2)
#define USBSTS_HCH       (1u << 0)   // остановлен
#define USBSTS_CNR       (1u << 11)  // Controller Not Ready

#define PORTSC_CCS       (1u << 0)   // устройство подключено
#define PORTSC_PED       (1u << 1)   // порт включён
#define PORTSC_PR        (1u << 4)   // сброс порта
#define PORTSC_PP        (1u << 9)   // питание порта
#define PORTSC_CSC       (1u << 17)  // изменение подключения
#define PORTSC_PRC       (1u << 21)  // сброс завершён
// Биты, которые сбрасываются записью единицы: трогать их случайно нельзя
#define PORTSC_RW1CS     (0x00FE0000u)
#define PORTSC_LWS       (1u << 16)  // разрешение записи PLS
#define PORTSC_WPR       (1u << 31)  // тёплый сброс (USB 3)

// "Нейтральное" значение PORTSC для записи обратно: всё прочитанное, кроме
// битов, у которых запись единицы что-то ДЕЛАЕТ. Главный из них -- PED (бит 1):
// запись единицы туда ВЫКЛЮЧАЕТ порт. Раньше маска его не снимала, и любая
// запись "снять отметку" в порт, который USB 3 уже включил сам, тут же этот
// порт выключала -- флешку мы видели подключённой, но говорить с ней не могли.
static inline uint32_t portsc_neutral(uint32_t sc) {
    return sc & ~(PORTSC_PED | PORTSC_PR | PORTSC_LWS | PORTSC_RW1CS | PORTSC_WPR);
}

// Runtime (смещения от BAR0 + RTSOFF), прерыватель 0
#define XRT_IMAN(i)      (0x20 + (i) * 32 + 0x00)
#define XRT_IMOD(i)      (0x20 + (i) * 32 + 0x04)
#define XRT_ERSTSZ(i)    (0x20 + (i) * 32 + 0x08)
#define XRT_ERSTBA(i)    (0x20 + (i) * 32 + 0x10)
#define XRT_ERDP(i)      (0x20 + (i) * 32 + 0x18)

// ------------------------------------------------------------------ TRB
#define TRB_RING_SIZE    256                    // TRB по 16 байт -> 4 КБ на кольцо
#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_LINK         6
#define TRB_ENABLE_SLOT  9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EVAL_CTX     13
#define TRB_RESET_EP     14
#define TRB_SET_TR_DEQ   16
#define TRB_CMD_NOOP     23
#define TRB_EV_TRANSFER  32
#define TRB_EV_CMD_DONE  33
#define TRB_EV_PORT      34

typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

typedef struct {
    uint64_t base;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed)) xhci_erst_t;

// ------------------------------------------------------- таблица устройств
#define XDEV_MAX 8
#define XHID_MAX 3          // HID-интерфейсов на устройство (клавиатура + мышь в одном приёмнике)
#define XHID_QUEUED 8       // столько передач interrupt IN держим в очереди на точку

typedef struct {
    uint8_t  iface, proto;  // номер интерфейса; протокол 1 — клавиатура, 2 — мышь
    uint8_t  ep, interval;  // номер точки interrupt IN и bInterval из дескриптора
    uint16_t mps;
    int      dci;
    int      kb;            // номер клавиатуры в hid.c
    volatile int active;    // таймер обслуживает эту точку
    xhci_trb_t* ring; uint64_t ring_phys; uint32_t idx, cycle;
    uint8_t* buf; uint64_t buf_phys;   // по 64 байта на TRB (номер TRB % 64)
} xhci_hid_t;

typedef struct {
    int      used;
    int      slot;          // номер слота у контроллера
    int      port;          // корневой порт (с нуля)
    int      speed;         // PSIV из PORTSC
    uint16_t vid, pid;
    uint8_t  cls, sub, proto;   // класс интерфейса (8/6/80 = флешка)
    uint8_t  ep_in, ep_out;     // номера bulk-точек интерфейса-накопителя
    char     vendor[9];         // SCSI INQUIRY
    char     product[17];
    int      disk;              // номер в реестре дисков (drivers/blk.c), -1 -- не диск

    xhci_trb_t* ep0_ring;  uint64_t ep0_phys;  uint32_t ep0_idx, ep0_cycle;
    xhci_trb_t* in_ring;   uint64_t in_ring_phys;  uint32_t in_idx,  in_cycle;
    xhci_trb_t* out_ring;  uint64_t out_ring_phys; uint32_t out_idx, out_cycle;
    uint32_t    blocks, block_size;   // SCSI READ CAPACITY
    uint32_t    tag;
    uint8_t     cfg_value;            // bConfigurationValue для SET_CONFIGURATION
    int         gone;                 // устройство выдернули: на шину больше не ходим
    uint64_t in_ctx_phys;  uint8_t* in_ctx;
    uint64_t dev_ctx_phys; uint8_t* dev_ctx;
    xhci_hid_t hid[XHID_MAX];          // загрузочные HID-интерфейсы (xhci-hid.c)
    int        hid_n;
} xhci_dev_t;

extern xhci_dev_t xhci_devs[XDEV_MAX];
extern int        xhci_ctx64;          // контекст 64 байта вместо 32
extern int        xhci_last_cc;        // код завершения последней передачи

#define XHCI_CTXSZ (xhci_ctx64 ? 64 : 32)
static inline uint32_t* xhci_ctx_at(uint8_t* base, int index) {
    return (uint32_t*)(base + (uintptr_t)index * XHCI_CTXSZ);
}

// ------------------------------------------------------- службы xhci.c
void* xhci_dma_alloc(uint32_t pages, uint64_t* out_phys);
void  xhci_dma_free(void* virt, uint32_t pages);
void  xhci_cmd_submit(uint64_t param, uint32_t status, uint32_t control);
int   xhci_wait_event(int type, xhci_trb_t* out, uint32_t ms);
int   xhci_wait_transfer(int slot, int dci, uint64_t trb_phys, xhci_trb_t* out, uint32_t ms);
void  xhci_ring_push(xhci_trb_t* ring, uint64_t phys, uint32_t* idx, uint32_t* cycle,
                     uint64_t param, uint32_t status, uint32_t control);
void  xhci_doorbell(int slot, uint32_t target);
int   xhci_control_nodata(xhci_dev_t* d, uint8_t rtype, uint8_t req, uint16_t value, uint16_t index);
int   xhci_dev_present(xhci_dev_t* d);
void  xhci_dev_gone(xhci_dev_t* d);
// Почему устройство не поднялось: шаг и код (печатается в строке порта)
void  xhci_fail(const char* step, int code);

void  xhci_log(const char* s, uint32_t c);
void  xhci_log_num(const char* pre, int v, const char* post);
void  xhci_err(const char* s);
void  xhci_err_hex(const char* pre, uint64_t v, const char* post);

// Второе кольцо событий (прерыватель 1): только передачи HID. Его разбирает
// таймер, поэтому ожидания накопителя на кольце 0 эти события не съедят
int   xhci_evt2_ready(void);
int   xhci_evt2_pop(xhci_trb_t* out);
void  xhci_evt2_done(void);

// ------------------------------------------------------- xhci-hid.c
int   xhci_hid_attach(xhci_dev_t* d);   // 0 — хоть одна точка заработала
void  xhci_hid_detach(xhci_dev_t* d);   // до освобождения колец устройства

// ------------------------------------------------------- xhci-msc.c
// Довести накопитель до рабочего состояния и зарегистрировать как диск.
// buf -- страница DMA для обмена на время настройки. 0 -- успех.
int   xhci_msc_attach(xhci_dev_t* d, uint8_t* buf, uint64_t buf_phys);

#endif
