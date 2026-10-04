#ifndef PROG_API_H
#define PROG_API_H

#include <stdint.h>

// Единая точка перехода через аппаратный шлюз INT 0x80.
// ВАЖНО: аргументы и возврат — uint64_t. Старая версия использовала uint32_t,
// что обрезало любой 64-битный адрес (куча процесса, PROG_LOAD_BASE = 0x4000000000
// и выше — это уже за пределами 32 бит). Именно поэтому test.c/psx.c/math.c держат
// свою локальную копию syscall3(), а не используют этот файл: эта версия чинит
// ту же обёртку, чтобы прикладной код мог наконец на неё перейти.
static inline uint64_t syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

// -----------------------------------------------------------------------------
// ВЫСОКОУРОВНЕВЫЙ СИ-ИНТЕРФЕЙС ДЛЯ ПРИЛОЖЕНИЙ
// -----------------------------------------------------------------------------

static inline void exit(int code) {
    syscall4(0, (uint64_t)code, 0, 0);
    while (1) { }
}

static inline void print(const char* str) {
    syscall4(1, (uint64_t)(uintptr_t)str, 0, 0);
}

static inline void print_color(const char* str, uint32_t color) {
    syscall4(1, (uint64_t)(uintptr_t)str, color, 0);
}

static inline void print_num(int val) {
    syscall4(7, (uint64_t)(int64_t)val, 0, 0);
}

static inline void put_pixel(int x, int y, uint32_t color) {
    syscall4(2, (uint64_t)x, (uint64_t)y, color);
}

static inline void flush(void) {
    syscall4(3, 0, 0, 0);
}

static inline void sleep(uint32_t ms) {
    syscall4(4, ms, 0, 0);
}

static inline void clear(uint32_t color) {
    syscall4(5, color, 0, 0);
}

static inline uint8_t get_key(void) {
    return (uint8_t)syscall4(6, 0, 0, 0);
}

// Файловые системные вызовы
static inline int open(const char* filename) {
    return (int)syscall4(10, (uint64_t)(uintptr_t)filename, 0, 0);
}

static inline int read(int fd, void* buf, int size) {
    return (int)syscall4(11, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)size);
}

static inline void close(int fd) {
    syscall4(12, (uint64_t)fd, 0, 0);
}

static inline int write(const char* filename, const void* buf, int size) {
    return (int)syscall4(13, (uint64_t)(uintptr_t)filename, (uint64_t)(uintptr_t)buf, (uint64_t)size);
}

// Куча процесса (sys_brk): текущий вызов — (uint64_t)-1 на ошибке,
// иначе возвращает прежнюю (при расширении) или текущую границу кучи.
static inline uint64_t brk(uint64_t new_end) {
    return syscall4(14, new_end, 0, 0);
}

// Графические системные вызовы (320x200, как у Doom/psx.prg/math.prg)
static inline void set_palette(const uint32_t* pal) {
    syscall4(20, (uint64_t)(uintptr_t)pal, 0, 0);
}

static inline void blit_frame(const void* frame_buf) {
    syscall4(21, (uint64_t)(uintptr_t)frame_buf, 0, 0);
}

// Частота TSC (тиков в 1 мс), измеренная ядром при загрузке — нужна для
// самодельного sleep_ms через rdtsc в Ring 3 (см. psx.c/math.c)
static inline uint64_t tsc_per_ms(void) {
    return syscall4(22, 0, 0, 0);
}

// Процессы: Unix-подобные fork()/execve(). wait()/waitpid() пока нет —
// ядро не может дождаться своего fork()-ребёнка из user-mode (см. sched_wait_child,
// он принимает task_t* и доступен только из ядра). Родитель, провожающий ребёнка,
// либо выходит сам (ребёнок осиротеет и продолжит работать), либо нужен отдельный
// sys_waitpid(pid) — следующий шаг плана, если это понадобится.
static inline int64_t fork(void) {
    return (int64_t)syscall4(30, 0, 0, 0);
}

static inline int execve(const char* filename, const char* args) {
    return (int)syscall4(31, (uint64_t)(uintptr_t)filename, (uint64_t)(uintptr_t)args, 0);
}

// Дождаться СВОЕГО ребёнка с данным pid (sched_wait_pid в ядре проверяет
// child->parent == вызывающий, так что чужого ребёнка не заберёшь).
// Возвращает его код выхода, либо -1, если это не твой ребёнок / не найден.
// Дождаться СВОЕГО ребёнка с данным pid. Возвращает 1 и кладёт код выхода
// в *out_status при успехе; 0, если это не твой ребёнок (или уже не существует) —
// *out_status тогда не трогается. Код может быть отрицательным (-signal, если
// ребёнка убил kill()), поэтому success/fail определяется ТОЛЬКО по возврату
// функции, а не по знаку *out_status.
static inline int waitpid(int64_t pid, int* out_status) {
    return (int)syscall4(32, (uint64_t)pid, (uint64_t)(uintptr_t)out_status, 0);
}

// Сигналы — пока только мгновенное завершение цели (SIGINT/SIGKILL не перехватываются,
// обработчиков в системе нет). kill(getpid(), sig) не возвращается — это самозавершение.
#define SIGINT  2
#define SIGKILL 9

static inline int kill(int64_t pid, int sig) {
    return (int)(int64_t)syscall4(33, (uint64_t)pid, (uint64_t)sig, 0);
}

#endif // PROG_API_H