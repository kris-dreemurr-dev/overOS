#include <stdint.h>

// Универсальная обёртка системного вызова:
static inline uint64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline void sys_print(const char* str) {
    syscall3(1, (uint64_t)str, 0x00FFFFFF, 0);
}

static inline uint8_t sys_get_key(void) {
    return (uint8_t)syscall3(6, 0, 0, 0);
}

static inline void sys_exit(int code) {
    syscall3(0, (uint64_t)code, 0, 0);
    while (1);
}

// 1. ТОЧКА ВХОДА ДОЛЖНА БЫТЬ ПЕРВОЙ В ФАЙЛЕ!
int main(void) {
    sys_print("[TEST.PRG] Key scanner started. Press keys (ESC to exit)...\n");
    sys_print("[TEST.PRG] Exiting...\n");
    sys_exit(0);
    return 0;
}

// 2. Вспомогательные функции ниже main
void print_hex(uint8_t val) {
    char buf[16];
    char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    buf[2] = hex[(val >> 4) & 0xF];
    buf[3] = hex[val & 0xF];
    buf[4] = '\n';
    buf[5] = '\0';
    sys_print(buf);
}