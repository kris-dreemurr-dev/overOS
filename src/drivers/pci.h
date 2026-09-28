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
int pci_find_usb_controller(pci_device_t* out_dev);

void cmd_fastfetch_audio(void);
const char* pci_get_audio_controller_name(void);

#endif