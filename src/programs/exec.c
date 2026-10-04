// exec_target.c — "целевая" программа для теста sys_execve (см. test_execve.c).
// Если execve() сработал правильно, именно ЭТОТ текст и этот код выхода (77)
// увидит оболочка — хотя запускался файл TESTEXEC.PRG, а не EXECTGT.PRG.

#include <stdint.h>

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                      : "memory");
    return ret;
}

static inline void sys_print_color(const char* str, uint32_t color) {
    syscall3(1, (uint64_t)str, color, 0);
}

static inline void sys_exit(int code) {
    syscall3(0, (uint64_t)code, 0, 0);
    while (1) { }
}

int main(void) {
    sys_print_color("[EXECTGT.PRG] I am the NEW image - execve() replaced the old process.\n", 0x00FF55);
    sys_exit(77);
    return 0;
}