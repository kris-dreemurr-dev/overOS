// test_execve.c — проверка sys_execve() (syscall 31) для devOS (Ring 3).
//
// Запускается как TESTEXEC.PRG, печатает своё имя и PID, затем вызывает
// execve("EXECTGT.PRG", ...). Если execve() сработал правильно, этот процесс
// (тот же PID!) должен ПРОДОЛЖИТЬ работу уже как EXECTGT.PRG — со своим
// собственным кодом и стеком, напечатать "I am the NEW image" и выйти с кодом 77.
// Если видишь после execve() хоть одну строку "[TESTEXEC.PRG] ..." — execve
// не сработал (вернулся в старый код вместо замены образа).
//
// ВАЖНО: EXECTGT.PRG должен быть собран и залит на диск ОТДЕЛЬНО (из exec_target.c)
// под именем, которое ты передашь в execve ниже — поправь строку под реальное имя файла.

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

static inline int64_t sys_execve(const char* filename, const char* args) {
    return (int64_t)syscall3(31, (uint64_t)filename, (uint64_t)args, 0);
}

// 1. ТОЧКА ВХОДА ДОЛЖНА БЫТЬ ПЕРВОЙ В ФАЙЛЕ!
int main(void) {
    sys_print("[TESTEXEC.PRG] Before execve()\n");
    sys_print("[TESTEXEC.PRG] If this process is still TESTEXEC.PRG after this point\n");
    sys_print("[TESTEXEC.PRG] in the task manager / any further output - execve() FAILED.\n");

    int64_t r = sys_execve("EXEC.PRG", "");

    // Сюда мы попадаем ТОЛЬКО если execve() не удался (например, файл не найден) —
    // в случае успеха управление уже ушло в exec_target.c и этот код не исполнится.
    sys_print_color("[TESTEXEC.PRG] execve() FAILED, still running old image.\n", 0xFF5555);
    (void)r;
    sys_exit(1);
    return 0;
}