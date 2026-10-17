// test_execve.c — проверка ov_execve() (syscall 31) для devOS (Ring 3).
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

#include "ovos.h"   // общие обёртки сисколов (src/lib)

// 1. ТОЧКА ВХОДА ДОЛЖНА БЫТЬ ПЕРВОЙ В ФАЙЛЕ!
int main(void) {
    ov_print("[TESTEXEC.PRG] Before execve()\n");
    ov_print("[TESTEXEC.PRG] If this process is still TESTEXEC.PRG after this point\n");
    ov_print("[TESTEXEC.PRG] in the task manager / any further output - execve() FAILED.\n");

    int64_t r = ov_execve("EXEC.PRG", "");

    // Сюда мы попадаем ТОЛЬКО если execve() не удался (например, файл не найден) —
    // в случае успеха управление уже ушло в exec_target.c и этот код не исполнится.
    ov_print_color("[TESTEXEC.PRG] execve() FAILED, still running old image.\n", 0xFF5555);
    (void)r;
    ov_exit(1);
    return 0;
}