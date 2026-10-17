// test_ctrlc.c — проверка Ctrl+C -> SIGINT (через tty->fg_pid и keyboard.c) для devOS.
//
// Печатает свой PID и уходит в бесконечный цикл — без Ctrl+C завис бы навсегда
// (ровно как ребёнок в test_pkill.c, только тут цель — ТЕКУЩИЙ TTY-процесс,
// а не fork()-ребёнок, и сигнал шлёт клавиатура, а не kill()/pkill()).
//
// Как проверять: запусти testctrlc.prg, посмотри PID в выводе, нажми Ctrl+C.
// Оболочка должна тут же вернуть управление с сообщением "exit code: -2"
// (-SIGINT), а в taskmgr этот процесс должен исчезнуть. Если Ctrl+C не
// сработал — цикл "still alive" будет печататься бесконечно.

#include "ovos.h"   // общие обёртки сисколов (src/lib)

static void u64_to_str(uint64_t v, char* out) {
    char tmp[21];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

static void print_num_line(const char* prefix, uint64_t num, uint32_t color) {
    char buf[64];
    int p = 0;
    while (prefix[p]) { buf[p] = prefix[p]; p++; }
    char ns[21];
    u64_to_str(num, ns);
    int q = 0;
    while (ns[q]) buf[p++] = ns[q++];
    buf[p++] = '\n';
    buf[p] = '\0';
    ov_print_color(buf, color);
}

// Своего PID ни один сисколл пока не отдаёт напрямую — но он не нужен для
// самой проверки Ctrl+C (цель берётся из tty->fg_pid в ядре, не от нас).
// Печатаем просто для ориентира в taskmgr.
int main(void) {
    ov_print_color("[CTRLC.PRG] Entering infinite loop. Press Ctrl+C to kill me.\n", 0x00FF55);
    ov_print_color("[CTRLC.PRG] (check taskmgr in another TTY to see my PID - this program can't query its own PID yet)\n", 0xFFDD55);

    uint64_t i = 0;
    while (1) {
        print_num_line("[CTRLC.PRG] still alive, tick ", i, 0x00FF55);
        i++;
        ov_sc(4, 300, 0, 0);   // sleep(300)
    }

    return 0;
}