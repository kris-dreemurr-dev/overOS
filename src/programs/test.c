#include "ovos.h"   // общие обёртки сисколов (src/lib)

// 1. ТОЧКА ВХОДА ДОЛЖНА БЫТЬ ПЕРВОЙ В ФАЙЛЕ!
int main(void) {
    ov_print("[TEST.PRG] Key scanner started. Press keys (ESC to exit)...\n");
    ov_print("[TEST.PRG] Exiting...\n");
    ov_exit(0);
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
    ov_print(buf);
}