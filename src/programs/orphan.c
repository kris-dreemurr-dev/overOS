// orphan.c — проверка сбора сирот (процесс, чей родитель умер раньше него).
//
// fork(): родитель сразу выходит, ребёнок живёт ещё пару секунд. По UNIX ребёнок
// должен достаться init (в overOS это kernel_main), а тот обязан собрать его зомби.
// Раньше parent указывал в освобождённую страницу, и такой зомби висел вечно.
//
// Как проверять: запусти orphan.prg, подожди 3 секунды и посмотри ps/taskmgr —
// ни "orphan", ни зомби в таблице задач остаться не должно.

#include "ovos.h"   // общие обёртки сисколов (src/lib)

static void u64_to_str(uint64_t v, char* out) {
    char tmp[21];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (i) out[j++] = tmp[--i];
    out[j] = '\0';
}

int main(void) {
    char buf[24];
    uint64_t child = ov_fork();

    if (child == 0) {
        u64_to_str(ov_getpid(), buf);
        ov_print_color("orphan: ребёнок PID ", 0x00FFFF55);
        ov_print_color(buf, 0x00FFFF55);
        ov_print_color(" живёт без родителя\n", 0x00FFFF55);
        ov_sleep(2000);
        ov_print_color("orphan: ребёнок завершился, его должен собрать init\n", 0x0055FF55);
        ov_exit(0);
    }

    ov_print_color("orphan: родитель выходит сразу, не дожидаясь ребёнка\n", 0x00FFFFFF);
    ov_exit(0);
    return 0;
}
