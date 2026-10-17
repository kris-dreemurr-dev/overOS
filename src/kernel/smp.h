#ifndef SMP_H
#define SMP_H
// smp.h — рабочие ядра (AP) для вычислений программ.
//
// Ядро ОС остаётся однопроцессорным: планировщик, драйверы и файловая система
// живут только на основном ядре (BSP). Дополнительные ядра — это «вычислители»
// для программ: процесс запускает на свободном ядре свою функцию (поток-рабочий)
// в собственном адресном пространстве, и она считает, не трогая ядро ОС.
// Из системных вызовов рабочему доступны только ожидание на слове памяти,
// номер ядра и выход — всё, что не трогает общее состояние ядра ОС.
//
// Свободное ядро спит в hlt, разбудить его может только IPI с основного:
// назначить работу, остановить (процесс завершается), сбросить TLB (у
// процесса сняли страницу), разбудить ждущего.

#include <stdint.h>

#define SMP_MAX       16            // рабочих ядер, не считая основного
#define SMP_AP_STACK  16384         // ядерный стек каждого рабочего ядра
#define SMP_IPI_VEC   0xF0          // единый вектор: смотри флаги в слоте
#define SMP_SPUR_VEC  0xFF          // ложное прерывание LAPIC

// Сисколлы (номера 137..142, обёртки в ovos.h)
#define SYS_SMP_INFO      137       // (ov_smp_info_t*) -> число рабочих ядер
#define SYS_WORKER_START  138       // (entry, arg, stack_top) -> номер ядра или <0
#define SYS_WAIT_U32      139       // (addr, val, timeout_ms): ждать, пока *addr == val
#define SYS_WAKE          140       // (addr): разбудить рабочих, ждущих на addr
#define SYS_WORKER_EXIT   141       // с рабочего: закончить; с основного: (номер) — остановить
#define SYS_WORKER_ID     142       // номер рабочего ядра, -1 на основном

// Что видит программа (sys_smp_info). Те же типы объявлены в ovos.h.
#ifndef OV_SMP_TYPES
#define OV_SMP_TYPES
typedef struct {
    int32_t  state;                 // 0 нет, 1 свободно, 2 назначено, 3 работает, 4 сломано
    int32_t  owner_pid;
    uint32_t apic_id;
    uint32_t reserved;
    uint64_t busy_tsc;              // тактов вне hlt с момента подъёма
    uint64_t total_tsc;             // тактов с момента подъёма
} ov_smp_cpu_t;

typedef struct {
    int32_t  online;                // рабочих ядер
    int32_t  free;                  // из них свободных
    uint32_t bsp_apic_id;
    uint32_t reserved;
    uint64_t tsc_per_ms;
    ov_smp_cpu_t cpu[SMP_MAX];
} ov_smp_info_t;
#endif


// Подъём рабочих ядер (загрузчик). Возвращает их число; 0 — без SMP.
int  smp_init(int expected_threads);
int  smp_online(void);
const char* smp_status_text(void);   // для строки загрузки: почему 0 ядер

// Хуки ядра ОС
void smp_stop_space(uint64_t cr3);   // снять всех рабочих с адресного пространства и дождаться
void smp_flush_space(uint64_t cr3);  // сбросить TLB рабочим на этом пространстве
void smp_poll(void);                 // основной цикл ядра: сбои рабочих -> снять процесс

// Сисколлы с основного ядра (вызывает syscall_dispatcher)
uint64_t smp_bsp_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                         int pid, uint64_t cr3);

// Стеки рабочих ядер: по адресу стека видно, на каком ядре мы (дёшево, без LAPIC)
extern uint8_t smp_ap_stacks[SMP_MAX][SMP_AP_STACK];
static inline int smp_on_ap(void) {
    uint64_t sp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(sp));
    return sp >= (uint64_t)smp_ap_stacks && sp < (uint64_t)smp_ap_stacks + sizeof(smp_ap_stacks);
}

// Вход с рабочего ядра: исключение (не возвращается)
void smp_ap_exception(uint64_t* frame);

#endif
