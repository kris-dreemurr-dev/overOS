#include "pci.h"

extern void outb(uint16_t port, uint8_t val);
extern void kputs(const char* str, uint32_t color);
extern void kdebug(const char* str, uint32_t color);
extern void kputc(char c, uint32_t color);
extern void itoa(int n, char* str);

void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1U << 31) | 
                       ((uint32_t)bus << 16) | 
                       ((uint32_t)slot << 11) | 
                       ((uint32_t)func << 8) | 
                       (offset & 0xFC);
                       
    outl(0xCF8, address);
    return inl(0xCFC);
}

void pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (1U << 31) | 
                       ((uint32_t)bus << 16) | 
                       ((uint32_t)slot << 11) | 
                       ((uint32_t)func << 8) | 
                       (offset & 0xFC);
                       
    outl(0xCF8, address);
    outl(0xCFC, val);
}

static void print_num(int val, uint32_t color) {
    char buf[16];
    itoa(val, buf);
    kdebug(buf, color);
}

static void print_hex(uint32_t val, uint32_t color) {
    char hex_str[11] = {'0', 'x'};
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        hex_str[2 + i] = hex_chars[val & 0xF];
        val >>= 4;
    }
    hex_str[10] = '\0';
    kdebug(hex_str, color);
}

void pci_scan_all(void) {
    kdebug("\n=== Scanning PCI Bus (Audio & USB) ===\n", 0x55FFFF);

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_config_read(bus, slot, func, 0x00);
                uint16_t vendor_id = vendor_device & 0xFFFF;
                
                if (vendor_id == 0xFFFF) continue;

                uint32_t class_rev = pci_config_read(bus, slot, func, 0x08);
                uint8_t base_class = (class_rev >> 24) & 0xFF;
                uint8_t subclass   = (class_rev >> 16) & 0xFF;
                uint8_t prog_if    = (class_rev >> 8)  & 0xFF;

                uint32_t bar0 = pci_config_read(bus, slot, func, 0x10);

                if (base_class == 0x0C && subclass == 0x03) {
                    kdebug("[PCI] Found USB Controller:\n", 0x55FF55);
                    kdebug("  Bus: ", 0xAAAAAA); print_num(bus, 0xFFFFFF);
                    kdebug(" | Slot: ", 0xAAAAAA); print_num(slot, 0xFFFFFF);
                    kdebug(" | Func: ", 0xAAAAAA); print_num(func, 0xFFFFFF);
                    
                    kdebug("\n  Type: ", 0xAAAAAA);
                    if (prog_if == 0x30)      kdebug("xHCI (USB 3.0)\n", 0xFFFF55);
                    else if (prog_if == 0x20) kdebug("EHCI (USB 2.0)\n", 0xFFFF55);
                    else if (prog_if == 0x10) kdebug("OHCI\n", 0xAAAAAA);
                    else if (prog_if == 0x00) kdebug("UHCI\n", 0xAAAAAA);
                    else                      kdebug("Unknown USB\n", 0xFF5555);

                    kdebug("  MMIO BAR0: ", 0xAAAAAA);
                    print_hex(bar0 & ~0xF, 0x55FF55);
                    kdebug("\n\n", 0);
                } else if (base_class == 0x04) {
                    kdebug("[PCI] Found Audio Controller:\n", 0x55FF55);
                    kdebug("  Bus: ", 0xAAAAAA); print_num(bus, 0xFFFFFF);
                    kdebug(" | Slot: ", 0xAAAAAA); print_num(slot, 0xFFFFFF);
                    kdebug(" | Func: ", 0xAAAAAA); print_num(func, 0xFFFFFF);

                    if (subclass == 0x03) {
                        kdebug("\n  Type: Intel HDA (High Definition Audio)\n", 0x55FFFF);
                    } else {
                        kdebug("\n  Type: Generic Multimedia Audio\n", 0x55FFFF);
                    }
                    kdebug("  BAR0: ", 0xAAAAAA);
                    print_hex(bar0 & ~0xF, 0x55FF55);
                    kdebug("\n\n", 0);
                } else if (base_class == 0x02) {
                    kdebug("[PCI] Found Network Controller:\n", 0x55FF55);
                    kdebug("  Bus: ", 0xAAAAAA); print_num(bus, 0xFFFFFF);
                    kdebug(" | Slot: ", 0xAAAAAA); print_num(slot, 0xFFFFFF);
                    kdebug(" | Func: ", 0xAAAAAA); print_num(func, 0xFFFFFF);

                    kdebug("\n  Category: ", 0xAAAAAA);
                    if (subclass == 0x00) {
                        kdebug("Ethernet Controller (Wired LAN)\n", 0x55FFFF);
                    } else if (subclass == 0x80) {
                        kdebug("Wireless / Network Controller (Wi-Fi)\n", 0xFFFF55);
                    } else {
                        kdebug("Other Network Device (Subclass ", 0xAAAAAA);
                        print_hex(subclass, 0xFFFFFF);
                        kdebug(")\n", 0);
                    }

                    // Выводим Vendor ID и Device ID
                    kdebug("  Hardware ID: [", 0xAAAAAA);
                    print_hex(vendor_id, 0x55FF55);
                    kdebug(":", 0xAAAAAA);
                    print_hex((vendor_device >> 16) & 0xFFFF, 0x55FF55);
                    kdebug("] ", 0);

                    // Расшифровка ключевых сетевых вендоров
                    if (vendor_id == 0x8086)      kdebug("(Intel Corporation)\n", 0x55FFFF);
                    else if (vendor_id == 0x10EC) kdebug("(Realtek Semiconductor)\n", 0x55FFFF);
                    else if (vendor_id == 0x14E4) kdebug("(Broadcom)\n", 0x55FFFF);
                    else if (vendor_id == 0x168C) kdebug("(Qualcomm Atheros)\n", 0x55FFFF);
                    else if (vendor_id == 0x1969) kdebug("(Atheros / Killer LAN)\n", 0x55FFFF);
                    else                          kdebug("(Unknown Vendor)\n", 0xAAAAAA);

                    // Проверяем тип BAR0: бит 0 = 1 -> Port I/O, бит 0 = 0 -> MMIO
                    kdebug("  BAR0: ", 0xAAAAAA);
                    if (bar0 & 1) {
                        kdebug("I/O Port ", 0xFFFF55);
                        print_hex(bar0 & ~0x3, 0xFFFFFF);
                    } else {
                        kdebug("MMIO ", 0x55FF55);
                        print_hex(bar0 & ~0xF, 0xFFFFFF);
                    }

                    // Читаем IRQ из PCI Config Space (смещение 0x3C: байт 0 — Interrupt Line)
                    uint32_t intr = pci_config_read(bus, slot, func, 0x3C);
                    uint8_t irq_line = intr & 0xFF;
                    kdebug(" | IRQ: ", 0xAAAAAA);
                    print_num(irq_line, 0xFFFFFF);
                    kdebug("\n\n", 0);
                }
            }
        }
    }
}

int pci_find_usb_controller(pci_device_t* out_dev) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_config_read(bus, slot, func, 0x00);
                if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_rev = pci_config_read(bus, slot, func, 0x08);
                uint8_t base_class = (class_rev >> 24) & 0xFF;
                uint8_t subclass   = (class_rev >> 16) & 0xFF;

                if (base_class == 0x0C && subclass == 0x03) {
                    out_dev->bus = bus;
                    out_dev->slot = slot;
                    out_dev->func = func;
                    out_dev->base_class = base_class;
                    out_dev->subclass = subclass;
                    out_dev->prog_if = (class_rev >> 8) & 0xFF;
                    out_dev->vendor_id = vendor_device & 0xFFFF;
                    out_dev->device_id = (vendor_device >> 16) & 0xFFFF;
                    out_dev->bar0 = pci_config_read(bus, slot, func, 0x10) & ~0xF;
                    return 1;
                }
            }
        }
    }
    return 0;
}

void cmd_fastfetch_audio(void) {
    pci_scan_all();
}

const char* pci_get_audio_controller_name(void) {
    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t vendor_device = pci_config_read(bus, slot, 0, 0);
            if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

            uint32_t class_rev = pci_config_read(bus, slot, 0, 8);
            if (((class_rev >> 24) & 0xFF) == 0x04) {
                uint8_t subclass = (class_rev >> 16) & 0xFF;
                if (subclass == 0x03) {
                    return "Intel HDA Audio";
                }
                return "Generic Audio Device";
            }
        }
    }
    return "Not Detected";
}