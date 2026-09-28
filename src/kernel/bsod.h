#ifndef BSOD_H
#define BSOD_H

#include <stdint.h>

typedef struct {
    uint64_t saved_esp;
    uint64_t saved_ebp;
    uint64_t is_running_jit;
} jit_crash_guard_t;

extern volatile jit_crash_guard_t g_jit_guard;

// 64-битная расширенная версия BSOD (R8-R15, CR4, EFER, Code Dump, Stack Dump)
void show_bsod(const char* reason, uint32_t error_code, int exc_no, uint64_t fault_eip, uint64_t fault_esp);

void handle_cpu_exception(int exc_no, uint64_t fault_eip, uint64_t fault_esp);

#endif // BSOD_H