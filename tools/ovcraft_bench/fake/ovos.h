// Заглушка ovos.h для замеров движка на основной машине (без overOS)
#pragma once
#include <stdint.h>
#include <stddef.h>
#define OV_SMP_MAX 16
typedef struct { int32_t state, owner_pid; uint32_t apic_id, reserved; uint64_t busy_tsc, total_tsc; } ov_smp_cpu_t;
typedef struct { int32_t online, free; uint32_t bsp_apic_id, reserved; uint64_t tsc_per_ms; ov_smp_cpu_t cpu[OV_SMP_MAX]; } ov_smp_info_t;
static inline int ov_smp_info(ov_smp_info_t* o) { if (o) { o->online = 0; o->free = 0; } return 0; }
static inline int ov_worker_start(void (*)(void*), void*, void*) { return -1; }
static inline int ov_wait_u32(volatile uint32_t*, uint32_t, uint32_t) { return 0; }
static inline void ov_wake(volatile uint32_t*) {}
extern uint64_t bench_tsc_per_ms;
static inline uint64_t ov_tsc_per_ms(void) { return bench_tsc_per_ms; }

// Файлы — через POSIX (модели .mdl на стенде)
#include <fcntl.h>
#include <unistd.h>
static inline int ov_open(const char* p, int) { return open(p, O_RDONLY); }
static inline int64_t ov_read(int fd, void* b, uint64_t n) { return read(fd, b, n); }
static inline int ov_close(int fd) { return close(fd); }
static inline size_t ov_strlcpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) d[i] = s[i];
    if (n) d[i] = 0;
    while (s[i]) i++;
    return i;
}
#ifdef __cplusplus
extern "C" void ov_exit(int code);     // memsim.cpp: cxxrt зовёт его при нехватке памяти
#endif
