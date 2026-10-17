// lltest.prg — проверка разделяемых библиотек .ll (/lib/libc.ll и /lib/libm.ll).
//
// Берёт функции по именам через ov_lib_sym и сверяет результат с эталонами,
// посчитанными на хосте. Так видно и то, что загрузчик правильно разместил и
// перенёс библиотеку, и то, что сама математика считает верно.
//
//     lltest            — все проверки
//     lltest heap       — только куча (malloc/free/realloc в цикле)

#include "ovos.h"
#include "libc/libc_ll.h"
#include "libm/libm_ll.h"

static int s_fail = 0;

static void say(const char* s) { ov_print(s); }

static void num(double v, char* out) {       // короткая печать числа с 6 знаками
    int neg = v < 0;
    if (neg) v = -v;
    uint64_t ip = (uint64_t)v;
    double fr = v - (double)ip;
    char t[24];
    int i = 0, j = 0;
    if (!ip) t[i++] = '0';
    while (ip) { t[i++] = (char)('0' + ip % 10); ip /= 10; }
    if (neg) out[j++] = '-';
    while (i) out[j++] = t[--i];
    out[j++] = '.';
    for (int k = 0; k < 6; k++) { fr *= 10.0; int d = (int)fr; out[j++] = (char)('0' + d); fr -= d; }
    out[j] = '\0';
}

static void check(const char* name, double got, double want, double tol) {
    double d = got - want;
    if (d < 0) d = -d;
    char a[32], b[32];
    num(got, a);
    num(want, b);
    say("  ");
    say(name);
    say(" = ");
    say(a);
    if (d <= tol) {
        ov_print_color("  ok\n", 0x0055FF55);
    } else {
        say("  != ");
        say(b);
        ov_print_color("  ОШИБКА\n", 0x00FF5555);
        s_fail++;
    }
}

static void check_int(const char* name, long got, long want) {
    char a[24], b[24];
    int i = 0;
    long v = got;
    if (!v) a[i++] = '0';
    int neg = v < 0; if (neg) v = -v;
    while (v) { a[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) a[i++] = '-';
    int j = 0; while (i) b[j++] = a[--i]; b[j] = '\0';
    say("  ");
    say(name);
    say(" = ");
    say(b);
    if (got == want) ov_print_color("  ok\n", 0x0055FF55);
    else { ov_print_color("  ОШИБКА\n", 0x00FF5555); s_fail++; }
}

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

int main(void) {
    char arg[64];
    int heap_only = 0;
    if (ov_get_arg(arg, sizeof(arg)) > 0 && arg[0] == 'h') heap_only = 1;

    libc_t c;
    int rc = libc_load(&c);
    if (rc < 0) {
        ov_print_color(rc == -1 ? "Не открылась /lib/libc.ll\n" : "В libc.ll нет нужного символа\n",
                       0x00FF5555);
        ov_exit(1);
    }
    ov_print_color("libc.ll загружена\n", 0x0055FFFF);

    // ---- куча ----
    say("Куча:\n");
    char* p = (char*)c.malloc(1024);
    if (!p) { ov_print_color("  malloc не дал памяти\n", 0x00FF5555); s_fail++; }
    else {
        c.memset(p, 'A', 1024);
        p[1023] = '\0';
        check_int("strlen после memset", (long)c.strlen(p), 1023);
        char* q = (char*)c.realloc(p, 4096);
        check_int("realloc сохранил данные", q && q[0] == 'A' ? 1 : 0, 1);
        c.free(q);
    }
    // Многократный цикл: список свободных блоков должен переиспользоваться
    uint64_t first = 0;
    int reuse = 1;
    for (int i = 0; i < 200; i++) {
        void* a = c.malloc(777);
        if (!a) { reuse = 0; break; }
        if (!first) first = (uint64_t)a;
        else if ((uint64_t)a != first) reuse = 0;
        c.free(a);
    }
    check_int("200 раз malloc/free в тот же блок", reuse, 1);

    if (heap_only) {
        ov_print_color(s_fail ? "\nЕсть ошибки\n" : "\nКуча в порядке\n", s_fail ? 0x00FF5555 : 0x0055FF55);
        libc_unload(&c);
        ov_exit(s_fail ? 1 : 0);
    }

    // ---- строки, числа, сортировка ----
    say("Строки и числа:\n");
    char buf[64];
    c.strcpy(buf, "overOS");
    c.strcat(buf, " 1.0");
    check_int("strcmp(strcat)", c.strcmp(buf, "overOS 1.0"), 0);
    check_int("strstr", c.strstr(buf, "OS") ? 1 : 0, 1);
    check_int("atoi(\"-4095\")", c.atoi("-4095"), -4095);
    check_int("strtol hex", c.strtol("0x1f4", 0, 0), 500);
    c.itoa(48879, buf, 16);
    check_int("itoa(48879,16)", c.strcmp(buf, "beef"), 0);
    c.snprintf(buf, sizeof(buf), "%s=%d/%04x", "n", 42, 255);
    check_int("snprintf", c.strcmp(buf, "n=42/00ff"), 0);

    int arr[9] = { 5, 3, 9, 1, 7, 2, 8, 6, 4 };
    c.qsort(arr, 9, sizeof(int), cmp_int);
    int sorted = 1;
    for (int i = 1; i < 9; i++) if (arr[i - 1] > arr[i]) sorted = 0;
    check_int("qsort", sorted, 1);
    int key = 7;
    int* found = (int*)c.bsearch(&key, arr, 9, sizeof(int), cmp_int);
    check_int("bsearch", found && *found == 7 ? 1 : 0, 1);

    c.srand(12345);
    int r1 = c.rand(), r2 = c.rand();
    check_int("rand даёт разное", r1 != r2 ? 1 : 0, 1);

    // ---- математика ----
    libm_t m;
    rc = libm_load(&m);
    if (rc < 0) {
        ov_print_color(rc == -1 ? "Не открылась /lib/libm.ll\n" : "В libm.ll нет нужного символа\n",
                       0x00FF5555);
        libc_unload(&c);
        ov_exit(1);
    }
    ov_print_color("libm.ll загружена\n", 0x0055FFFF);
    say("Математика (эталоны посчитаны на хосте):\n");

    check("sqrt(2)",        m.sqrt(2.0),            1.4142135623730951, 1e-12);
    check("exp(1)",         m.exp(1.0),             2.718281828459045,  1e-12);
    check("log(10)",        m.log(10.0),            2.302585092994046,  1e-12);
    check("log2(1024)",     m.log2(1024.0),         10.0,               1e-10);
    check("log10(1000)",    m.log10(1000.0),        3.0,                1e-10);
    check("pow(2,10)",      m.pow(2.0, 10.0),       1024.0,             1e-9);
    check("pow(9,0.5)",     m.pow(9.0, 0.5),        3.0,                1e-10);
    check("sin(1)",         m.sin(1.0),             0.8414709848078965, 1e-12);
    check("cos(1)",         m.cos(1.0),             0.5403023058681398, 1e-12);
    check("sin(100)",       m.sin(100.0),          -0.5063656411097588, 1e-10);
    check("tan(0.5)",       m.tan(0.5),             0.5463024898437905, 1e-12);
    check("atan(1)",        m.atan(1.0),            0.7853981633974483, 1e-12);
    check("atan2(1,-1)",    m.atan2(1.0, -1.0),     2.356194490192345,  1e-12);
    check("asin(0.5)",      m.asin(0.5),            0.5235987755982989, 1e-12);
    check("acos(-0.5)",     m.acos(-0.5),           2.0943951023931957, 1e-12);
    check("tanh(0.75)",     m.tanh(0.75),           0.6351489523872873, 1e-12);
    check("hypot(3,4)",     m.hypot(3.0, 4.0),      5.0,                1e-12);
    check("fmod(10,3)",     m.fmod(10.0, 3.0),      1.0,                1e-12);
    check("floor(-2.5)",    m.floor(-2.5),         -3.0,                0.0);
    check("ceil(-2.5)",     m.ceil(-2.5),          -2.0,                0.0);
    check("round(2.5)",     m.round(2.5),           3.0,                0.0);
    check("ldexp(3,10)",    m.ldexp(3.0, 10),       3072.0,             0.0);
    check("pi",             *m.pi,                  3.14159265358979312, 1e-15);

    // Контрольная сумма: интеграл sin от 0 до pi численно равен 2
    double sum = 0.0;
    const int N = 2000;
    for (int i = 0; i < N; i++) sum += m.sin((*m.pi) * (i + 0.5) / N) * (*m.pi) / N;
    check("интеграл sin[0,pi]", sum, 2.0, 1e-6);

    libm_unload(&m);
    libc_unload(&c);

    if (s_fail) {
        ov_print_color("\nОшибок: ", 0x00FF5555);
        char b[16];
        int n = s_fail, i = 0;
        char t[16];
        if (!n) t[i++] = '0';
        while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
        int j = 0; while (i) b[j++] = t[--i]; b[j] = '\0';
        ov_print_color(b, 0x00FF5555);
        ov_print("\n");
        ov_exit(1);
    }
    ov_print_color("\nВсе проверки пройдены\n", 0x0055FF55);
    ov_exit(0);
    return 0;
}
