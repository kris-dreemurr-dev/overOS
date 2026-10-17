// heaptest.c — проверка sys_brk на СЖАТИЕ кучи (syscall 14).
//
// Раньше brk умел только расти: страницы, отданные обратно (brk вниз), оставались
// за процессом, и память к системе не возвращалась. Тест 200 раз подряд берёт
// по 4 МБ и тут же отдаёт их. Суммарно это 800 МБ — больше, чем свободно в системе,
// поэтому без возврата страниц цикл обязан сорваться на "нет памяти".
//
// Как проверять: heaptest.prg печатает число успешных циклов. 200 — память
// возвращается; меньше — утечка вернулась.

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

#define CHUNK (4u * 1024u * 1024u)
#define ROUNDS 400

int main(void) {
    char buf[24];
    uint64_t base = ov_brk(0);
    int ok = 0;

    for (int r = 0; r < ROUNDS; r++) {
        uint64_t top = ov_brk(base + CHUNK);
        if (top == (uint64_t)-1) break;

        // Трогаем начало и конец, чтобы страницы точно были выделены
        volatile uint8_t* p = (volatile uint8_t*)base;
        p[0] = (uint8_t)r;
        p[CHUNK - 1] = (uint8_t)r;
        if (p[0] != (uint8_t)r || p[CHUNK - 1] != (uint8_t)r) break;

        if (ov_brk(base) == (uint64_t)-1) break;   // отдаём обратно
        ok++;
    }

    ov_print_color("heaptest: успешных циклов по 4 МБ: ", 0x00FFFFFF);
    u64_to_str((uint64_t)ok, buf);
    ov_print_color(buf, (ok == ROUNDS) ? 0x0055FF55 : 0x00FF5555);
    ov_print_color(ok == ROUNDS ? " из 400 - память возвращается\n" : " из 400 - память НЕ возвращается\n",
                    (ok == ROUNDS) ? 0x0055FF55 : 0x00FF5555);
    ov_exit(ok == ROUNDS ? 0 : 1);
    return 0;
}
