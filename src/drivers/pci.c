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

// Сколько устройств реально отвечает на шине (для отчёта загрузчика)
int pci_count_devices(void) {
    int n = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vd = pci_config_read((uint8_t)bus, slot, func, 0x00);
                if ((vd & 0xFFFF) == 0xFFFF) continue;
                n++;
                if (func == 0) {
                    uint32_t hdr = pci_config_read((uint8_t)bus, slot, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;      // не многофункциональное
                }
            }
        }
    }
    return n;
}

// Контроллер USB заданного типа: 0x20 — EHCI, 0x30 — xHCI, 0x10 — OHCI, 0x00 — UHCI.
// Нужен для выбора драйвера: при наличии и xHCI, и EHCI берём первый.
int pci_find_usb_by_progif(uint8_t prog_if, pci_device_t* out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vd = pci_config_read((uint8_t)bus, slot, func, 0x00);
                if ((vd & 0xFFFF) == 0xFFFF) continue;
                uint32_t cr = pci_config_read((uint8_t)bus, slot, func, 0x08);
                if (((cr >> 24) & 0xFF) != 0x0C || ((cr >> 16) & 0xFF) != 0x03) continue;
                if (((cr >> 8) & 0xFF) != prog_if) continue;
                if (out) {
                    out->bus = (uint8_t)bus; out->slot = slot; out->func = func;
                    out->base_class = 0x0C; out->subclass = 0x03; out->prog_if = prog_if;
                    out->vendor_id = (uint16_t)(vd & 0xFFFF);
                    out->device_id = (uint16_t)(vd >> 16);
                    out->bar0 = pci_config_read((uint8_t)bus, slot, func, 0x10);
                }
                return 1;
            }
        }
    }
    return 0;
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

// Поиск любого PCI-устройства по паре Base Class / Subclass
// Возвращает 0 при успехе, -1 если устройство не найдено
int pci_find_by_class(uint8_t base_class, uint8_t subclass, pci_device_t* out_dev) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_config_read(bus, slot, func, 0x00);
                if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_rev = pci_config_read(bus, slot, func, 0x08);
                uint8_t b_class = (class_rev >> 24) & 0xFF;
                uint8_t s_class = (class_rev >> 16) & 0xFF;

                if (b_class == base_class && s_class == subclass) {
                    if (out_dev) {
                        out_dev->bus        = bus;
                        out_dev->slot       = slot;
                        out_dev->func       = func;
                        out_dev->base_class = b_class;
                        out_dev->subclass   = s_class;
                        out_dev->prog_if    = (class_rev >> 8) & 0xFF;
                        out_dev->vendor_id  = vendor_device & 0xFFFF;
                        out_dev->device_id  = (vendor_device >> 16) & 0xFFFF;
                        out_dev->bar0       = pci_config_read(bus, slot, func, 0x10) & ~0xF;
                    }
                    return 0; // Нашли!
                }
            }
        }
    }
    return -1; // Не найдено
}

// Включение прямого доступа к памяти (Bus Master) и адресного пространства MMIO
void pci_enable_bus_master(const pci_device_t* dev) {
    // Регистр Command находится по смещению 0x04
    uint32_t cmd = pci_config_read(dev->bus, dev->slot, dev->func, 0x04);
    
    // Бит 1 = Memory Space (включает MMIO BAR0)
    // Бит 2 = Bus Master (разрешает контроллеру инициировать DMA-передачу)
    cmd |= (1U << 1) | (1U << 2);
    
    pci_config_write(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
// =============================================================================
// lspci — что стоит на шине. Нужна была для Wi-Fi: пока мы не знаем, какой
// именно чип в ноутбуке, писать драйвер не для чего, а база имён устройств
// (у Linux это pci.ids на мегабайт) нам не по карману. Поэтому печатаем то же,
// что показывает `lspci -nn`: адрес, класс и пару vendor:device, плюс
// идентификатор подсистемы для сетевых карт — у Intel именно он отличает
// конкретный модуль (AX200 от AX201 и так далее).
// =============================================================================
static const struct { uint16_t id; const char* name; } s_pci_vendors[] = {
    { 0x8086, "Intel" },        { 0x8087, "Intel" },       { 0x10EC, "Realtek" },
    { 0x14E4, "Broadcom" },     { 0x168C, "Qualcomm Atheros" },
    { 0x17CB, "Qualcomm" },     { 0x1969, "Atheros" },     { 0x14C3, "MediaTek" },
    { 0x1814, "Ralink/MediaTek" }, { 0x10DE, "NVIDIA" },   { 0x1002, "AMD/ATI" },
    { 0x1022, "AMD" },          { 0x1B21, "ASMedia" },     { 0x11AB, "Marvell" },
    { 0x197B, "JMicron" },      { 0x1106, "VIA" },         { 0x144D, "Samsung" },
    { 0x15B7, "SanDisk" },      { 0x1C5C, "SK hynix" },    { 0x126F, "Silicon Motion" },
    { 0x1AF4, "Virtio" },       { 0x1234, "QEMU" },       { 0x1B36, "Red Hat/QEMU" },        { 0x15AD, "VMware" },
    { 0x80EE, "VirtualBox" },   { 0x1D6A, "Aquantia" },    { 0x1A3B, "AzureWave" },
};

static const char* pci_vendor_name(uint16_t v) {
    for (unsigned i = 0; i < sizeof(s_pci_vendors) / sizeof(s_pci_vendors[0]); i++)
        if (s_pci_vendors[i].id == v) return s_pci_vendors[i].name;
    return "";
}

static const char* pci_class_name(uint8_t c, uint8_t s) {
    switch (c) {
        case 0x00: return "Unclassified";
        case 0x01:
            if (s == 0x01) return "IDE";
            if (s == 0x06) return "SATA";
            if (s == 0x08) return "NVMe";
            return "Mass storage";
        case 0x02: return (s == 0x00) ? "Ethernet" : "Network (Wi-Fi?)";
        case 0x03: return "VGA/Display";
        case 0x04: return (s == 0x03) ? "Audio (HDA)" : "Multimedia";
        case 0x05: return "Memory";
        case 0x06:
            if (s == 0x00) return "Host bridge";
            if (s == 0x01) return "ISA bridge";
            if (s == 0x04) return "PCI bridge";
            return "Bridge";
        case 0x07: return "Communication";
        case 0x08: return "System peripheral";
        case 0x09: return "Input";
        case 0x0B: return "Processor";
        case 0x0C:
            if (s == 0x03) return "USB";
            if (s == 0x05) return "SMBus";
            return "Serial bus";
        case 0x0D: return "Wireless (BT/other)";
        case 0x10: return "Encryption";
        case 0x11: return "Signal processing";
        default:   return "Device";
    }
}

static void pci_put_hex(uint32_t val, int digits, uint32_t color) {
    char buf[9];
    const char* hex = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) { buf[i] = hex[val & 0xF]; val >>= 4; }
    buf[digits] = '\0';
    kputs(buf, color);
}

void cmd_lspci(void) {
    int wifi_found = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vd = pci_config_read((uint8_t)bus, slot, func, 0x00);
                uint16_t vid = (uint16_t)(vd & 0xFFFF);
                if (vid == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                uint16_t did = (uint16_t)(vd >> 16);
                uint32_t cr  = pci_config_read((uint8_t)bus, slot, func, 0x08);
                uint8_t  bc  = (uint8_t)((cr >> 24) & 0xFF);
                uint8_t  sc  = (uint8_t)((cr >> 16) & 0xFF);
                uint32_t sub = pci_config_read((uint8_t)bus, slot, func, 0x2C);

                int net = (bc == 0x02);
                uint32_t col = net ? 0x55FF55 : 0xFFFFFF;

                pci_put_hex(bus, 2, 0xAAAAAA);  kputs(":", 0xAAAAAA);
                pci_put_hex(slot, 2, 0xAAAAAA); kputs(".", 0xAAAAAA);
                pci_put_hex(func, 1, 0xAAAAAA); kputs(" ", 0);
                kputs(pci_class_name(bc, sc), col);
                kputs(" [", 0xAAAAAA); pci_put_hex(bc, 2, 0xAAAAAA);
                pci_put_hex(sc, 2, 0xAAAAAA); kputs("]: ", 0xAAAAAA);
                const char* vn = pci_vendor_name(vid);
                if (vn[0]) { kputs(vn, col); kputs(" ", 0); }
                pci_put_hex(vid, 4, col); kputs(":", col); pci_put_hex(did, 4, col);
                if (net) {
                    kputs("  подсистема ", 0xAAAAAA);
                    pci_put_hex(sub & 0xFFFF, 4, 0xFFFFFF); kputs(":", 0xFFFFFF);
                    pci_put_hex(sub >> 16, 4, 0xFFFFFF);
                    if (sc != 0x00) wifi_found = 1;
                }
                kputs("\n", 0);

                if (func == 0) {
                    uint32_t hdr = pci_config_read((uint8_t)bus, slot, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;   // не многофункциональное
                }
            }
        }
    }
    kputs(wifi_found ? "\nWi-Fi: карта на шине есть (строка Network выше).\n"
                     : "\nWi-Fi: карты на шине PCI не видно.\n",
          wifi_found ? 0x55FF55 : 0xFFFF55);
}
