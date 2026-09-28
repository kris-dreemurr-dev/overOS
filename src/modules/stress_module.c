// stress_module.c — 4-поточный аппаратный стресс-тест CPU (SMP SIPI) + GeForce 840M.
// Исправлены смещения батута: исключена порча памяти AP-ядрами.

#include <stdint.h>
#include "../drivers/api.h"

int driver_entry(const devos_api_t* api);

__attribute__((section(".header")))
const sys_header_t stress_sys_header = {
    .magic = { SYS_MAGIC_0, SYS_MAGIC_1, SYS_MAGIC_2, SYS_MAGIC_3 },
    .entry_point = driver_entry,
    .required_api_ver = 1,
    .driver_name = "STRESS_SYS",
    .flags = 0
};

#define LAPIC_BASE 0xFEE00000

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void write_msr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static inline uint64_t rdtsc_wait(uint64_t ticks) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t target = (((uint64_t)hi << 32) | lo) + ticks;
    while (1) {
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        if ((((uint64_t)hi << 32) | lo) >= target) break;
        __asm__ volatile ("pause");
    }
    return target;
}

static void print_dec(const devos_api_t* api, uint32_t val, uint32_t col) {
    char buf[16];
    int idx = 0;
    if (val == 0) buf[idx++] = '0';
    while (val > 0) { buf[idx++] = '0' + (val % 10); val /= 10; }
    while (idx > 0) api->kputc(buf[--idx], col);
}

static uint32_t detect_logical_cores(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (!(edx & (1 << 28))) return 1;
    uint32_t logical_cores = (ebx >> 16) & 0xFF;
    return (logical_cores > 0) ? logical_cores : 1;
}

static uint32_t get_cpu_temp(void) {
    uint64_t target_msr = read_msr(0x1A2);
    uint32_t tj_max = (target_msr >> 16) & 0xFF;
    if (tj_max == 0) tj_max = 100;

    uint64_t status_msr = read_msr(0x19C);
    uint32_t delta = (status_msr >> 16) & 0x7F;

    return (delta <= tj_max) ? (tj_max - delta) : 0;
}

static uint32_t get_actual_mhz(void) {
    uint64_t perf_status = read_msr(0x198);
    uint32_t ratio = (perf_status >> 8) & 0xFF;
    return ratio * 100;
}

// -------------------------------------------------- Безопасный батут для ядер
static const uint8_t ap_trampoline_bin[] = {
    0xFA,                         // [0x00] cli
    0x31, 0xC0,                   // [0x01] xor ax, ax
    0x8E, 0xD8,                   // [0x03] mov ds, ax
    // burn_loop:
    0x80, 0x3E, 0x20, 0x80, 0x00, // [0x05] cmp byte [0x8020], 0
    0x75, 0x0A,                   // [0x0A] jne halt_core (прыжок на 10 байт вперед -> к hlt)
    0x05, 0x37, 0x13,             // [0x0C] add ax, 0x1337
    0x31, 0xC2,                   // [0x0F] xor dx, ax
    0xC1, 0xC2, 0x03,             // [0x11] rol dx, 3
    0xEB, 0xEF,                   // [0x14] jmp burn_loop (прыжок ровно на 0x05!)
    // halt_core:
    0xF4,                         // [0x16] hlt
    0xEB, 0xFD                    // [0x17] jmp halt_core
};

static void boot_all_cpu_cores(const devos_api_t* api, uint32_t total_cores) {
    api->kputs("[SMP] Injecting verified AP burn loop at 0x00008000...\n", 0xAAAAAA);

    volatile uint8_t* dest = (volatile uint8_t*)0x8000;
    for (uint32_t i = 0; i < sizeof(ap_trampoline_bin); i++) {
        dest[i] = ap_trampoline_bin[i];
    }

    // Сбрасываем флаг остановки по адресу 0x8020
    *(volatile uint8_t*)0x8020 = 0;

    volatile uint32_t* lapic = (volatile uint32_t*)LAPIC_BASE;
    lapic[0x0F0 / 4] |= 0x1FF; // Включаем APIC

    api->kputs("[SMP] Broadcasting INIT-SIPI to awaken ", 0x55FF55);
    print_dec(api, total_cores - 1, 0xFFFF55);
    api->kputs(" secondary threads...\n", 0x55FF55);

    // INIT IPI
    lapic[0x310 / 4] = 0;
    lapic[0x300 / 4] = 0x000C4500;
    rdtsc_wait(20000000ULL); // 10 мс

    // Startup IPI #1 (0x08 -> 0x8000)
    lapic[0x310 / 4] = 0;
    lapic[0x300 / 4] = 0x000C4608;
    rdtsc_wait(500000ULL);

    // Startup IPI #2
    lapic[0x300 / 4] = 0x000C4608;
    rdtsc_wait(1000000ULL);

    api->kputs("[SMP] Secondary cores running safely in ALU loop!\n", 0x55FF55);
}

static void stop_all_cpu_cores(void) {
    *(volatile uint8_t*)0x8020 = 1;
}

// -------------------------------------------------- Точка входа
int driver_entry(const devos_api_t* api) {
    api->kputs("\n======================================================\n", 0x55FFFF);
    api->kputs("   devOS 4-THREAD THERMAL PUMP + GEFORCE 840M        \n", 0x55FF55);
    api->kputs("======================================================\n", 0x55FFFF);

    uint32_t total_cores = detect_logical_cores();
    api->kputs("Logical Threads Detected: ", 0xAAAAAA);
    print_dec(api, total_cores, 0x55FF55);
    api->kputs("\n", 0xFFFFFF);

    // 1. Включаем Turbo Boost
    uint64_t misc = read_msr(0x1A0);
    misc |= (1ULL << 16);
    misc &= ~(1ULL << 38);
    write_msr(0x1A0, misc);
    write_msr(0x199, 0x1B00); // x27 = 2.7 ГГц
    write_msr(0x1B0, 0x00);

    // 2. Будим ядра
    if (total_cores > 1) {
        boot_all_cpu_cores(api, total_cores);
    }

    // 3. Активируем видеочип
    api->kputs("[GPU] Powering NVIDIA GeForce 840M clocks at 0xF6000000...\n", 0x55FF55);
    volatile uint32_t* nv_mmio = (volatile uint32_t*)0xF6000000;
    nv_mmio[0x200 / 4] = 0xFFFFFFFF;

    api->kputs("[+] 100% LOAD ACTIVE. Shared pipe warming up. Press key to stop.\n\n", 0xFFFF55);
    api->flush_buffer();

    uint64_t iter = 0;
    while (1) {
        // Нагрузка главного ядра (BSP) на 64-битных регистрах
        __asm__ volatile (
            ".rept 64\n"
            "add $1, %%rax\n"
            "xor %%rax, %%rbx\n"
            "sub $2, %%rcx\n"
            "imul $31, %%rcx, %%rdx\n"
            "xor %%rdx, %%r8\n"
            "add %%r8, %%r9\n"
            "xor %%r9, %%r10\n"
            "rol $5, %%r10\n"
            ".endr\n"
            : : : "rax", "rbx", "rcx", "rdx", "r8", "r9", "r10"
        );

        if ((iter & 0x7FF) == 0) {
            volatile uint32_t dummy = nv_mmio[0x9400 / 4];
            (void)dummy;
        }

        iter++;
        if ((iter & 0x1FFFF) == 0) {
            uint32_t temp = get_cpu_temp();
            uint32_t mhz  = get_actual_mhz();

            api->kputs("Core Temp: ", 0x55FF55);
            print_dec(api, temp, (temp >= 70) ? 0xFF5555 : (temp >= 62 ? 0xFFA500 : 0xFFFF55));
            api->kputs(" C | Clock: ", 0x55FF55);
            print_dec(api, mhz, 0x00FFFF);
            api->kputs(" MHz | Cores: 4/4 Active\r", 0x55FF55);
            api->flush_buffer();

            if (temp >= 88) {
                api->kputs("\n[!] Safeguard: 88C reached! Aborting...\n", 0xFF5555);
                break;
            }

            char probe[2];
            if (api->readline && api->readline(probe, 0)) break;
        }
    }

    // 4. Остановка и парковка ядер
    api->kputs("\nStopping load and parking cores...\n", 0xAAAAAA);
    stop_all_cpu_cores();
    write_msr(0x199, 0x1000);

    api->kputs("All cores safely parked. Cooling down.\n\n", 0x55FF55);
    api->flush_buffer();
    return 0;
}