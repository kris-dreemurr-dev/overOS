// cpu.c — что за процессор и на какой частоте он работает.
//
// Раньше система не знала о процессоре ничего, кроме строки из CPUID, и не
// трогала управление частотой вообще: множитель оставался таким, каким его
// оставила прошивка. Здесь мы:
//   * читаем топологию и паспортные множители (базовый, максимальный турбо);
//   * снимаем запрет турбо, если прошивка его поставила;
//   * просим у процессора максимальную производительность — через HWP на
//     новых Intel, через IA32_PERF_CTL на старых, через P-state MSR на AMD;
//   * измеряем НАСТОЯЩУЮ текущую частоту по счётчикам APERF/MPERF.
//
// Любой MSR здесь может отсутствовать на конкретном железе, а обращение к
// несуществующему даёт #GP. Поэтому все обращения идут через msr_try_*, где
// обработчик исключений перешагивает инструкцию (см. bsod.c), и ядро не падает
// на чужом ноутбуке из-за одного регистра.

#include "cpu.h"
#include "bsod.h"

extern uint64_t g_tsc_per_ms;
extern void sleep_ms(uint32_t ms);

// ---- Архитектурные MSR ----------------------------------------------------
#define MSR_PLATFORM_INFO     0x000000CE   // базовый и минимальный множители
#define MSR_PERF_STATUS       0x00000198
#define MSR_PERF_CTL          0x00000199   // запрос P-state (EIST)
#define MSR_MISC_ENABLE       0x000001A0   // бит 38 — ЗАПРЕТ турбо
#define MSR_TURBO_RATIO_LIMIT 0x000001AD
#define MSR_MPERF             0x000000E7
#define MSR_APERF             0x000000E8
#define MSR_PM_ENABLE         0x00000770   // HWP
#define MSR_HWP_CAPABILITIES  0x00000771
#define MSR_HWP_REQUEST       0x00000774

// AMD
#define MSR_AMD_HWCR          0xC0010015   // бит 25 — ЗАПРЕТ Core Performance Boost
#define MSR_AMD_PSTATE_CTL    0xC0010062   // какой P-state запросить (0 — самый быстрый)
#define MSR_AMD_PSTATE_STATUS 0xC0010063
#define MSR_AMD_PSTATE_DEF0   0xC0010064   // описание P0: множитель и делитель

static cpu_info_t s_cpu;
static int        s_ready = 0;

static inline void cpuid_raw(uint32_t leaf, uint32_t sub,
                             uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                             : "a"(leaf), "c"(sub));
}

// Чтение MSR, которого может не быть: 1 — прочитали, 0 — процессор ответил #GP
static int msr_try_read(uint32_t msr, uint64_t* out) {
    uint32_t lo = 0, hi = 0;
    g_msr_fault = 0;
    g_msr_probe = 1;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    int ok = !g_msr_fault;
    g_msr_probe = 0;
    if (ok && out) *out = ((uint64_t)hi << 32) | lo;
    return ok;
}

static int msr_try_write(uint32_t msr, uint64_t val) {
    g_msr_fault = 0;
    g_msr_probe = 1;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
    int ok = !g_msr_fault;
    g_msr_probe = 0;
    return ok;
}

// ---- Определение процессора ------------------------------------------------
static void read_identity(void) {
    uint32_t a, b, c, d;

    cpuid_raw(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;
    *(uint32_t*)&s_cpu.vendor[0] = b;
    *(uint32_t*)&s_cpu.vendor[4] = d;
    *(uint32_t*)&s_cpu.vendor[8] = c;
    s_cpu.vendor[12] = '\0';

    s_cpu.is_intel = (b == 0x756E6547);   // "Genu"
    s_cpu.is_amd   = (b == 0x68747541);   // "Auth"

    // Строка модели (расширенные листы 0x80000002..4)
    cpuid_raw(0x80000000, 0, &a, &b, &c, &d);
    s_cpu.brand[0] = '\0';
    if (a >= 0x80000004) {
        uint32_t* p = (uint32_t*)s_cpu.brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid_raw(leaf, 0, &a, &b, &c, &d);
            *p++ = a; *p++ = b; *p++ = c; *p++ = d;
        }
        s_cpu.brand[48] = '\0';
    }

    cpuid_raw(1, 0, &a, &b, &c, &d);
    uint32_t family = (a >> 8) & 0xF;
    uint32_t model  = (a >> 4) & 0xF;
    if (family == 0xF) family += (a >> 20) & 0xFF;
    if (family == 6 || family == 0xF) model |= ((a >> 16) & 0xF) << 4;
    s_cpu.family = (int)family;
    s_cpu.model  = (int)model;
    s_cpu.stepping = (int)(a & 0xF);
    s_cpu.has_eist = (c >> 7) & 1;
    s_cpu.threads  = (int)((b >> 16) & 0xFF);     // логических на пакет
    if (s_cpu.threads < 1) s_cpu.threads = 1;

    // Физические ядра: у Intel — лист 4, у AMD — 0x8000001E/0x80000008
    s_cpu.cores = s_cpu.threads;
    if (s_cpu.is_intel && max_leaf >= 4) {
        cpuid_raw(4, 0, &a, &b, &c, &d);
        int n = (int)((a >> 26) & 0x3F) + 1;
        if (n > 0) s_cpu.cores = n;
    } else if (s_cpu.is_amd) {
        cpuid_raw(0x80000000, 0, &a, &b, &c, &d);
        if (a >= 0x8000001E) {
            cpuid_raw(0x8000001E, 0, &a, &b, &c, &d);
            int per_core = (int)((b >> 8) & 0xFF) + 1;   // потоков на ядро
            if (per_core > 0) s_cpu.cores = s_cpu.threads / per_core;
        }
    }
    if (s_cpu.cores < 1) s_cpu.cores = 1;

    // Возможности управления питанием (лист 6)
    s_cpu.has_turbo_bit = 0;
    s_cpu.has_hwp = 0;
    s_cpu.has_aperf = 0;
    if (max_leaf >= 6) {
        cpuid_raw(6, 0, &a, &b, &c, &d);
        s_cpu.has_turbo_bit = a & 1;          // Turbo Boost / Core Performance Boost
        s_cpu.has_hwp       = (a >> 7) & 1;   // Hardware P-States
        s_cpu.has_aperf     = c & 1;          // счётчики APERF/MPERF
    }
    if (s_cpu.is_amd) {
        cpuid_raw(0x80000007, 0, &a, &b, &c, &d);
        if ((d >> 9) & 1) s_cpu.has_turbo_bit = 1;   // CPB
        if ((d >> 10) & 1) s_cpu.has_aperf = 1;      // EffFreq (APERF/MPERF)
    }
}

// ---- Паспортные частоты ----------------------------------------------------
// Шаг множителя у x86 — 100 МГц (у совсем старых Core 2 — 133, их мы и не ждём)
#define BCLK_MHZ 100

static void read_ratios(void) {
    uint64_t v;
    s_cpu.base_mhz = 0;
    s_cpu.max_mhz  = 0;

    if (s_cpu.is_intel) {
        if (msr_try_read(MSR_PLATFORM_INFO, &v)) {
            s_cpu.base_mhz = (int)(((v >> 8) & 0xFF) * BCLK_MHZ);
            s_cpu.min_mhz  = (int)(((v >> 40) & 0xFF) * BCLK_MHZ);
        }
        // Максимум турбо — множитель при загрузке одного ядра
        if (msr_try_read(MSR_TURBO_RATIO_LIMIT, &v)) {
            int r = (int)(v & 0xFF);
            if (r > 0) s_cpu.max_mhz = r * BCLK_MHZ;
        }
    } else if (s_cpu.is_amd) {
        // P0 описан в MSR_AMD_PSTATE_DEF0: FID/DID (семейство 17h и новее)
        if (msr_try_read(MSR_AMD_PSTATE_DEF0, &v) && (v >> 63)) {
            uint32_t fid = (uint32_t)(v & 0xFF);
            uint32_t did = (uint32_t)((v >> 8) & 0x3F);
            if (did) s_cpu.base_mhz = (int)((fid * 200) / did);
        }
    }

    // Паспортную частоту можно взять и из строки модели ("@ 2.90GHz") --
    // пригодится там, где MSR не ответили (например, под гипервизором).
    if (s_cpu.base_mhz == 0) {
        for (int i = 0; i < 44 && s_cpu.brand[i]; i++) {
            if (s_cpu.brand[i] == '@') {
                int ghz = 0, frac = 0, digits = 0, j = i + 1;
                while (s_cpu.brand[j] == ' ') j++;
                while (s_cpu.brand[j] >= '0' && s_cpu.brand[j] <= '9')
                    ghz = ghz * 10 + (s_cpu.brand[j++] - '0');
                if (s_cpu.brand[j] == '.') {
                    j++;
                    while (s_cpu.brand[j] >= '0' && s_cpu.brand[j] <= '9' && digits < 2) {
                        frac = frac * 10 + (s_cpu.brand[j++] - '0');
                        digits++;
                    }
                }
                while (digits++ < 2) frac *= 10;
                s_cpu.base_mhz = ghz * 1000 + frac * 10;
                break;
            }
        }
    }
    if (s_cpu.max_mhz < s_cpu.base_mhz) s_cpu.max_mhz = s_cpu.base_mhz;
}

// ---- Снять ограничения -----------------------------------------------------
void cpu_unleash(void) {
    uint64_t v;
    s_cpu.turbo_on = 0;
    s_cpu.governor = CPU_GOV_NONE;

    if (s_cpu.is_intel) {
        // 1. Турбо: бит 38 в MISC_ENABLE — это именно ЗАПРЕТ, снимаем его
        if (msr_try_read(MSR_MISC_ENABLE, &v)) {
            if (v & (1ULL << 38)) {
                v &= ~(1ULL << 38);
                msr_try_write(MSR_MISC_ENABLE, v);
            }
            if (msr_try_read(MSR_MISC_ENABLE, &v)) s_cpu.turbo_on = !(v & (1ULL << 38));
        }

        // 2. HWP (Skylake и новее): аппаратный регулятор, просим максимум
        if (s_cpu.has_hwp && msr_try_write(MSR_PM_ENABLE, 1)) {
            uint64_t cap = 0;
            int highest = 0xFF, lowest = 1;
            if (msr_try_read(MSR_HWP_CAPABILITIES, &cap)) {
                highest = (int)(cap & 0xFF);
                lowest  = (int)((cap >> 24) & 0xFF);
            }
            // min = max = highest, EPP = 0 (чистая производительность)
            uint64_t req = ((uint64_t)highest & 0xFF)
                         | (((uint64_t)highest & 0xFF) << 8)
                         | (0ULL << 24);
            (void)lowest;
            if (msr_try_write(MSR_HWP_REQUEST, req)) s_cpu.governor = CPU_GOV_HWP;
        }

        // 3. Старый путь (EIST): прямо просим максимальный множитель
        if (s_cpu.governor == CPU_GOV_NONE && s_cpu.has_eist && s_cpu.max_mhz > 0) {
            uint64_t ratio = (uint64_t)(s_cpu.max_mhz / BCLK_MHZ);
            if (msr_try_write(MSR_PERF_CTL, ratio << 8)) s_cpu.governor = CPU_GOV_PSTATE;
        }
    } else if (s_cpu.is_amd) {
        // Core Performance Boost: бит 25 в HWCR — тоже запрет
        if (msr_try_read(MSR_AMD_HWCR, &v)) {
            if (v & (1ULL << 25)) {
                v &= ~(1ULL << 25);
                msr_try_write(MSR_AMD_HWCR, v);
            }
            if (msr_try_read(MSR_AMD_HWCR, &v)) s_cpu.turbo_on = !(v & (1ULL << 25));
        }
        // Просим самый быстрый P-state
        if (msr_try_write(MSR_AMD_PSTATE_CTL, 0)) s_cpu.governor = CPU_GOV_PSTATE;
    }
}

// ---- Настоящая текущая частота --------------------------------------------
// APERF считает такты на фактической частоте, MPERF — на паспортной.
// Отношение даёт реальный множитель; окно измерения задаёт вызывающий.
static uint64_t s_aperf_prev, s_mperf_prev;
static int      s_counters_primed = 0;

int cpu_current_mhz(void) {
    if (!s_cpu.has_aperf || s_cpu.base_mhz == 0) return s_cpu.base_mhz;

    uint64_t a = 0, m = 0;
    if (!msr_try_read(MSR_APERF, &a) || !msr_try_read(MSR_MPERF, &m)) return s_cpu.base_mhz;

    if (!s_counters_primed) {
        s_aperf_prev = a;
        s_mperf_prev = m;
        s_counters_primed = 1;
        return s_cpu.base_mhz;
    }

    uint64_t da = a - s_aperf_prev;
    uint64_t dm = m - s_mperf_prev;
    s_aperf_prev = a;
    s_mperf_prev = m;

    if (dm == 0) return s_cpu.cur_mhz ? s_cpu.cur_mhz : s_cpu.base_mhz;
    // Окно меньше миллисекунды шумит -- такой замер отбрасываем
    if (dm < 100000) return s_cpu.cur_mhz ? s_cpu.cur_mhz : s_cpu.base_mhz;

    uint64_t mhz = ((uint64_t)s_cpu.base_mhz * da) / dm;
    if (mhz > 0 && mhz < 20000) s_cpu.cur_mhz = (int)mhz;
    return s_cpu.cur_mhz;
}

void cpu_init(void) {
    if (s_ready) return;
    read_identity();
    read_ratios();
    cpu_unleash();
    s_cpu.cur_mhz = s_cpu.base_mhz;
    s_ready = 1;
}

const cpu_info_t* cpu_get(void) {
    if (!s_ready) cpu_init();
    cpu_current_mhz();
    return &s_cpu;
}

// ---- Температура -----------------------------------------------------------
// Intel: цифровой датчик (CPUID.06H:EAX бит 0) показывает, сколько градусов
// осталось до TjMax — порога, где процессор начинает сбрасывать частоту.
// TjMax берём из MSR_TEMPERATURE_TARGET (у старых моделей его нет — 100 °C).
// Датчик ядра (IA32_THERM_STATUS) — ядро, на котором мы сейчас (BSP);
// пакетный (IA32_PACKAGE_THERM_STATUS, CPUID.06H:EAX бит 6) — самое горячее
// место кристалла, его и показываем как главное.
// AMD (семейство 17h+): Tctl в регистре SMN 0x59800, доступ через окно
// индекс/данные моста 0:0.0 (0x60/0x64); старые 15h/16h — PCI 0:18.3, 0xA4.
#define MSR_THERM_STATUS      0x0000019C
#define MSR_TEMP_TARGET       0x000001A2
#define MSR_PKG_THERM_STATUS  0x000001B1

extern uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern void pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

int cpu_temperature(int* pkg_c, int* core_c, int* tjmax_c) {
    *pkg_c = *core_c = -1;
    *tjmax_c = 100;
    if (s_cpu.is_intel) {
        uint32_t a, b, c, d;
        cpuid_raw(0, 0, &a, &b, &c, &d);
        if (a < 6) return 0;
        cpuid_raw(6, 0, &a, &b, &c, &d);
        if (!(a & 1)) return 0;                        // датчика нет
        uint64_t v;
        if (msr_try_read(MSR_TEMP_TARGET, &v)) {
            int t = (int)((v >> 16) & 0xFF);
            if (t >= 50 && t <= 130) *tjmax_c = t;
        }
        if (msr_try_read(MSR_THERM_STATUS, &v) && (v & (1ull << 31)))
            *core_c = *tjmax_c - (int)((v >> 16) & 0x7F);
        if ((a & (1u << 6)) && msr_try_read(MSR_PKG_THERM_STATUS, &v))
            *pkg_c = *tjmax_c - (int)((v >> 16) & 0x7F);
        if (*pkg_c < 0) *pkg_c = *core_c;
        return *pkg_c >= 0;
    }
    if (s_cpu.is_amd) {
        *tjmax_c = 95;
        if (s_cpu.family >= 0x17) {
            pci_config_write(0, 0, 0, 0x60, 0x00059800);
            uint32_t r = pci_config_read(0, 0, 0, 0x64);
            int t = (int)((r >> 21) * 125 / 1000);           // шаг 0.125 °C
            if (r & (1u << 19)) t -= 49;                     // диапазон -49..206
            if (t > 0 && t < 150) { *pkg_c = *core_c = t; return 1; }
        } else if (s_cpu.family >= 0x15) {
            uint32_t r = pci_config_read(0, 0x18, 3, 0xA4);
            int t = (int)((r >> 21) * 125 / 1000);
            if (t > 0 && t < 150) { *pkg_c = *core_c = t; return 1; }
        }
    }
    return 0;
}
