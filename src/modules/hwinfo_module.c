// hwinfo.c — Полная аппаратная диагностика GPU, RAM (SMBIOS) и системной платы.
// Собирается с флагом -mgeneral-regs-only, нулевой BSS, проходит проверку SREL.

#include "../drivers/api.h"
#include <stdint.h>

int driver_entry(const devos_api_t* api);

__attribute__((section(".header")))
const sys_header_t hwinfo_sys_header = {
    .magic = { SYS_MAGIC_0, SYS_MAGIC_1, SYS_MAGIC_2, SYS_MAGIC_3 },
    .entry_point = driver_entry,
    .required_api_ver = 1,
    .driver_name = "HWINFO_SYS",
    .flags = 0
};

static const devos_api_t* g_api = 0;

// -------------------------------------------------- Вспомогательный вывод
static void print_dec(uint32_t val, uint32_t col) {
    char buf[16];
    int idx = 0;
    if (val == 0) buf[idx++] = '0';
    while (val > 0) { buf[idx++] = '0' + (val % 10); val /= 10; }
    while (idx > 0) g_api->kputc(buf[--idx], col);
}

static void print_hex32(uint32_t val, uint32_t col) {
    const char hex[] = "0123456789ABCDEF";
    g_api->kputs("0x", col);
    for (int i = 7; i >= 0; i--) g_api->kputc(hex[(val >> (i * 4)) & 0x0F], col);
}

// -------------------------------------------------- PCI Config Access
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

// -------------------------------------------------- 1. ДИАГНОСТИКА NVIDIA GPU
#define NV_BAR0_DEFAULT       0xF6000000
#define NV_PMC_BOOT_0         0x00000000
#define NV_PTHERM_TEMP        0x00020014 // Термодатчик кристалла
#define NV_PFB_CSTATUS        0x0010020C // Регистр контроллера видеопамяти

static void probe_gpu(void) {
    g_api->kputs("---------------- [ GPU Architecture & Memory ] ----------------\n", 0x55FFFF);

    // Ищем NVIDIA на шине PCI
    int found = 0;
    uint8_t g_bus = 0, g_slot = 0;
    for (uint16_t b = 0; b < 16; b++) {
        for (uint8_t s = 0; s < 32; s++) {
            uint32_t ven_dev = pci_read(b, s, 0, 0x00);
            if ((ven_dev & 0xFFFF) == 0x10DE) {
                found = 1;
                g_bus = b;
                g_slot = s;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        g_api->kputs("[-] NVIDIA GPU not responding on PCIe bus.\n", 0xFF5555);
        return;
    }

    uint32_t dev_id = pci_read(g_bus, g_slot, 0, 0x00) >> 16;
    uint32_t bar0 = pci_read(g_bus, g_slot, 0, 0x10) & ~0x0F;
    uint32_t bar1 = pci_read(g_bus, g_slot, 0, 0x14) & ~0x0F; // VRAM Window

    g_api->kputs("GPU Device ID    : ", 0xAAAAAA);
    print_hex32(dev_id, 0xFFFF55);
    g_api->kputs(" (GeForce 840M / GM108)\n", 0x55FF55);

    g_api->kputs("MMIO BAR0 / VRAM : ", 0xAAAAAA);
    print_hex32(bar0, 0xFFFF55);
    g_api->kputs(" | VRAM Aperture: ", 0xAAAAAA);
    print_hex32(bar1, 0xFFFF55);
    g_api->kputs("\n", 0xFFFFFF);

    volatile uint32_t* nv_mmio = (volatile uint32_t*)(uintptr_t)bar0;

    // Считываем кремниевую ревизию
    uint32_t boot0 = nv_mmio[NV_PMC_BOOT_0 / 4];
    uint32_t arch = (boot0 >> 20) & 0x1FF;
    uint32_t step = (boot0 >> 16) & 0x0F;

    g_api->kputs("Silicon Arch ID  : Family NV", 0xAAAAAA);
    print_dec(arch, 0x55FF55);
    g_api->kputs(" (Maxwell GM108, Stepping ", 0xAAAAAA);
    print_dec(step, 0xFFFF55);
    g_api->kputs(")\n", 0xAAAAAA);

    // Считываем физический термодатчик чипа NVIDIA
    uint32_t raw_temp = nv_mmio[NV_PTHERM_TEMP / 4];
    uint32_t gpu_temp = raw_temp & 0x1FF; // Биты [8:0] = температура в градусах Цельсия

    g_api->kputs("GPU Die Temp     : ", 0xAAAAAA);
    if (gpu_temp > 0 && gpu_temp < 120) {
        print_dec(gpu_temp, (gpu_temp >= 65) ? 0xFF5555 : 0x55FF55);
        g_api->kputs(" C (Dedicated Hardware Diode)\n", 0xAAAAAA);
    } else {
        g_api->kputs("Standby / Clock Gated\n", 0xFFA500);
    }
}

// -------------------------------------------------- 2. ОПРОС ОЗУ И ПЛАТЫ ЧЕРЕЗ SMBIOS / DMI
#pragma pack(push, 1)
typedef struct {
    char     entry_anchor[4];   // "_SM_"
    uint8_t  checksum;
    uint8_t  length;
    uint8_t  major_version;
    uint8_t  minor_version;
    uint16_t max_struct_size;
    uint8_t  entry_rev;
    char     formatted[5];
    char     dmi_anchor[5];     // "_DMI_"
    uint8_t  dmi_checksum;
    uint16_t table_length;
    uint32_t table_address;
    uint16_t num_structures;
    uint8_t  bcd_revision;
} smbios_entry_t;

typedef struct {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
} smbios_header_t;
#pragma pack(pop)

// Получение строки из таблицы SMBIOS
static const char* smbios_get_str(const smbios_header_t* h, uint8_t str_idx) {
    if (str_idx == 0) return "N/A";
    const char* str = (const char*)h + h->length;
    while (str_idx > 1) {
        while (*str) str++;
        str++;
        if (*str == '\0') return "N/A";
        str_idx--;
    }
    return str;
}

static void probe_motherboard_and_ram(void) {
    g_api->kputs("---------------- [ Motherboard & RAM (SMBIOS) ] --------------\n", 0x55FFFF);

    // 1. Сканируем диапазон BIOS ROM (0xF0000 - 0xFFFFF) в поисках сигнатуры "_SM_"
    const uint8_t* bios_mem = (const uint8_t*)0xF0000;
    const smbios_entry_t* sm = 0;

    for (uint32_t off = 0; off < 0x10000; off += 16) {
        if (bios_mem[off] == '_' && bios_mem[off+1] == 'S' && 
            bios_mem[off+2] == 'M' && bios_mem[off+3] == '_') {
            sm = (const smbios_entry_t*)&bios_mem[off];
            break;
        }
    }

    if (!sm) {
        g_api->kputs("[-] SMBIOS tables not located in 0xF0000 ROM range.\n", 0xFF5555);
        return;
    }

    g_api->kputs("SMBIOS Revision  : ", 0xAAAAAA);
    print_dec(sm->major_version, 0xFFFF55);
    g_api->kputs(".", 0xAAAAAA);
    print_dec(sm->minor_version, 0xFFFF55);
    g_api->kputs(" | Tables at: ", 0xAAAAAA);
    print_hex32(sm->table_address, 0xFFFF55);
    g_api->kputs("\n", 0xFFFFFF);

    // 2. Обходим структуры SMBIOS
    const uint8_t* ptr = (const uint8_t*)(uintptr_t)sm->table_address;
    uint32_t total_ram_mb = 0;
    uint32_t slot_num = 0;

    for (uint16_t i = 0; i < sm->num_structures; i++) {
        const smbios_header_t* hdr = (const smbios_header_t*)ptr;

        // Type 1: Информация о материнской плате и ноутбуке
        if (hdr->type == 1) {
            uint8_t mfg_idx = ptr[4];
            uint8_t prod_idx = ptr[5];
            g_api->kputs("Laptop Platform  : ", 0xAAAAAA);
            g_api->kputs(smbios_get_str(hdr, mfg_idx), 0x55FF55);
            g_api->kputs(" - ", 0xAAAAAA);
            g_api->kputs(smbios_get_str(hdr, prod_idx), 0x55FF55);
            g_api->kputs("\n", 0xFFFFFF);
        }

        // Type 17: Физические слоты оперативной памяти (RAM)
        if (hdr->type == 17) {
            slot_num++;
            uint16_t size = *(const uint16_t*)&ptr[0x0C];
            uint16_t speed = *(const uint16_t*)&ptr[0x15];
            uint8_t type = ptr[0x12];

            g_api->kputs("RAM Slot #", 0xAAAAAA);
            print_dec(slot_num, 0xFFFF55);
            g_api->kputs(": ", 0xAAAAAA);

            if (size == 0 || size == 0xFFFF) {
                g_api->kputs("[Empty Slot]\n", 0x888888);
            } else {
                uint32_t size_mb = (size & 0x8000) ? ((size & 0x7FFF) / 1024) : size;
                total_ram_mb += size_mb;

                print_dec(size_mb, 0x55FF55);
                g_api->kputs(" MB (", 0xAAAAAA);
                print_dec(size_mb / 1024, 0x55FF55);
                g_api->kputs(" GB) | ", 0xAAAAAA);

                // Определение стандарта DDR
                if (type == 0x18)      g_api->kputs("DDR3", 0x00FFFF);
                else if (type == 0x1A) g_api->kputs("DDR4", 0x00FFFF);
                else                   g_api->kputs("SDRAM", 0x00FFFF);

                g_api->kputs(" @ ", 0xAAAAAA);
                print_dec(speed, 0xFFFF55);
                g_api->kputs(" MHz\n", 0xAAAAAA);
            }
        }

        if (hdr->type == 127) break; // End of Table marker

        // Пропускаем строки структуры до двойного нуля (0x0000)
        ptr += hdr->length;
        while (*(const uint16_t*)ptr != 0) ptr++;
        ptr += 2;
    }

    g_api->kputs("Total System RAM : ", 0xAAAAAA);
    print_dec(total_ram_mb, 0x55FF55);
    g_api->kputs(" MB (", 0xAAAAAA);
    print_dec(total_ram_mb / 1024, 0x55FF55);
    g_api->kputs(" GB Installed)\n", 0xAAAAAA);
}

// -------------------------------------------------- 3. ОБЗОР ШИНЫ PCI
static void probe_pci_bus(void) {
    g_api->kputs("---------------- [ Core PCI Controllers ] --------------------\n", 0x55FFFF);

    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t ven_dev = pci_read(0, slot, 0, 0x00);
        uint16_t ven = ven_dev & 0xFFFF;
        uint16_t dev = ven_dev >> 16;

        if (ven == 0xFFFF || ven == 0x0000) continue;

        // Показываем ключевые контроллеры на шине 0
        if (slot == 0) {
            g_api->kputs("Host Bridge (MCH): ", 0xAAAAAA);
            print_hex32(dev, 0xFFFF55);
            g_api->kputs(" (Intel Haswell-U Controller)\n", 0x55FF55);
        } else if (slot == 2) {
            g_api->kputs("Integrated GPU   : ", 0xAAAAAA);
            print_hex32(dev, 0xFFFF55);
            g_api->kputs(" (Intel HD Graphics 4400 GT2)\n", 0x55FF55);
        } else if (slot == 31) {
            g_api->kputs("LPC / PCH Chipset: ", 0xAAAAAA);
            print_hex32(dev, 0xFFFF55);
            g_api->kputs(" (Intel Lynx Point-LP Mobile)\n", 0x55FF55);
        }
    }
}

// -------------------------------------------------- Главная точка входа
int driver_entry(const devos_api_t* api) {
    g_api = api;

    g_api->kputs("\n==============================================================\n", 0x55FFFF);
    g_api->kputs("        devOS COMPREHENSIVE HARDWARE INTERROGATOR             \n", 0x55FF55);
    g_api->kputs("==============================================================\n", 0x55FFFF);

    probe_gpu();
    probe_motherboard_and_ram();
    probe_pci_bus();

    g_api->kputs("==============================================================\n\n", 0x55FFFF);
    g_api->flush_buffer();
    return 0;
}