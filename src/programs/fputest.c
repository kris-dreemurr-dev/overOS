// fputest.prg — проверка того, что ядро сохраняет регистры SSE при переключении задач.
//
// До появления fxsave/fxrstor в switch_to регистры xmm были общими на всю
// систему: любая задача, считавшая во float (DOOM, Quake), получала чужие
// значения после переключения. Тест ловит это напрямую.
//
// Что делает: ветвится на два процесса, каждый кладёт в xmm0..xmm7 свой узор,
// уходит в сон (там гарантированно происходит переключение задач) и проверяет,
// что узор вернулся целым. Если ядро не сохраняет состояние, процессы затирают
// регистры друг другу и тест это печатает.

#include "ovos.h"

#define ROUNDS 300

static void xmm_set(uint64_t v) {
    __asm__ volatile(
        "movq %0, %%xmm0\n\t movq %0, %%xmm1\n\t movq %0, %%xmm2\n\t movq %0, %%xmm3\n\t"
        "movq %0, %%xmm4\n\t movq %0, %%xmm5\n\t movq %0, %%xmm6\n\t movq %0, %%xmm7"
        :: "r"(v)
        : "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7");
}

// Возвращает номер первого испорченного регистра (1..8) или 0, если всё цело
static int xmm_check(uint64_t v) {
    uint64_t got[8];
    __asm__ volatile(
        "movq %%xmm0, %0\n\t movq %%xmm1, %1\n\t movq %%xmm2, %2\n\t movq %%xmm3, %3\n\t"
        "movq %%xmm4, %4\n\t movq %%xmm5, %5\n\t movq %%xmm6, %6\n\t movq %%xmm7, %7"
        : "=m"(got[0]), "=m"(got[1]), "=m"(got[2]), "=m"(got[3]),
          "=m"(got[4]), "=m"(got[5]), "=m"(got[6]), "=m"(got[7]));
    for (int i = 0; i < 8; i++) if (got[i] != v) return i + 1;
    return 0;
}

static void print_num(const char* label, int v) {
    char b[24];
    int i = 0, n = v;
    if (n == 0) b[i++] = '0';
    while (n > 0) { b[i++] = (char)('0' + (n % 10)); n /= 10; }
    char out[32];
    int p = 0;
    while (label[p]) { out[p] = label[p]; p++; }
    int j = p;
    while (i > 0) out[j++] = b[--i];
    out[j] = '\0';
    ov_print(out);
}

static int run_rounds(uint64_t pattern) {
    int bad = 0;
    for (int r = 0; r < ROUNDS; r++) {
        xmm_set(pattern);
        ov_sleep(2);                      // здесь планировщик уводит процессор другой задаче
        int who = xmm_check(pattern);
        if (who) bad++;
    }
    return bad;
}

int main(void) {
    ov_print("fputest: два процесса крутят свои узоры в xmm0..xmm7\n");

    int pid = ov_fork();
    if (pid < 0) {
        ov_print_color("fputest: fork не удался\n", 0xFF5555);
        ov_exit(1);
    }

    if (pid == 0) {
        int bad = run_rounds(0xC0FFEE00C0FFEE00ULL);
        if (bad) { print_num("fputest: потомок, испорчено раундов: ", bad); ov_print("\n"); }
        ov_exit(bad ? 1 : 0);
    }

    int bad = run_rounds(0x1234567812345678ULL);
    int status = 0;
    while (ov_waitpid(pid, &status, 0) <= 0) ov_sleep(5);

    if (bad) { print_num("fputest: родитель, испорчено раундов: ", bad); ov_print("\n"); }

    if (bad == 0 && status == 0) {
        ov_print_color("fputest: ОК - регистры SSE переживают переключение задач\n", 0x55FF55);
        ov_exit(0);
    }
    ov_print_color("fputest: ОШИБКА - состояние SSE теряется при переключении\n", 0xFF5555);
    ov_exit(1);
    return 0;
}
