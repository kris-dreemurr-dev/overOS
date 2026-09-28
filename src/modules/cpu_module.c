// cpuinfo_module.c — Полная аппаратная диагностика процессора (CPUID + MSR) для devOS.
// Полная совместимость с -mgeneral-regs-only, без BSS, размер ~4 КБ.

#include "../drivers/api.h"
#include <stdint.h>

int driver_entry(const devos_api_t* api);

__attribute__((section(".header")))
const sys_header_t cpuinfo_sys_header = {
    .magic = { SYS_MAGIC_0, SYS_MAGIC_1, SYS_MAGIC_2, SYS_MAGIC_3 },
    .entry_point = driver_entry,
    .required_api_ver = 1,
    .driver_name = "CPUINFO_SYS",
    .flags = 0
};

static const devos_api_t* g_api = 0;

// -------------------------------------------------- Низкоуровневые вызовы
static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile ("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void print_dec(uint32_t val, uint32_t col) {
    char buf[16];
    int idx = 0;
    if (val == 0) buf[idx++] = '0';
    while (val > 0) { buf[idx++] = '0' + (val % 10); val /= 10; }
    while (idx > 0) g_api->kputc(buf[--idx], col);
}

static void print_hex(uint32_t val, uint32_t col) {
    const char hex[] = "0123456789ABCDEF";
    g_api->kputs("0x", col);
    for (int i = 7; i >= 0; i--) g_api->kputc(hex[(val >> (i * 4)) & 0x0F], col);
}

// -------------------------------------------------- Главная диагностика
int driver_entry(const devos_api_t* api) {
    g_api = api;

    g_api->kputs("\n======================================================\n", 0x55FFFF);
    g_api->kputs("        devOS HARDWARE CPU ARCHITECTURE PROBE         \n", 0x55FF55);
    g_api->kputs("======================================================\n", 0x55FFFF);
    g_api->flush_buffer();

    uint32_t eax, ebx, ecx, edx;

    // 1. Имя производителя (Vendor ID)
    char vendor[13];
    cpuid(0, 0, &eax, (uint32_t*)&vendor[0], (uint32_t*)&vendor[8], (uint32_t*)&vendor[4]);
    vendor[12] = '\0';
    g_api->kputs("CPU Vendor       : ", 0xAAAAAA);
    g_api->kputs(vendor, 0xFFFF55);
    g_api->kputs("\n", 0xFFFFFF);

    // 2. Полная бренд-строка (Brand String)
    char brand[49];
    for (int i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, 0, 
              (uint32_t*)&brand[i * 16 + 0], 
              (uint32_t*)&brand[i * 16 + 4], 
              (uint32_t*)&brand[i * 16 + 8], 
              (uint32_t*)&brand[i * 16 + 12]);
    }
    brand[48] = '\0';
    // Пропускаем ведущие пробелы
    char* clean_brand = brand;
    while (*clean_brand == ' ') clean_brand++;
    g_api->kputs("Processor Model  : ", 0xAAAAAA);
    g_api->kputs(clean_brand, 0x55FF55);
    g_api->kputs("\n", 0xFFFFFF);

    // 3. Семейство, модель, степпинг
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    uint32_t stepping = eax & 0x0F;
    uint32_t model = ((eax >> 4) & 0x0F) | (((eax >> 16) & 0x0F) << 4);
    uint32_t family = ((eax >> 8) & 0x0F) | (((eax >> 20) & 0xFF) << 4);

    g_api->kputs("Silicon ID       : Family ", 0xAAAAAA);
    print_dec(family, 0xFFFF55);
    g_api->kputs(", Model ", 0xAAAAAA);
    print_hex(model, 0xFFFF55);
    g_api->kputs(", Stepping ", 0xAAAAAA);
    print_dec(stepping, 0xFFFF55);
    g_api->kputs("\n", 0xFFFFFF);

    // 4. Топология ядер и потоков
    uint32_t max_logical = (ebx >> 16) & 0xFF; // Всего логических процессоров
    // Детализация через CPUID leaf 4 (кэш/ядра)
    cpuid(4, 0, &eax, &ebx, &ecx, &edx);
    uint32_t physical_cores = ((eax >> 26) & 0x3F) + 1;

    g_api->kputs("Topology         : ", 0xAAAAAA);
    print_dec(physical_cores, 0x55FF55);
    g_api->kputs(" Physical Cores | ", 0xAAAAAA);
    print_dec(max_logical, 0x55FF55);
    g_api->kputs(" Logical Threads", 0xAAAAAA);
    if (max_logical > physical_cores) {
        g_api->kputs(" (Hyper-Threading: ON)\n", 0x00FFFF);
    } else {
        g_api->kputs(" (Hyper-Threading: OFF)\n", 0xFFA500);
    }

    // 5. Частотная сетка из MSR
    g_api->kputs("---------------- [ Frequency Plan ] ------------------\n", 0x55FFFF);
    
    // Базовый множитель (MSR_PLATFORM_INFO 0xCE)
    uint64_t plat_info = read_msr(0xCE);
    uint32_t base_ratio = (plat_info >> 8) & 0xFF;
    g_api->kputs("Base Multiplier  : x", 0xAAAAAA);
    print_dec(base_ratio, 0xFFFF55);
    g_api->kputs(" (", 0xAAAAAA);
    print_dec(base_ratio * 100, 0xFFFF55);
    g_api->kputs(" MHz)\n", 0xAAAAAA);

    // Лимиты Turbo Boost (MSR_TURBO_RATIO_LIMIT 0x1AD)
    uint64_t turbo_limit = read_msr(0x1AD);
    uint32_t turbo_1c = turbo_limit & 0xFF;
    uint32_t turbo_2c = (turbo_limit >> 8) & 0xFF;

    g_api->kputs("Turbo 1-Core Max : x", 0xAAAAAA);
    print_dec(turbo_1c, 0x55FF55);
    g_api->kputs(" (", 0xAAAAAA);
    print_dec(turbo_1c * 100, 0x55FF55);
    g_api->kputs(" MHz)\n", 0xAAAAAA);

    g_api->kputs("Turbo All-Cores  : x", 0xAAAAAA);
    print_dec(turbo_2c, 0x55FF55);
    g_api->kputs(" (", 0xAAAAAA);
    print_dec(turbo_2c * 100, 0x55FF55);
    g_api->kputs(" MHz)\n", 0xAAAAAA);

    // Текущий фактический множитель (IA32_PERF_STATUS 0x198)
    uint64_t perf_status = read_msr(0x198);
    uint32_t cur_ratio = (perf_status >> 8) & 0xFF;
    g_api->kputs("Current Frequency: x", 0xAAAAAA);
    print_dec(cur_ratio, 0x00FFFF);
    g_api->kputs(" (", 0xAAAAAA);
    print_dec(cur_ratio * 100, 0x00FFFF);
    g_api->kputs(" MHz)\n", 0xAAAAAA);

    // 6. Термопакет и лимиты мощности (MSR_PKG_POWER_INFO 0x614)
    g_api->kputs("---------------- [ Thermal & Limits ] ----------------\n", 0x55FFFF);
    
    // TjMax (MSR_TEMPERATURE_TARGET 0x1A2)
    uint64_t target_msr = read_msr(0x1A2);
    uint32_t tj_max = (target_msr >> 16) & 0xFF;
    if (tj_max == 0) tj_max = 100;
    
    // Текущая температура (IA32_THERM_STATUS 0x19C)
    uint64_t status_msr = read_msr(0x19C);
    uint32_t delta = (status_msr >> 16) & 0x7F;
    uint32_t cur_temp = (delta <= tj_max) ? (tj_max - delta) : 0;

    g_api->kputs("TjMax Threshold  : ", 0xAAAAAA);
    print_dec(tj_max, 0xFF5555);
    g_api->kputs(" C\n", 0xAAAAAA);

    g_api->kputs("Current Core Temp: ", 0xAAAAAA);
    print_dec(cur_temp, (cur_temp >= 65) ? 0xFF5555 : 0x55FF55);
    g_api->kputs(" C\n", 0xAAAAAA);

    // Чтение пакета питания TDP (Package Power Limit MSR 0x610)
    uint64_t power_limit = read_msr(0x610);
    // Для мобильных Haswell делитель мощности равен 1/8 Вт
    uint32_t pl1_watts = (power_limit & 0x7FFF) / 8;
    g_api->kputs("TDP Power (PL1)  : ", 0xAAAAAA);
    print_dec(pl1_watts, 0xFFFF55);
    g_api->kputs(" Watts\n", 0xAAAAAA);

    // 7. Поддерживаемые инструкции
    g_api->kputs("---------------- [ Instruction Sets ] ----------------\n", 0x55FFFF);
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    cpuid(7, 0, &eax, (uint32_t*)&brand[0], (uint32_t*)&brand[4], (uint32_t*)&brand[8]); // расширенные флаги (ebx в brand)
    uint32_t ext_ebx = *(uint32_t*)&brand[0];

    g_api->kputs("Features         : ", 0xAAAAAA);
    if (edx & (1 << 25)) g_api->kputs("SSE ", 0x55FF55);
    if (edx & (1 << 26)) g_api->kputs("SSE2 ", 0x55FF55);
    if (ecx & (1 << 19)) g_api->kputs("SSE4.1 ", 0x55FF55);
    if (ecx & (1 << 20)) g_api->kputs("SSE4.2 ", 0x55FF55);
    if (ecx & (1 << 12)) g_api->kputs("FMA3 ", 0x55FF55);
    if (ecx & (1 << 28)) g_api->kputs("AVX ", 0x55FF55);
    if (ext_ebx & (1 << 5)) g_api->kputs("AVX2 ", 0x55FF55);
    if (ecx & (1 << 5))  g_api->kputs("VMX ", 0x55FF55);
    g_api->kputs("\n======================================================\n\n", 0x55FFFF);
    g_api->flush_buffer();

    return 0;
}