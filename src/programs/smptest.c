// smptest.prg — проверка рабочих ядер (smp.c).
//
//   smptest        — сколько ядер, одна и та же работа на одном и на всех,
//                    затем 200 раундов «разбудить — посчитать — уснуть»
//   smptest spin   — рабочие крутятся без конца, а программа выходит: ядро ОС
//                    обязано снять их раньше, чем освободит память программы
//   smptest crash  — рабочий разыменует NULL: должна упасть программа, а не
//                    система, и ядро должно вернуться в строй
//
// Работа — счёт простых чисел в диапазоне: чистая арифметика, без памяти и
// системных вызовов, ровно то, для чего рабочие ядра и задуманы.

#include "ovos.h"

#define STACK 65536
#define RANGE 600000u

static uint8_t s_stacks[OV_SMP_MAX][STACK] __attribute__((aligned(16)));

typedef struct {
    volatile uint32_t gen;        // номер раунда: рабочие ждут, пока он не сменится
    volatile uint32_t done;       // сколько рабочих закончили раунд
    volatile uint32_t quit;
    uint32_t workers;
    uint32_t parts;               // на сколько кусков делим работу (рабочие + основной)
    volatile uint32_t result[OV_SMP_MAX + 1];
} pool_t;

static pool_t s_pool;

static void out(const char* s) { ov_sc(1, (uint64_t)s, 0xD0D0D0, 0); }
static void num(uint64_t v) {
    char b[24]; int i = 23; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    out(b + i);
}

static uint32_t count_primes(uint32_t lo, uint32_t hi) {
    uint32_t n = 0;
    for (uint32_t x = lo < 2 ? 2 : lo; x < hi; x++) {
        int p = 1;
        for (uint32_t d = 2; d * d <= x; d++) if (x % d == 0) { p = 0; break; }
        n += (uint32_t)p;
    }
    return n;
}

static uint32_t part(uint32_t i, uint32_t parts) {
    // кусок i из parts поровну по числам (верхние чуть дороже — для теста неважно)
    return (uint32_t)((uint64_t)RANGE * i / parts);
}

static void worker(void* arg) {
    uint32_t me = (uint32_t)(uint64_t)arg;           // 1..workers (0 — основной)
    uint32_t seen = 0;
    for (;;) {
        while (s_pool.gen == seen) ov_wait_u32(&s_pool.gen, seen, 0);
        seen = s_pool.gen;
        if (s_pool.quit) break;
        uint32_t p = s_pool.parts;
        s_pool.result[me] = count_primes(part(me, p), part(me + 1, p));
        __atomic_add_fetch(&s_pool.done, 1, __ATOMIC_SEQ_CST);
    }
    __atomic_add_fetch(&s_pool.done, 1, __ATOMIC_SEQ_CST);
}

static void spin_worker(void* arg) {
    volatile uint64_t* x = (volatile uint64_t*)arg;
    for (;;) (*x)++;
}

static void crash_worker(void* arg) {
    (void)arg;
    volatile uint32_t* p = 0;
    *p = 1;
}

// Один раунд на всех: основной поток тоже считает свой кусок
static uint32_t round_all(void) {
    s_pool.done = 0;
    __atomic_add_fetch(&s_pool.gen, 1, __ATOMIC_SEQ_CST);
    ov_wake(&s_pool.gen);
    s_pool.result[0] = count_primes(part(0, s_pool.parts), part(1, s_pool.parts));
    uint32_t d;
    while ((d = s_pool.done) != s_pool.workers) ov_wait_u32(&s_pool.done, d, 5);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < s_pool.parts; i++) sum += s_pool.result[i];
    return sum;
}

int main(void) {
    char arg[32];
    int al = ov_get_arg(arg, sizeof(arg));
    int crash = (al > 0 && arg[0] == 'c');
    int spin = (al > 0 && arg[0] == 's');

    ov_smp_info_t info;
    int n = ov_smp_info(&info);
    out("Рабочих ядер: "); num((uint64_t)(n > 0 ? n : 0)); out(" (свободно "); num((uint64_t)info.free); out(")\n");
    if (n <= 0) { out("Рабочих ядер нет: всё считается на одном.\n"); return 0; }

    if (crash) {
        int c = ov_worker_start(crash_worker, 0, s_stacks[0] + STACK);
        out("Запущен падающий рабочий на ядре "); num((uint64_t)c); out(". Сейчас программу должны снять.\n");
        ov_sleep(2000);
        out("ОШИБКА: программа всё ещё жива\n");
        return 1;
    }

    if (spin) {
        static volatile uint64_t cnt[OV_SMP_MAX];
        for (int i = 0; i < n && i < OV_SMP_MAX; i++) ov_worker_start(spin_worker, (void*)&cnt[i], s_stacks[i] + STACK);
        ov_sleep(300);
        out("Крутятся, счётчик ядра 0: "); num(cnt[0]); out(". Выходим, не останавливая их.\n");
        return 0;
    }

    uint64_t k = ov_tsc_per_ms();
    // 1. Всё на одном ядре
    uint64_t t0 = ov_rdtsc();
    uint32_t ref = count_primes(0, RANGE);
    uint64_t t1 = ov_rdtsc();
    out("Одно ядро:  "); num(ref); out(" простых, "); num((t1 - t0) / k); out(" мс\n");

    // 2. Рабочие
    if (n > OV_SMP_MAX) n = OV_SMP_MAX;
    s_pool.workers = 0;
    s_pool.parts = (uint32_t)n + 1;
    for (int i = 0; i < n; i++) {
        int c = ov_worker_start(worker, (void*)(uint64_t)(i + 1), s_stacks[i] + STACK);
        if (c < 0) break;
        s_pool.workers++;
    }
    s_pool.parts = s_pool.workers + 1;
    out("Запущено рабочих: "); num(s_pool.workers); out("\n");

    t0 = ov_rdtsc();
    uint32_t got = round_all();
    t1 = ov_rdtsc();
    out("Все ядра:   "); num(got); out(" простых, "); num((t1 - t0) / k); out(" мс");
    out(got == ref ? "  — совпало\n" : "  — НЕ СОВПАЛО\n");

    // 3. Много коротких раундов: проверка сна и пробуждения
    int bad = 0;
    t0 = ov_rdtsc();
    for (int r = 0; r < 200; r++) if (round_all() != ref) bad++;
    t1 = ov_rdtsc();
    out("200 раундов: "); num((t1 - t0) / k); out(" мс, ошибок "); num((uint64_t)bad); out("\n");

    s_pool.quit = 1;
    s_pool.done = 0;
    __atomic_add_fetch(&s_pool.gen, 1, __ATOMIC_SEQ_CST);
    ov_wake(&s_pool.gen);
    uint32_t d;
    while ((d = s_pool.done) != s_pool.workers) ov_wait_u32(&s_pool.done, d, 5);
    ov_smp_info(&info);
    out("Рабочие вышли, свободно ядер: "); num((uint64_t)info.free); out("\n");
    return bad ? 1 : 0;
}
