// nv_probe.sys — Поиск и пробуждение GPU NVIDIA на шине PCI
#include "../drivers/api.h"
#include <stdint.h>

int driver_entry(const devos_api_t* api);

__attribute__((section(".header")))
const sys_header_t nv_sys_header = {
    .magic = { SYS_MAGIC_0, SYS_MAGIC_1, SYS_MAGIC_2, SYS_MAGIC_3 },
    .entry_point = driver_entry,
    .required_api_ver = 1,
    .driver_name = "NV_PROBE_SYS",
    .flags = 0
};

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1U << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC));
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((1U << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC));
    outl(0xCF8, address);
    outl(0xCFC, val);
}

static void print_hex32(const devos_api_t* api, uint32_t val, uint32_t col) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) api->kputc(hex[(val >> (i * 4)) & 0x0F], col);
}

int driver_entry(const devos_api_t* api) {
    api->kputs("\nScanning PCI bus for NVIDIA dGPU...\n", 0x55FFFF);
    api->flush_buffer();

    int found = 0;
    for (uint16_t bus = 0; bus < 16; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t dev_ven = pci_read(bus, slot, 0, 0x00);
            uint16_t ven = (uint16_t)(dev_ven & 0xFFFF);
            uint16_t dev = (uint16_t)(dev_ven >> 16);

            if (ven == 0x10DE) { // NVIDIA Vendor ID
                found = 1;
                api->kputs("[+] Found NVIDIA GPU: Device ID = 0x", 0x55FF55);
                print_hex32(api, dev, 0xFFFF55);
                api->kputs(" at Bus: ", 0x55FF55);
                print_hex32(api, bus, 0xFFFF55);
                api->kputs("\n", 0xFFFFFF);

                // 1. Пробуждаем шину: включаем Memory Space (bit 1) и Bus Master (bit 2)
                uint32_t cmd = pci_read(bus, slot, 0, 0x04);
                cmd |= (1 << 1) | (1 << 2);
                pci_write(bus, slot, 0, 0x04, cmd);

                // 2. Читаем физический адрес MMIO (BAR0)
                uint32_t bar0 = pci_read(bus, slot, 0, 0x10) & ~0x0F;
                api->kputs("    BAR0 MMIO Physical Address: 0x", 0xAAAAAA);
                print_hex32(api, bar0, 0xFFFF55);
                api->kputs("\n", 0xFFFFFF);

                if (bar0 != 0) {
                    // 3. Читаем регистр идентификации чипа (NV_PMC_BOOT_0)
                    volatile uint32_t* nv_mmio = (volatile uint32_t*)(uintptr_t)bar0;
                    uint32_t pmc_boot = nv_mmio[0]; // смещение 0x00000000

                    api->kputs("    NV_PMC_BOOT_0 Register: 0x", 0x55FF55);
                    print_hex32(api, pmc_boot, 0xFFFF55);
                    api->kputs("\n", 0xFFFFFF);

                    // Расшифровка архитектуры чипа
                    uint32_t chip = (pmc_boot >> 20) & 0x1FF;
                    api->kputs("    GPU Architecture Code: 0x", 0x55FFFF);
                    print_hex32(api, chip, 0xFFFF55);
                    api->kputs(" (Active & Responding!)\n", 0x55FF55);
                }
                api->flush_buffer();
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        api->kputs("[-] NVIDIA GPU not found on PCI. It might be powered off by ACPI (D3cold).\n", 0xFF5555);
        api->flush_buffer();
    }
    return 0;
}