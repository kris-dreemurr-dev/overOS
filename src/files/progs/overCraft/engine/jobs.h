#ifndef OVENGINE_JOBS_H
#define OVENGINE_JOBS_H
// =============================================================================
// jobs.h — пул потоков движка поверх рабочих ядер overOS.
//
// Каждый рабочий поток живёт на своём ядре всё время работы игры и спит в
// ov_wait_u32, пока основной не объявит новую партию задач (gen++ и ov_wake).
// Партия — count задач одной функции; потоки разбирают номера атомарным
// счётчиком, основной поток считает вместе со всеми, а потом ждёт, пока
// закончат остальные. Так любая параллельная часть кадра — одна функция.
//
// Задачи не выделяют память и не зовут систему (на рабочих ядрах можно
// только ждать), поэтому всё нужное им готовит основной поток заранее.
// =============================================================================

#include <stdint.h>
#include "ovengine.h"

namespace ov {

class JobPool {
public:
    // max_workers < 0 — сколько дадут; 0 — всё на основном потоке
    void start(int max_workers);
    int threads() const { return 1 + (int)workers_; }

    // Выполнить fn(ctx, 0..count-1, thread) на всех потоках и дождаться
    void run(int count, JobFn fn, void* ctx);

    // Время работы потоков в последних партиях (такты rdtsc), для статистики
    uint64_t busy(int thread) const { return busy_[thread]; }
    void reset_busy() { for (int i = 0; i < OE_MAX_THREADS; i++) busy_[i] = 0; }

    static void worker_main(void* arg);

private:
    void work(int thread);

    volatile uint32_t gen_ = 0;         // номер партии: рабочие ждут его смены
    volatile uint32_t next_ = 0;        // следующая свободная задача
    volatile uint32_t done_ = 0;        // сколько рабочих закончили партию
    volatile uint32_t started_ = 0;     // для раздачи номеров потокам
    uint32_t workers_ = 0;
    int count_ = 0;
    JobFn fn_ = nullptr;
    void* ctx_ = nullptr;
    uint64_t busy_[OE_MAX_THREADS] = {};
    uint8_t* stacks_ = nullptr;
};

} // namespace ov

#endif
