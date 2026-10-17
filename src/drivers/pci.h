#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t base_class;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0;
} pci_device_t;

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

void pci_scan_all(void);
// Сколько устройств на шине (отчёт загрузчика)
int  pci_count_devices(void);
// Контроллер USB по типу: 0x30 xHCI, 0x20 EHCI, 0x10 OHCI, 0x00 UHCI
int  pci_find_usb_by_progif(uint8_t prog_if, pci_device_t* out);
int pci_find_usb_controller(pci_device_t* out_dev);

// --- НОВЫЕ ФУНКЦИИ ДЛЯ HDA И ДРУГИХ ДРАЙВЕРОВ ---
int  pci_find_by_class(uint8_t base_class, uint8_t subclass, pci_device_t* out_dev);
void pci_enable_bus_master(const pci_device_t* dev);

// Список устройств на шине, как `lspci -nn` в Linux
void cmd_lspci(void);

void cmd_fastfetch_audio(void);
const char* pci_get_audio_controller_name(void);

#endif