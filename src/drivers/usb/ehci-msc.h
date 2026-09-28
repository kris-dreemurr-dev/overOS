#ifndef EHCI_MSC_H
#define EHCI_MSC_H

#include <stdint.h>

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

typedef struct {
    uint8_t  dev_addr;
    uint8_t  ep_in;
    uint8_t  ep_out;
    uint16_t max_packet_in;
    uint16_t max_packet_out;
    uint32_t total_blocks;
    uint32_t block_size;
} ehci_msc_device_t;

int  ehci_msc_init_device(uint8_t dev_addr, const uint8_t* cfg_desc, uint16_t cfg_len);
int  ehci_msc_read_sectors(uint32_t lba, uint16_t count, void* out_buffer);
int  ehci_msc_write_sectors(uint32_t lba, uint16_t count, const void* in_buffer);
ehci_msc_device_t* ehci_msc_get_device(void);

#endif