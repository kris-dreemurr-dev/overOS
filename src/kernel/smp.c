// smp.c — рабочие ядра (AP): подъём, назначение работы программам, IPI.
//
// Устройство (подробнее в smp.h):
//   * основное ядро (BSP) поднимает остальные рассылкой INIT-SIPI-SIPI, они
//     проходят трамплин asm/ap_boot.asm и попадают в ap_main;
//   * у каждого рабочего свой GDT с TSS (занятый дескриптор TSS второе ядро
//     загрузить не может), свой ядерный стек и общий IDT ядра;
//   * в простое рабочее спит в hlt; основное шлёт IPI SMP_IPI_VEC, а что
//     делать, рабочее читает из своего слота (назначено / стоп / сбросить TLB);
//   * программа на рабочем ядре — обычный Ring 3 в адресном пространстве
//     процесса-владельца. Системные вызовы оттуда перехватываются ещё в
//     заглушке int 0x80 (smp_ap_syscall) и до syscall_dispatcher не доходят:
//     всё ядро ОС рассчитано на одно ядро, и чужой поток в нём испортил бы
//     состояние. Исключение там же — снимает только рабочего, не ядро ОС.
//
// Файл собирается с -mgeneral-regs-only: обработчики на рабочем ядре не
// должны трогать xmm, иначе испортят регистры программы, которая там считает.

#include "smp.h"
#include "sched.h"
#include "../memory/vmm.h"

extern uint64_t g_tsc_per_ms;
extern uint32_t timer_ticks;
#define TICKS (*(volatile uint32_t*)&timer_ticks)
extern void sleep_ms(uint32_t ms);
extern void kdebug(const char* str, uint32_t color);
extern void idt_load_this_cpu(void);
extern void idt_set_gate(int vec, void (*handler)(void), int dpl);

extern char ap_tr_start[], ap_tr_end[];
extern char ap_tr_cr0[], ap_tr_cr3[], ap_tr_cr4[], ap_tr_efer[];
extern char ap_tr_stack_base[], ap_tr_stack_size[], ap_tr_entry[], ap_tr_counter[], ap_tr_max[];

#define TRAMPOLINE_PHYS 0x8000

enum { CPU_OFF = 0, CPU_IDLE, CPU_ASSIGNED, CPU_RUNNING, CPU_BROKEN, CPU_CLAIMED };

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) smp_tss_t;

typedef struct {
    volatile int      state;
    volatile int      owner_pid;
    volatile uint64_t cr3;
    uint64_t          entry, arg, stack;
    volatile int      stop;              // основное просит бросить работу
    volatile uint32_t flush_req, flush_ack;
    volatile uint64_t wait_addr;         // ждёт на этом слове (SYS_WAIT_U32)
    volatile int      fault;             // программа упала на этом ядре
    int               fault_vec;
    uint64_t          fault_rip;
    int               fault_pid;
    uint32_t          apic_id;
    uint64_t          online_tsc;
    volatile uint64_t idle_tsc;
    uint64_t          gdt[7] __attribute__((aligned(16)));
    smp_tss_t         tss __attribute__((aligned(16)));
} smp_cpu_t;

uint8_t smp_ap_stacks[SMP_MAX][SMP_AP_STACK] __attribute__((aligned(4096)));
static smp_cpu_t s_cpu[SMP_MAX];
static volatile int s_online = 0;
static volatile int s_running = 0;       // сколько ядер в ASSIGNED/RUNNING: быстрый выход из хуков
static int s_x2apic = 0;
static volatile uint32_t* s_lapic = 0;
static uint32_t s_bsp_apic = 0;
static uint64_t s_pat = 0;
static const char* s_status = "not started";

// ---------------------------------------------------------------- мелочи
static inline uint64_t rdmsr(uint32_t m) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t m, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline void cpu_pause(void) { __asm__ volatile ("pause" ::: "memory"); }
static inline int cas(volatile int* p, int old, int nw) {
    return __atomic_compare_exchange_n(p, &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// Ждать условие не дольше ms, не полагаясь на прерывания (основное ядро может
// звать это под cli — например, из sched_exit_current)
static uint64_t tsc_deadline(uint32_t ms) {
    uint64_t per = g_tsc_per_ms ? g_tsc_per_ms : 3000000;
    return rdtsc() + per * ms;
}

// ---------------------------------------------------------------- LAPIC
static uint32_t lapic_read(uint32_t reg) {
    if (s_x2apic) return (uint32_t)rdmsr(0x800 + (reg >> 4));
    return s_lapic[reg / 4];
}
static void lapic_write(uint32_t reg, uint32_t v) {
    if (s_x2apic) { wrmsr(0x800 + (reg >> 4), v); return; }
    s_lapic[reg / 4] = v;
}
static uint32_t lapic_id(void) {
    return s_x2apic ? (uint32_t)rdmsr(0x802) : (s_lapic[0x20 / 4] >> 24);
}
static void lapic_eoi(void) { lapic_write(0xB0, 0); }

static void lapic_icr(uint32_t dest, uint32_t low) {
    if (s_x2apic) {
        wrmsr(0x830, ((uint64_t)dest << 32) | low);
        return;
    }
    s_lapic[0x310 / 4] = dest << 24;
    s_lapic[0x300 / 4] = low;
    for (int i = 0; i < 1000000 && (s_lapic[0x300 / 4] & (1u << 12)); i++) cpu_pause();
}

static void ipi(smp_cpu_t* c) { lapic_icr(c->apic_id, 0x4000 | SMP_IPI_VEC); }

// ---------------------------------------------------------------- рабочее ядро
static smp_cpu_t* this_cpu(void) {
    uint64_t sp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(sp));
    uint64_t i = (sp - (uint64_t)smp_ap_stacks) / SMP_AP_STACK;
    return &s_cpu[i < SMP_MAX ? i : 0];
}
static int cpu_index(smp_cpu_t* c) { return (int)(c - s_cpu); }

__attribute__((noreturn)) void ap_idle(int idx);

// Бросить всё, что делали, и вернуться в простой: стек — с вершины заново
__attribute__((noreturn)) static void ap_abandon(smp_cpu_t* c) {
    int idx = cpu_index(c);
    uint64_t top = (uint64_t)smp_ap_stacks[idx] + SMP_AP_STACK;
    __asm__ volatile ("cli\n\t"
                      "mov %0, %%rsp\n\t"
                      "mov %1, %%edi\n\t"
                      "call ap_idle\n\t"
                      : : "r"(top), "r"(idx) : "memory");
    __builtin_unreachable();
}

// Простой: hlt, пока основное не назначит работу. Сюда же возвращаются после
// выхода программы, сбоя или остановки.
__attribute__((noreturn)) void ap_idle(int idx) {
    smp_cpu_t* c = &s_cpu[idx];
    uint64_t kcr3 = (uint64_t)vmm_get_kernel_pml4_phys();
    __asm__ volatile ("mov %0, %%cr3" : : "r"(kcr3) : "memory");
    int was = c->state;
    c->wait_addr = 0;
    c->cr3 = 0;
    c->owner_pid = 0;
    c->flush_ack = c->flush_req;
    c->stop = 0;
    if (was == CPU_RUNNING || was == CPU_ASSIGNED) __atomic_sub_fetch(&s_running, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->state, CPU_IDLE, __ATOMIC_SEQ_CST);

    for (;;) {
        __asm__ volatile ("cli");
        if (c->state == CPU_ASSIGNED && cas(&c->state, CPU_ASSIGNED, CPU_RUNNING)) break;
        uint64_t t = rdtsc();
        __asm__ volatile ("sti; hlt; cli" ::: "memory");   // IPI между проверкой и hlt не теряется: sti даёт тень
        c->idle_tsc += rdtsc() - t;
    }

    if (c->stop) ap_abandon(c);
    __asm__ volatile ("mov %0, %%cr3" : : "r"(c->cr3) : "memory");
    // Чистое FPU/SSE: прошлый рабочий мог быть из другого процесса
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile ("fninit; ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile (
        "mov %0, %%rdi\n\t"
        "pushq $0x23\n\t"           // SS Ring 3
        "pushq %1\n\t"              // RSP
        "pushq $0x202\n\t"          // RFLAGS: IF=1
        "pushq $0x1B\n\t"           // CS Ring 3
        "pushq %2\n\t"              // RIP
        "xor %%eax, %%eax\n\t"
        "xor %%ebx, %%ebx\n\t"
        "xor %%ecx, %%ecx\n\t"
        "xor %%edx, %%edx\n\t"
        "xor %%esi, %%esi\n\t"
        "xor %%ebp, %%ebp\n\t"
        "xor %%r8d, %%r8d\n\t"
        "xor %%r9d, %%r9d\n\t"
        "xor %%r10d, %%r10d\n\t"
        "xor %%r11d, %%r11d\n\t"
        "iretq\n\t"
        : : "r"(c->arg), "r"(c->stack), "r"(c->entry) : "memory");
    __builtin_unreachable();
}

// Вектор SMP_IPI_VEC приходит только на рабочие ядра
void smp_ipi_handler(uint64_t* frame) {
    (void)frame;
    smp_cpu_t* c = this_cpu();
    lapic_eoi();
    if (c->flush_req != c->flush_ack) {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
        __atomic_store_n(&c->flush_ack, c->flush_req, __ATOMIC_SEQ_CST);
    }
    if (c->stop) ap_abandon(c);
}

__attribute__((naked)) static void smp_ipi_stub(void) {
    __asm__ volatile (
        "push %rax\n\t push %rbx\n\t push %rcx\n\t push %rdx\n\t push %rsi\n\t"
        "push %rdi\n\t push %rbp\n\t push %r8\n\t push %r9\n\t push %r10\n\t"
        "push %r11\n\t push %r12\n\t push %r13\n\t push %r14\n\t push %r15\n\t"
        "mov %rsp, %rdi\n\t"
        "call smp_ipi_handler\n\t"
        "pop %r15\n\t pop %r14\n\t pop %r13\n\t pop %r12\n\t pop %r11\n\t"
        "pop %r10\n\t pop %r9\n\t pop %r8\n\t pop %rbp\n\t pop %rdi\n\t"
        "pop %rsi\n\t pop %rdx\n\t pop %rcx\n\t pop %rbx\n\t pop %rax\n\t"
        "iretq\n\t");
}

__attribute__((naked)) static void smp_spurious_stub(void) {
    __asm__ volatile ("iretq");
}

// Исключение на рабочем ядре: падает только программа (её снимет smp_poll на
// основном ядре), само рабочее возвращается в простой
void smp_ap_exception(uint64_t* frame) {
    smp_cpu_t* c = this_cpu();
    c->fault_vec = (int)frame[15];
    c->fault_rip = frame[17];
    c->fault_pid = c->owner_pid;
    __atomic_store_n(&c->fault, 1, __ATOMIC_SEQ_CST);
    ap_abandon(c);
}

// Системный вызов с рабочего ядра (из заглушки int 0x80, frame как у неё)
uint64_t smp_ap_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t* frame) {
    (void)a3; (void)frame;
    smp_cpu_t* c = this_cpu();
    switch (num) {
        case 0:
        case SYS_WORKER_EXIT:
            ap_abandon(c);
        case SYS_WORKER_ID:
            return (uint64_t)cpu_index(c);
        case SYS_WAIT_U32: {
            if ((a1 & 3) || !vmm_user_range_ok(a1, 4)) return (uint64_t)-1;
            volatile uint32_t* p = (volatile uint32_t*)a1;
            c->wait_addr = a1;
            while (*p == (uint32_t)a2) {
                if (c->stop) ap_abandon(c);
                uint64_t t = rdtsc();
                __asm__ volatile ("sti; hlt; cli" ::: "memory");   // разбудит SYS_WAKE (IPI)
                c->idle_tsc += rdtsc() - t;
            }
            c->wait_addr = 0;
            return 0;
        }
        default:
            return (uint64_t)-1;    // ядро ОС на рабочих ядрах недоступно
    }
}

// Первая C-функция рабочего ядра (из трамплина)
static void build_gdt(smp_cpu_t* c, uint64_t rsp0) {
    c->gdt[0] = 0;
    c->gdt[1] = 0x00209A0000000000ULL;    // 0x08 код ядра
    c->gdt[2] = 0x0000920000000000ULL;    // 0x10 данные ядра
    c->gdt[3] = 0x0020FA0000000000ULL;    // 0x18 код Ring 3
    c->gdt[4] = 0x0000F20000000000ULL;    // 0x20 данные Ring 3
    uint8_t* t = (uint8_t*)&c->tss;
    for (unsigned i = 0; i < sizeof(c->tss); i++) t[i] = 0;
    c->tss.rsp0 = rsp0;
    c->tss.iomap_base = sizeof(c->tss);
    uint64_t base = (uint64_t)&c->tss, lim = sizeof(c->tss) - 1;
    c->gdt[5] = (lim & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
                (((lim >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    c->gdt[6] = base >> 32;
}

void ap_main(int idx) {
    smp_cpu_t* c = &s_cpu[idx];
    uint64_t top = (uint64_t)smp_ap_stacks[idx] + SMP_AP_STACK;
    build_gdt(c, top);
    struct { uint16_t lim; uint64_t base; } __attribute__((packed)) gdtr = { sizeof(c->gdt) - 1, (uint64_t)c->gdt };
    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t mov %%ax, %%es\n\t mov %%ax, %%ss\n\t"
        "xor %%eax, %%eax\n\t mov %%ax, %%fs\n\t mov %%ax, %%gs\n\t"
        "mov $0x28, %%ax\n\t ltr %%ax\n\t"
        : : "m"(gdtr) : "rax", "memory");
    idt_load_this_cpu();
    wrmsr(0x277, s_pat);                  // тот же PAT, что у основного (WC для экрана)

    if (s_x2apic) {
        uint64_t b = rdmsr(0x1B);
        if ((b & (3u << 10)) != (3u << 10)) wrmsr(0x1B, b | (3u << 10));
    }
    lapic_write(0xF0, lapic_read(0xF0) | 0x100 | SMP_SPUR_VEC);   // включить LAPIC
    lapic_write(0x80, 0);                                        // TPR: принимать всё
    lapic_write(0x350, 0x10000);                                 // LINT0/1 этому ядру не нужны
    lapic_write(0x360, 0x10000);
    c->apic_id = lapic_id();
    c->online_tsc = rdtsc();
    c->idle_tsc = 0;
    c->state = CPU_IDLE;
    __atomic_add_fetch(&s_online, 1, __ATOMIC_SEQ_CST);
    ap_idle(idx);
}

// ---------------------------------------------------------------- основное ядро
int smp_online(void) { return s_online; }
const char* smp_status_text(void) { return s_status; }

int smp_init(int expected_threads) {
    if (expected_threads <= 1) { s_status = "single-core CPU"; return 0; }

    uint64_t base = rdmsr(0x1B);
    if (!(base & (1u << 8))) { s_status = "not the bootstrap CPU?"; return 0; }
    s_x2apic = (base & (1u << 10)) ? 1 : 0;
    if (!(base & (1u << 11))) wrmsr(0x1B, base | (1u << 11));      // APIC включён глобально
    uint64_t lapic_phys = base & 0x000FFFFFFFFFF000ULL;
    if (!s_x2apic) {
        if (lapic_phys >= 0x100000000ULL) { s_status = "LAPIC above 4 GiB"; return 0; }
        vmm_set_uc(lapic_phys, 0x1000);                            // регистры, не память
        s_lapic = (volatile uint32_t*)lapic_phys;
    }
    uint64_t kcr3 = (uint64_t)vmm_get_kernel_pml4_phys();
    if (kcr3 >= 0x100000000ULL) { s_status = "kernel PML4 above 4 GiB"; return 0; }

    s_bsp_apic = lapic_id();
    s_pat = rdmsr(0x277);

    // LAPIC основного: включаем программно, если прошивка не включила. Через
    // него уходят IPI. LINT0 (там PIC, виртуальный провод) должен остаться
    // ExtINT; проверяем после, что таймер всё ещё тикает.
    uint32_t svr = lapic_read(0xF0);
    if (!(svr & 0x100)) {
        lapic_write(0xF0, svr | 0x100 | SMP_SPUR_VEC);
        uint32_t l0 = lapic_read(0x350);
        if (l0 & 0x10000) lapic_write(0x350, 0x700);              // ExtINT
        uint32_t l1 = lapic_read(0x360);
        if (l1 & 0x10000) lapic_write(0x360, 0x400);              // NMI
        uint32_t t0 = TICKS;
        sleep_ms(20);
        if (TICKS == t0) {
            lapic_write(0xF0, svr);
            s_status = "LAPIC broke the timer";
            return 0;
        }
    }

    idt_set_gate(SMP_IPI_VEC, smp_ipi_stub, 0);
    idt_set_gate(SMP_SPUR_VEC, smp_spurious_stub, 0);

    // Трамплин на 0x8000 и его параметры
    uint8_t* dst = (uint8_t*)PHYS_TO_VIRT(TRAMPOLINE_PHYS);
    uint64_t len = (uint64_t)(ap_tr_end - ap_tr_start);
    for (uint64_t i = 0; i < len; i++) dst[i] = (uint8_t)ap_tr_start[i];
#define TRF(sym, type) (*(type*)(dst + ((sym) - ap_tr_start)))
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    TRF(ap_tr_cr0, uint32_t) = (uint32_t)cr0;
    TRF(ap_tr_cr3, uint32_t) = (uint32_t)kcr3;
    TRF(ap_tr_cr4, uint32_t) = (uint32_t)cr4 & ~((1u << 7) | (1u << 17));   // без PGE и PCIDE (в PM32 нельзя)
    TRF(ap_tr_efer, uint64_t) = rdmsr(0xC0000080) & ~(1ULL << 10);   // LMA только для чтения
    TRF(ap_tr_stack_base, uint64_t) = (uint64_t)smp_ap_stacks;
    TRF(ap_tr_stack_size, uint64_t) = SMP_AP_STACK;
    TRF(ap_tr_entry, uint64_t) = (uint64_t)ap_main;
    TRF(ap_tr_counter, uint32_t) = 0;
    TRF(ap_tr_max, uint32_t) = SMP_MAX;
    volatile uint32_t* counter = &TRF(ap_tr_counter, uint32_t);
#undef TRF
    __asm__ volatile ("wbinvd" ::: "memory");

    // INIT, затем дважды SIPI на вектор 0x08 (= 0x8000), всем, кроме себя
    lapic_icr(0, 0x000C4500);
    sleep_ms(10);
    lapic_icr(0, 0x000C4600 | (TRAMPOLINE_PHYS >> 12));
    sleep_ms(1);
    if (*counter < (uint32_t)(expected_threads - 1)) lapic_icr(0, 0x000C4600 | (TRAMPOLINE_PHYS >> 12));

    // Ждём, пока все отметятся (до 200 мс)
    int want = expected_threads - 1;
    if (want > SMP_MAX) want = SMP_MAX;
    for (int i = 0; i < 200 && s_online < want; i++) sleep_ms(1);
    sleep_ms(2);
    s_status = s_online ? "ok" : "no core answered";
    return s_online;
}

static void release_one(smp_cpu_t* c) {
    if (cas(&c->state, CPU_ASSIGNED, CPU_IDLE)) {        // ещё не начало — просто забираем
        c->owner_pid = 0;
        c->cr3 = 0;
        __atomic_sub_fetch(&s_running, 1, __ATOMIC_SEQ_CST);
        return;
    }
    if (c->state != CPU_RUNNING) return;
    c->stop = 1;
    ipi(c);
    uint64_t dl = tsc_deadline(200);
    while (c->state == CPU_RUNNING && rdtsc() < dl) cpu_pause();
    if (c->state == CPU_RUNNING) {
        // Ядро не ответило: больше ему ничего не даём и не ждём
        c->state = CPU_BROKEN;
        __atomic_sub_fetch(&s_running, 1, __ATOMIC_SEQ_CST);
        kdebug("[smp] worker core did not stop\n", 0xFF5555);
    }
}

void smp_stop_space(uint64_t cr3) {
    if (!s_running || !cr3) return;
    for (int i = 0; i < SMP_MAX; i++) {
        smp_cpu_t* c = &s_cpu[i];
        if ((c->state == CPU_ASSIGNED || c->state == CPU_RUNNING) && c->cr3 == cr3) release_one(c);
    }
}

void smp_flush_space(uint64_t cr3) {
    if (!s_running || !cr3) return;
    for (int i = 0; i < SMP_MAX; i++) {
        smp_cpu_t* c = &s_cpu[i];
        if (c->state != CPU_RUNNING || c->cr3 != cr3) continue;
        uint32_t want = __atomic_add_fetch(&c->flush_req, 1, __ATOMIC_SEQ_CST);
        ipi(c);
        uint64_t dl = tsc_deadline(50);
        while (c->state == CPU_RUNNING && (int32_t)(c->flush_ack - want) < 0 && rdtsc() < dl) cpu_pause();
    }
}

void smp_poll(void) {
    for (int i = 0; i < SMP_MAX; i++) {
        smp_cpu_t* c = &s_cpu[i];
        if (!c->fault) continue;
        c->fault = 0;
        kdebug("[smp] worker thread crashed, killing its process\n", 0xFF5555);
        if (c->fault_pid > 0) sched_send_signal((uint64_t)c->fault_pid, 11);
    }
}

uint64_t smp_bsp_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, int pid, uint64_t cr3) {
    switch (num) {
        case SYS_SMP_INFO: {
            if (a1) {
                ov_smp_info_t* o = (ov_smp_info_t*)a1;
                int fr = 0;
                uint64_t now = rdtsc();
                o->online = s_online;
                o->bsp_apic_id = s_bsp_apic;
                o->reserved = 0;
                o->tsc_per_ms = g_tsc_per_ms;
                for (int i = 0; i < SMP_MAX; i++) {
                    smp_cpu_t* c = &s_cpu[i];
                    ov_smp_cpu_t* d = &o->cpu[i];
                    int st = c->state == CPU_CLAIMED ? CPU_ASSIGNED : c->state;
                    d->state = st;
                    d->owner_pid = c->owner_pid;
                    d->apic_id = c->apic_id;
                    d->reserved = 0;
                    d->total_tsc = st ? now - c->online_tsc : 0;
                    d->busy_tsc = (st && d->total_tsc > c->idle_tsc) ? d->total_tsc - c->idle_tsc : 0;
                    if (st == CPU_IDLE) fr++;
                }
                o->free = fr;
            }
            return (uint64_t)s_online;
        }
        case SYS_WORKER_START: {
            // Код и стек — в пользовательской половине выше 4 ГБ (ниже identity ядра)
            if (a1 < 0x100000000ULL || a1 >= 0x0000800000000000ULL) return (uint64_t)-1;
            if (a3 < 0x100000000ULL || a3 >= 0x0000800000000000ULL || (a3 & 15)) return (uint64_t)-1;
            for (int i = 0; i < SMP_MAX; i++) {
                smp_cpu_t* c = &s_cpu[i];
                if (c->state != CPU_IDLE || !cas(&c->state, CPU_IDLE, CPU_CLAIMED)) continue;
                c->entry = a1;
                c->arg = a2;
                c->stack = a3 - 8;          // как после call: выравнивание ABI
                c->owner_pid = pid;
                c->cr3 = cr3;
                c->stop = 0;
                c->fault = 0;
                __atomic_add_fetch(&s_running, 1, __ATOMIC_SEQ_CST);
                __atomic_store_n(&c->state, CPU_ASSIGNED, __ATOMIC_SEQ_CST);
                ipi(c);
                return (uint64_t)i;
            }
            return (uint64_t)-2;            // свободных ядер нет
        }
        case SYS_WAIT_U32: {
            // На основном ядре ждём, уступая процессор: здесь живут все остальные
            volatile uint32_t* p = (volatile uint32_t*)a1;
            uint32_t start = TICKS;
            while (*p == (uint32_t)a2) {
                if (a3 && (uint32_t)(TICKS - start) >= (uint32_t)a3) return 1;
                sched_yield();
            }
            return 0;
        }
        case SYS_WAKE:
            for (int i = 0; i < SMP_MAX; i++) {
                smp_cpu_t* c = &s_cpu[i];
                if (c->state == CPU_RUNNING && c->cr3 == cr3 && c->wait_addr) ipi(c);
            }
            return 0;
        case SYS_WORKER_EXIT:               // остановить своего рабочего по номеру
            if (a1 >= SMP_MAX || s_cpu[a1].owner_pid != pid) return (uint64_t)-1;
            release_one(&s_cpu[a1]);
            return 0;
        case SYS_WORKER_ID:
            return (uint64_t)-1;
    }
    return (uint64_t)-1;
}
