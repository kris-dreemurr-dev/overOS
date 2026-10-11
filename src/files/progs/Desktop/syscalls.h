#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
// Структуры мыши, времени и видео
// -----------------------------------------------------------------------------
typedef struct {
    int x, y;
    uint8_t buttons;
    int last_dx, last_dy;
} __attribute__((packed)) user_mouse_t;

typedef struct {
    uint8_t sec, min, hour;
    uint8_t day, month, year;
} __attribute__((packed)) rtc_time_t;

typedef struct {
    uint32_t width, height, pitch, bpp;
    uint64_t paddr;
} fb_var_info_t;

typedef struct {
    char name[32];
    char arch[16];
    char version[32];
} __attribute__((packed)) kernel_info_t;

// -----------------------------------------------------------------------------
// Протокол оконного сервера (Window Manager ABI)
// -----------------------------------------------------------------------------
#define MAX_SYS_WINDOWS  16
#define DEFAULT_WIN_SIZE 320

#define WEV_MOUSE_DOWN 1
#define WEV_MOUSE_UP   2
#define WEV_KEY        3

#define NET_STATE_DISCONNECTED 0
#define NET_STATE_LOCAL_ONLY   1
#define NET_STATE_ONLINE       2


typedef struct {
    const char*     title;
    int             w;
    int             h;
    const uint16_t* icon;
} __attribute__((packed)) win_desc_t;

typedef struct {
    int      id;
    int      pid;
    char     title[64];
    int      x, y;
    int      w, h;
    int      has_icon;
    uint16_t icon[16];
    int      is_minimized;
    int      is_active;
} __attribute__((packed)) kernel_window_t;

typedef struct {
    char     name[32];      // Имя файла
    uint8_t  attr;          // 0x10 = папка, 0x20 = файл
    uint32_t size;          // Размер в байтах
    uint32_t cluster;       // Стартовый кластер
    char     clean_name[64];
} __attribute__((packed)) sys_file_info_t;

typedef struct {
    int type;        // WEV_*
    int x, y;        // координаты внутри клиентской области окна
    int buttons;
    int key;         // скан-код для WEV_KEY
} win_event_t;

typedef struct {
    uint32_t state;       // NET_STATE_*
    uint32_t ip;
    uint32_t gateway_ip;
    uint16_t link_speed;  // 10, 100, 1000 Mbps
} __attribute__((packed)) sys_net_info_t;

// -----------------------------------------------------------------------------
// Системные вызовы ядра overOS (int 0x80)
// -----------------------------------------------------------------------------

static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(0), "b"(code) : "memory");
    while (1);
}

// Ядро кладёт результат сисколла в RAX: без "+a" компилятор считал бы, что RAX остался равен
// номеру вызова, и при повторном sys_sleep в цикле мог бы отправить уже другое число (например, exit).
static inline void sys_sleep(uint32_t ms) {
    uint64_t r = 4;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(ms) : "memory");
}

static inline uint8_t sys_get_key(void) {
    uint64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(6) : "memory");
    return (uint8_t)ret;
}

static inline int sys_get_mouse(user_mouse_t* ms) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(7), "b"(ms) : "memory");
    return ret;
}

static inline int sys_get_time(rtc_time_t* t) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(8), "b"(t) : "memory");
    return ret;
}

static inline int sys_get_kernel_info(kernel_info_t* info) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(9), "b"(info) : "memory");
    return ret;
}

static inline int sys_open(const char* fn, int flags) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(10), "b"(fn), "c"(flags) : "memory");
    return ret;
}

static inline int sys_read(int fd, void* buf, int sz) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(11), "b"(fd), "c"(buf), "d"(sz) : "memory");
    return ret;
}

static inline void sys_close(int fd) {
    uint64_t r = 12;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(fd) : "memory");
}

static inline int sys_write(int fd, const void* buf, uint32_t sz) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(13), "b"(fd), "c"(buf), "d"(sz) : "memory");
    return ret;
}

static inline void* sys_sbrk(int64_t inc) {
    uint64_t cur = 0;
    __asm__ volatile ("int $0x80" : "=a"(cur) : "a"(14), "b"(0) : "memory");
    if (inc == 0) return (void*)cur;
    uint64_t res = 0;
    __asm__ volatile ("int $0x80" : "=a"(res) : "a"(14), "b"(cur + inc) : "memory");
    if (res == (uint64_t)-1) return (void*)(uintptr_t)-1;   // ядро отказало (нет памяти)
    return (void*)cur;
}

static inline int sys_ioctl(int fd, uint64_t req, void* arg) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(15), "b"(fd), "c"(req), "d"(arg) : "memory");
    return ret;
}

static inline int sys_get_dir_files(const char* path, sys_file_info_t* buf, int max_files) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(16), "b"(path), "c"(buf), "d"(max_files) : "memory");
    return ret;
}

static inline int sys_fork(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(30) : "memory");
    return ret;
}

static inline int sys_execve(const char* fn, const char* args) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(31), "b"(fn), "c"(args) : "memory");
    return ret;
}

static inline int sys_create_window(const win_desc_t* desc) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(40), "b"(desc) : "memory");
    return ret;
}

static inline int sys_get_windows(kernel_window_t* out, int max_cnt) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(41), "b"(out), "c"(max_cnt) : "memory");
    return ret;
}

// Ядро берёт 4-й аргумент из R15 (frame[0]), поэтому кладём его туда явно.
static inline int sys_update_window(int id, int x, int y, int is_minimized) {
    uint64_t ret = 42;
    register uint64_t r15v __asm__("r15") = (uint64_t)is_minimized;
    __asm__ volatile ("int $0x80" : "+a"(ret) : "b"(id), "c"(x), "d"(y), "r"(r15v) : "memory");
    return (int)ret;
}

static inline int sys_kill(int pid, int sig) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(33), "b"(pid), "c"(sig) : "memory");
    return ret;
}

static inline int sys_waitpid(int pid, int* status, int options) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(32), "b"(pid), "c"(status), "d"(options) : "memory");
    return ret;
}

static inline int sys_win_present(int id, const void* px) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(43), "b"(id), "c"(px) : "memory");
    return ret;
}
static inline int sys_win_read(int id, void* dst, uint32_t* seq) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(44), "b"(id), "c"(dst), "d"(seq) : "memory");
    return ret;
}
static inline int sys_win_poll_event(int id, win_event_t* ev) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(45), "b"(id), "c"(ev) : "memory");
    return ret;
}
static inline int sys_win_push_event(int id, const win_event_t* ev) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(46), "b"(id), "c"(ev) : "memory");
    return ret;
}

static inline void sys_system_reboot(void) {
    __asm__ volatile ("int $0x80" : : "a"(50) : "memory");
    while (1);
}

static inline void sys_system_shutdown(void) {
    __asm__ volatile ("int $0x80" : : "a"(51) : "memory");
    while (1);
}

static inline int sys_get_net_info(sys_net_info_t* info) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(60), "b"(info) : "memory");
    return ret;
}

#endif