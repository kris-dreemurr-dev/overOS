// exec_target.c — "целевая" программа для теста sys_execve (см. test_execve.c).
// Если execve() сработал правильно, именно ЭТОТ текст и этот код выхода (77)
// увидит оболочка — хотя запускался файл TESTEXEC.PRG, а не EXECTGT.PRG.

#include "ovos.h"   // общие обёртки сисколов (src/lib)

int main(void) {
    ov_print_color("[EXECTGT.PRG] I am the NEW image - execve() replaced the old process.\n", 0x00FF55);
    ov_exit(77);
    return 0;
}