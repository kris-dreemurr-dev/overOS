// test_pkill.c — проверка sys_kill()/SIGKILL (syscall 33) для devOS (Ring 3).
//
// Родитель форкается, ребёнок уходит в бесконечный цикл (без kill() завис бы
// навсегда — нет собственного способа выйти). Родитель ждёт немного, затем
// убивает ребёнка через kill(pid, SIGKILL) и проверяет через waitpid(), что
// код выхода пришёл отрицательным (-SIGKILL = -9), как и положено по Unix-
// соглашению WIFSIGNALED.
//
// Этим же ребёнком (отдельным запуском) можно проверить и команду шелла
// "pkill <pid>" вручную: запусти testpkill.prg, посмотри PID ребёнка
// в выводе и в parallel-окне (или после) набери "pkill <pid>" из оболочки —
// родитель должен тут же проснуться в waitpid() и напечатать -9.

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

// Возвращает 1, если собрали СВОЕГО ребёнка (код выхода — в *out_status); 0 иначе.
static inline int sys_waitpid(uint64_t pid, int* out_status) {
    return (int)syscall3(32, pid, (uint64_t)(uintptr_t)out_status, 0);
}

static inline int64_t sys_kill(uint64_t pid, int sig) {
    return (int64_t)syscall3(33, pid, (uint64_t)sig, 0);
}

#define SIGKILL 9

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
    sys_print("[PKILL.PRG] Before fork()\n");

    uint64_t ret = sys_fork();

    if (ret == 0) {
        // --- Ветка ребёнка: бесконечный цикл без собственного способа выйти ---
        // Если kill() не работает, этот процесс зависнет навсегда (и останется READY
        // в taskmgr до перезагрузки). Это и есть сам тест.
        sys_print_color("[PKILL.PRG] CHILD: entering infinite loop, waiting to be killed...\n", 0x00FF55);
        int i = 0;
        while (1) {
            print_num_line("[PKILL.PRG] CHILD still alive, tick ", i, 0x00FF55);
            i++;
            syscall3(4, 200, 0, 0);   // sleep(200)
        }
        // недостижимо
    } else if ((int64_t)ret > 0) {
        // --- Ветка родителя ---
        uint64_t child_pid = ret;
        print_num_line("[PKILL.PRG] PARENT: child pid = ", (int64_t)child_pid, 0xFFDD55);

        sys_print_color("[PKILL.PRG] PARENT: letting child run for a bit...\n", 0xFFDD55);
        for (int i = 0; i < 3; i++) {
            syscall3(4, 200, 0, 0);   // sleep(200) — даём ребёнку поработать и напечатать пару тиков
        }

        print_num_line("[PKILL.PRG] PARENT: sending SIGKILL to PID ", (int64_t)child_pid, 0xFFDD55);
        int64_t kr = sys_kill(child_pid, SIGKILL);
        if (kr != 0) {
            sys_print_color("[PKILL.PRG] kill() FAILED — PID not found?\n", 0xFF5555);
            sys_exit(1);
        }

        sys_print_color("[PKILL.PRG] PARENT: waiting for child to be reaped...\n", 0xFFDD55);
        int status = 0;
        int reaped = sys_waitpid(child_pid, &status);

        if (!reaped) {
            sys_print_color("[PKILL.PRG] waitpid() FAILED — not our child?\n", 0xFF5555);
            sys_exit(1);
        }

        print_num_line("[PKILL.PRG] PARENT: child reaped, exit code = ", status, 0x55FF55);
        if (status == -SIGKILL) {
            sys_print_color("[PKILL.PRG] OK: exit code == -SIGKILL, as expected.\n", 0x55FF55);
        } else {
            sys_print_color("[PKILL.PRG] MISMATCH: expected exit code == -9.\n", 0xFF5555);
        }

        sys_print_color("[PKILL.PRG] PARENT exiting with code 0\n", 0xFFDD55);
        sys_exit(0);
    } else {
        sys_print_color("[PKILL.PRG] fork() FAILED (returned -1)\n", 0xFF5555);
        sys_exit(1);
    }

    return 0;
}