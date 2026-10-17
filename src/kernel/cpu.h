#ifndef CPU_H
#define CPU_H
// cpu.h — паспорт процессора и управление его частотой (см. cpu.c)

#include <stdint.h>

#define CPU_GOV_NONE   0   // частотой никто не управляет (решает прошивка)
#define CPU_GOV_PSTATE 1   // мы сами запросили максимальный P-state
#define CPU_GOV_HWP    2   // аппаратный регулятор Intel, попрошенный на максимум

typedef struct {
    char vendor[16];       // "GenuineIntel" / "AuthenticAMD"
    char brand[52];        // строка модели из CPUID
    int  family, model, stepping;
    int  cores;            // физических ядер в пакете
    int  threads;          // логических (потоков)
    int  base_mhz;         // паспортная (базовая) частота
    int  max_mhz;          // максимум с турбо
    int  min_mhz;          // минимальный множитель, если известен
    int  cur_mhz;          // измеренная текущая (APERF/MPERF)
    int  turbo_on;         // турбо разрешено
    int  governor;         // CPU_GOV_*
    int  has_eist, has_hwp, has_turbo_bit, has_aperf;
    int  is_intel, is_amd;
} cpu_info_t;

void cpu_init(void);               // определить и сразу снять ограничения
void cpu_unleash(void);            // снять запрет турбо и запросить максимум
int  cpu_current_mhz(void);        // измерить текущую частоту (нужен повторный вызов)
const cpu_info_t* cpu_get(void);
// Температура в °C: пакет, текущее ядро, TjMax. 0 — датчика нет (или не умеем)
int  cpu_temperature(int* pkg_c, int* core_c, int* tjmax_c);

#endif // CPU_H
