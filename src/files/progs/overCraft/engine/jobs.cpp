// =============================================================================
// jobs.cpp — пул потоков движка (см. jobs.h).
// =============================================================================

#include "jobs.h"
#include "ovos.h"

namespace ov {

static const int kStack = 64 * 1024;

struct StartArg {
    JobPool* pool;
};
static StartArg s_arg;

static inline uint64_t tsc() {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void JobPool::start(int max_workers) {
    if (workers_) return;
    ov_smp_info_t info;
    int n = ov_smp_info(&info);
    if (n < 0) n = 0;
    if (n > info.free) n = info.free;
    if (max_workers >= 0 && n > max_workers) n = max_workers;
    if (n > OE_MAX_THREADS - 1) n = OE_MAX_THREADS - 1;
    if (n <= 0) return;

    stacks_ = new uint8_t[(size_t)n * kStack + 16];
    s_arg.pool = this;
    for (int i = 0; i < n; i++) {
        uint8_t* top = stacks_ + (size_t)(i + 1) * kStack;
        if (ov_worker_start(&JobPool::worker_main, &s_arg, top) < 0) break;
        workers_++;
    }
}

// Поток на рабочем ядре: номер берёт сам, дальше — ждать партию и работать
void JobPool::worker_main(void* arg) {
    JobPool* p = ((StartArg*)arg)->pool;
    int me = (int)__atomic_add_fetch(&p->started_, 1, __ATOMIC_SEQ_CST);   // 1..workers
    uint32_t seen = 0;
    for (;;) {
        while (p->gen_ == seen) ov_wait_u32(&p->gen_, seen, 0);
        seen = p->gen_;
        p->work(me);
        __atomic_add_fetch(&p->done_, 1, __ATOMIC_SEQ_CST);
    }
}

void JobPool::work(int thread) {
    uint64_t t0 = tsc();
    for (;;) {
        uint32_t i = __atomic_fetch_add(&next_, 1, __ATOMIC_SEQ_CST);
        if ((int)i >= count_) break;
        fn_(ctx_, (int)i, thread);
    }
    busy_[thread] += tsc() - t0;
}

void JobPool::run(int count, JobFn fn, void* ctx) {
    if (count <= 0) return;
    if (!workers_ || count == 1) {               // нечего делить
        uint64_t t0 = tsc();
        for (int i = 0; i < count; i++) fn(ctx, i, 0);
        busy_[0] += tsc() - t0;
        return;
    }
    fn_ = fn;
    ctx_ = ctx;
    count_ = count;
    next_ = 0;
    done_ = 0;
    __atomic_add_fetch(&gen_, 1, __ATOMIC_SEQ_CST);
    ov_wake(&gen_);
    work(0);
    // Основной закончил свою долю: ждём остальных. Сначала коротко крутимся
    // (партии в кадре короткие), потом уступаем процессор системе.
    for (int spin = 0; done_ != workers_ && spin < 20000; spin++) __asm__ volatile ("pause");
    uint32_t d;
    while ((d = done_) != workers_) ov_wait_u32(&done_, d, 1);
}

} // namespace ov
