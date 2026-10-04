// test_fork.c — проверка sys_fork() + sys_waitpid() (syscall 30 / 32) для devOS (Ring 3).
// Родитель печатает свой PID и PID ребёнка, несколько тактов работает ПАРАЛЛЕЛЬНО
// с ребёнком (round-robin между двумя разными задачами планировщика — их вывод может
// перемежаться), затем зовёт waitpid(child_pid, &status) и печатает полученный код
// выхода. sys_waitpid возвращает 1/0 (нашли/не нашли СВОЕГО ребёнка) отдельно от
// самого кода выхода — код может быть отрицательным (если ребёнка убили kill()),
// и это не путается с "ребёнок не найден", как было бы при одном общем числе.
// После waitpid ребёнок должен полностью исчезнуть из taskmgr (sched_dump_tasks
// в ядре печатает таблицу задач сразу после fork() и сразу после waitpid()).

#include <stdint.h>

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                      : "memory");
    return ret;
}

static inline void sys_print(const char* str) {
    syscall3(1, (uint64_t)str, 0x00FFFFFF, 0);
}

static inline void sys_print_color(const char* str, uint32_t color) {
    syscall3(1, (uint64_t)str, color, 0);
}

static inline void sys_exit(int code) {
    syscall3(0, (uint64_t)code, 0, 0);
    while (1) { }
}

static inline uint64_t sys_fork(void) {
    return syscall3(30, 0, 0, 0);
}

// Возвращает 1, если собрали СВОЕГО ребёнка (его код выхода — в *out_status); 0 иначе.
static inline int sys_waitpid(uint64_t pid, int* out_status) {
    return (int)syscall3(32, pid, (uint64_t)(uintptr_t)out_status, 0);
}

// itoa для чисел без знака (без libc)
static void u64_to_str(uint64_t v, char* out) {
    char tmp[21];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

// то же самое, но для знаковых (код выхода может быть -1)
static void i64_to_str(int64_t v, char* out) {
    if (v < 0) { out[0] = '-'; u64_to_str((uint64_t)(-v), out + 1); return; }
    u64_to_str((uint64_t)v, out);
}

static void print_num_line(const char* prefix, int64_t num, uint32_t color) {
    char buf[96];
    int p = 0;
    while (prefix[p]) { buf[p] = prefix[p]; p++; }
    char ns[21];
    i64_to_str(num, ns);
    int q = 0;
    while (ns[q]) buf[p++] = ns[q++];
    buf[p++] = '\n';
    buf[p] = '\0';
    sys_print_color(buf, color);
}

// 1. ТОЧКА ВХОДА ДОЛЖНА БЫТЬ ПЕРВОЙ В ФАЙЛЕ!
int main(void) {
    sys_print("[FORK.PRG] Before fork()\n");

    uint64_t ret = sys_fork();

    if (ret == 0) {
        // --- Ветка ребёнка ---
        // Если мы вообще сюда попали с правильными регистрами и стеком Ring 3 —
        // восстановление кадра прерывания в sys_fork отработало верно.
        sys_print_color("[FORK.PRG] CHILD: fork() returned 0, as expected.\n", 0x00FF55);

        for (int i = 0; i < 5; i++) {
            print_num_line("[FORK.PRG] CHILD tick ", i, 0x00FF55);
            syscall3(4, 50, 0, 0);   // sleep(50)
        }

        sys_print_color("[FORK.PRG] CHILD exiting with code 42\n", 0x00FF55);
        sys_exit(42);
    } else if ((int64_t)ret > 0) {
        // --- Ветка родителя ---
        uint64_t child_pid = ret;
        print_num_line("[FORK.PRG] PARENT: child pid = ", (int64_t)child_pid, 0xFFDD55);

        // Пока ребёнок работает сам по себе (отдельная задача планировщика),
        // родитель тоже делает несколько тактов — вывод может перемежаться.
        for (int i = 0; i < 3; i++) {
            sys_print_color("[FORK.PRG] PARENT tick\n", 0xFFDD55);
            syscall3(4, 50, 0, 0);   // sleep(50)
        }

        sys_print_color("[FORK.PRG] PARENT: waiting for child...\n", 0xFFDD55);
        int status = 0;
        int reaped = sys_waitpid(child_pid, &status);

        if (reaped) {
            print_num_line("[FORK.PRG] PARENT: child reaped, exit code = ", status, 0x55FF55);
        } else {
            sys_print_color("[FORK.PRG] PARENT: waitpid() FAILED (not our child?)\n", 0xFF5555);
        }

        sys_print_color("[FORK.PRG] PARENT exiting with code 0\n", 0xFFDD55);
        sys_exit(0);
    } else {
        // --- fork() вернул -1: ошибка (нет памяти и т.п.) ---
        sys_print_color("[FORK.PRG] fork() FAILED (returned -1)\n", 0xFF5555);
        sys_exit(1);
    }

    return 0;
}