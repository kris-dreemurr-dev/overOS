#ifndef USB_H
#define USB_H
// usb.h — общий слой USB: то, что одинаково для любого контроллера.
//
//   usb.c         выбор контроллеров на старте, опрос портов (горячее
//                 подключение), замок шины
//   ehci.c        контроллер USB 2.0 и перечисление устройств на нём
//   ehci-msc.c    накопители (Bulk-Only Transport) поверх EHCI
//   xhci.c        контроллер USB 3.0 и перечисление устройств на нём
//   xhci-msc.c    накопители поверх xHCI
//
// Найденный накопитель драйвер регистрирует в реестре дисков (drivers/blk.c),
// дальше им занимается монтирование (fs/mount.c). Сам USB о файловых
// системах ничего не знает.

#include <stdint.h>

#define USB_DRV_NONE  0
#define USB_DRV_EHCI  1
#define USB_DRV_XHCI  2

// ---- стандартные запросы и дескрипторы (USB 2.0, глава 9) -------------------
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_CLEAR_FEATURE  0x01
#define USB_REQ_SET_FEATURE    0x03
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG     0x09

#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIG        0x02
#define USB_DESC_STRING        0x03
#define USB_DESC_INTERFACE     0x04
#define USB_DESC_ENDPOINT      0x05
#define USB_DESC_HUB           0x29

#define USB_CLASS_HID          0x03
#define USB_CLASS_STORAGE      0x08
#define USB_CLASS_HUB          0x09

// ---- накопители: Bulk-Only Transport и SCSI ---------------------------------
#define CBW_SIGNATURE 0x43425355 // "USBC"
#define CSW_SIGNATURE 0x53425355 // "USBS"
#define CBW_FLAGS_IN  0x80
#define CBW_FLAGS_OUT 0x00

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_transfer_len;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cmd_len;
    uint8_t  cmd[16];
} __attribute__((packed)) usb_cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t  status;
} __attribute__((packed)) usb_csw_t;

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY   0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

// Строки производителя и модели из ответа INQUIRY (8 и 16 символов)
void usb_inquiry_strings(const uint8_t* inq, char* vendor, char* product);

// ---- общий слой ---------------------------------------------------------------
// Старт: поднять все контроллеры, какие есть (xHCI и EHCI), и перечислить
// устройства. Возвращает тот, что ведёт шину (USB_DRV_*).
int  usb_bootstrap(void);
int  usb_active_driver(void);

// Горячее подключение: заглянуть в порты всех контроллеров, поднять новые
// устройства и отметить пропавшие. Зовёт поток storaged (fs/mount.c).
void usb_poll(void);

// Один обмен по шине за раз. Опрос портов идёт из своего потока, а чтение
// секторов -- из чужих сисколлов; у обоих контроллеров одно кольцо событий
// (xHCI) и одно асинхронное расписание (EHCI), и два одновременных обмена
// забирали бы ответы друг друга. Берут его только входные точки драйверов.
void usb_lock(void);
void usb_unlock(void);

// Подробный вывод драйверов на консоль (команда usb-debug)
void usb_set_verbose(int v);
int  usb_verbose(void);

#endif
