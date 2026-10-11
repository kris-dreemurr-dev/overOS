#include <stdint.h>
#include <stddef.h>

// Системные вызовы ядра Ring 3
static inline void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(0), "b"(code) : "memory");
    while (1);
}

static inline void sys_sleep(uint32_t ms) {
    uint64_t r = 4;
    __asm__ volatile ("int $0x80" : "+a"(r) : "b"(ms) : "memory");
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

static inline int sys_get_dir_files(const char* path, void* buf, int max_files) {
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

typedef struct {
    const char*     title;
    int             w, h;
    const uint16_t* icon;
} __attribute__((packed)) win_desc_t;

static inline int sys_create_window(const win_desc_t* desc) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(40), "b"(desc) : "memory");
    return ret;
}

// Иконка папки 16x16
static const uint16_t icon_folder[16] = {
    0b0000000000000000,
    0b0001111000000000,
    0b0011111100000000,
    0b0111111111111110,
    0b0100000000000010,
    0b0101111111111110,
    0b0101111111111110,
    0b0101111111111110,
    0b0101111111111110,
    0b0101111111111110,
    0b0101111111111110,
    0b0101111111111110,
    0b0101111111111110,
    0b0111111111111110,
    0b0000000000000000,
    0b0000000000000000
};

// Проверка и запуск файла
int overfiles_launch(const char* path) {
    int len = 0;
    while (path[len]) len++;

    // 1. БЛОКИРУЕМ .SYS (Ring 0 модули ядра)
    if (len > 4 && (path[len-4] == '.' && 
       (path[len-3] == 's' || path[len-3] == 'S') &&
       (path[len-2] == 'y' || path[len-2] == 'Y') &&
       (path[len-1] == 's' || path[len-1] == 'S'))) {
        return -1; // Ошибка: SYS модуль
    }

    // 2. БЛОКИРУЕМ TTY .PRG (программы без метки --desktop)
    int fd = sys_open(path, 0);
    if (fd >= 0) {
        uint8_t hdr[64];
        int rd = sys_read(fd, hdr, 64);
        sys_close(fd);

        if (rd >= 64 && hdr[0] == 'D' && hdr[1] == 'P' && hdr[2] == 'R' && hdr[3] == 'G') {
            uint64_t flags = *(uint64_t*)&hdr[48];
            if (!(flags & 0x01)) {
                return -2; // Ошибка: консольная программа TTY
            }
        }
    }

    // 3. Запуск разрешенного GUI процесса
    int pid = sys_fork();
    if (pid == 0) {
        sys_execve(path, 0);
        sys_exit(1);
    }
    return 0;
}

int main(void) {
    // Регистрация окна через оконный сервер DE
    win_desc_t desc = {
        .title = "overFiles - Файлы",
        .w = 540,
        .h = 320,
        .icon = icon_folder
    };

    int win_id = sys_create_window(&desc);
    if (win_id < 0) {
        sys_exit(1);
    }

    // Главный цикл приложения
    while (1) {
        sys_sleep(50);
    }

    return 0;
}