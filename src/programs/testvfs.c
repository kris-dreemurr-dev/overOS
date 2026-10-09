// testvfs.c — Комплексный стресс-тест подсистемы VFS и devfs для devOS (Ring 3).
//
// Проверяет:
// 1. Потоковый вывод через write(1, ...) (FD 1 привязан к /dev/tty)
// 2. Виртуальные устройства: /dev/null, /dev/zero
// 3. ioctl к /dev/fb0 (получение геометрии экрана)
// 4. Реальные файлы FAT32: запись и чтение через дескрипторы
// 5. Наследование таблицы fd_table при sys_fork()
// 6. Ошибки на невалидных FD (fd = 99)

#include <stdint.h>

// -----------------------------------------------------------------------------
// Системные вызовы через int $0x80 (64-битный шлюз)
// -----------------------------------------------------------------------------
static inline uint64_t syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                      : "memory");
    return ret;
}

static inline void sys_exit(int code) {
    syscall4(0, (uint64_t)code, 0, 0);
    while (1) { }
}

static inline void sys_print_color(const char* str, uint32_t color) {
    syscall4(1, (uint64_t)str, color, 0);
}

static inline int sys_open(const char* path, uint32_t flags) {
    return (int)(int64_t)syscall4(10, (uint64_t)path, (uint64_t)flags, 0);
}

static inline int64_t sys_read(int fd, void* buf, uint64_t size) {
    return (int64_t)syscall4(11, (uint64_t)fd, (uint64_t)buf, size);
}

static inline int sys_close(int fd) {
    return (int)(int64_t)syscall4(12, (uint64_t)fd, 0, 0);
}

static inline int64_t sys_write(int fd, const void* buf, uint64_t size) {
    return (int64_t)syscall4(13, (uint64_t)fd, (uint64_t)buf, size);
}

static inline int sys_ioctl(int fd, uint64_t req, void* arg) {
    return (int)(int64_t)syscall4(15, (uint64_t)fd, req, (uint64_t)arg);
}

static inline uint64_t sys_fork(void) {
    return syscall4(30, 0, 0, 0);
}

static inline int sys_waitpid(uint64_t pid, int* out_status) {
    return (int)syscall4(32, pid, (uint64_t)(uintptr_t)out_status, 0);
}

// -----------------------------------------------------------------------------
// Вспомогательные строковые функции (без libc)
// -----------------------------------------------------------------------------
static int kstrlen(const char* s) {
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

static void u64_to_str(uint64_t v, char* out) {
    char tmp[21];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

static void print_pass(const char* name) {
    sys_print_color("  [PASS] ", 0x55FF55);
    sys_print_color(name, 0xFFFFFF);
    sys_print_color("\n", 0xFFFFFF);
}

static void print_fail(const char* name) {
    sys_print_color("  [FAIL] ", 0xFF5555);
    sys_print_color(name, 0xFFFFFF);
    sys_print_color("\n", 0xFFFFFF);
}

static void print_info_num(const char* prefix, uint64_t val) {
    sys_print_color(prefix, 0xAAAAAA);
    char buf[24];
    u64_to_str(val, buf);
    sys_print_color(buf, 0xFFFF55);
    sys_print_color("\n", 0xFFFFFF);
}

// Структура для ioctl(/dev/fb0)
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint64_t paddr;
} fb_var_info_t;

#define FBIOGET_VSCREENINFO 0x4600

// =============================================================================
// 1. ТОЧКА ВХОДА ДОЛЖНА БЫТЬ ПЕРВОЙ В ФАЙЛЕ!
// =============================================================================
int main(void) {
    sys_print_color("\n================ [ devOS VFS TEST SUITE ] ================\n", 0x55FFFF);

    // -------------------------------------------------------------------------
    // ТЕСТ 1: Проверка stdout (FD 1)
    // -------------------------------------------------------------------------
    sys_print_color("[TEST 1] Testing sys_write() to FD 1 (stdout / devfs tty)...\n", 0xFFFF55);
    const char* msg = "  -> Hello from write(fd=1)! This text is routed via VFS.\n";
    int64_t w_ret = sys_write(1, msg, kstrlen(msg));
    if (w_ret == kstrlen(msg)) {
        print_pass("write(1, ...) succeeded");
    } else {
        print_fail("write(1, ...) failed");
    }

    // -------------------------------------------------------------------------
    // ТЕСТ 2: Проверка виртуального устройства /dev/null
    // -------------------------------------------------------------------------
    sys_print_color("\n[TEST 2] Testing /dev/null device node...\n", 0xFFFF55);
    int fd_null = sys_open("/dev/null", 0);
    if (fd_null >= 0) {
        char dump_buf[16] = "DISCARD_THIS";
        int64_t nw = sys_write(fd_null, dump_buf, 12);
        int64_t nr = sys_read(fd_null, dump_buf, 16);
        sys_close(fd_null);

        if (nw == 12 && nr == 0) {
            print_pass("/dev/null swallowed data and returned EOF on read");
        } else {
            print_fail("/dev/null returned unexpected read/write counts");
        }
    } else {
        print_fail("Failed to open /dev/null");
    }

    // -------------------------------------------------------------------------
    // ТЕСТ 3: Проверка виртуального устройства /dev/zero
    // -------------------------------------------------------------------------
    sys_print_color("\n[TEST 3] Testing /dev/zero device node...\n", 0xFFFF55);
    int fd_zero = sys_open("/dev/zero", 0);
    if (fd_zero >= 0) {
        uint8_t zbuf[32];
        for (int i = 0; i < 32; i++) zbuf[i] = 0xAA; // Заполняем мусором

        int64_t zr = sys_read(fd_zero, zbuf, sizeof(zbuf));
        sys_close(fd_zero);

        int all_zeros = 1;
        for (int i = 0; i < 32; i++) {
            if (zbuf[i] != 0) { all_zeros = 0; break; }
        }

        if (zr == 32 && all_zeros) {
            print_pass("/dev/zero correctly filled buffer with 0x00");
        } else {
            print_fail("/dev/zero failed to clear buffer");
        }
    } else {
        print_fail("Failed to open /dev/zero");
    }

    // -------------------------------------------------------------------------
    // ТЕСТ 4: Проверка /dev/fb0 и sys_ioctl
    // -------------------------------------------------------------------------
    sys_print_color("\n[TEST 4] Testing /dev/fb0 and sys_ioctl()...\n", 0xFFFF55);
    int fd_fb = sys_open("/dev/fb0", 0);
    if (fd_fb >= 0) {
        fb_var_info_t fb_info;
        fb_info.width = 0;
        fb_info.height = 0;
        fb_info.bpp = 0;

        int ioctl_ret = sys_ioctl(fd_fb, FBIOGET_VSCREENINFO, &fb_info);
        sys_close(fd_fb);

        if (ioctl_ret == 0 && fb_info.width > 0 && fb_info.height > 0 && fb_info.bpp == 32) {
            print_pass("/dev/fb0 returned valid display parameters");
            print_info_num("     Screen Width:  ", fb_info.width);
            print_info_num("     Screen Height: ", fb_info.height);
            print_info_num("     Screen BPP:    ", fb_info.bpp);
        } else {
            print_fail("ioctl(FBIOGET_VSCREENINFO) failed or invalid data");
        }
    } else {
        print_fail("Failed to open /dev/fb0");
    }

    // -------------------------------------------------------------------------
    // ТЕСТ 5: Проверка реальной файловой системы FAT32
    // -------------------------------------------------------------------------
    sys_print_color("\n[TEST 5] Testing FAT32 File I/O via VFS...\n", 0xFFFF55);
    const char* test_file = "/VFSTEST.TXT";
    const char* test_content = "OVEROS_VFS_VERIFIED_DATA";

    // 1. Запись файла
    int fd_wr = sys_open(test_file, 1); // 1 = O_WRONLY
    if (fd_wr >= 0) {
        sys_write(fd_wr, test_content, kstrlen(test_content));
        sys_close(fd_wr);
        sys_print_color("  -> File written via VFS\n", 0xAAAAAA);
    }

    // 2. Чтение файла обратно
    int fd_rd = sys_open(test_file, 0); // 0 = O_RDONLY
    if (fd_rd >= 0) {
        char rdbuf[32];
        for (int i = 0; i < 32; i++) rdbuf[i] = 0;

        int64_t bytes = sys_read(fd_rd, rdbuf, sizeof(rdbuf) - 1);
        sys_close(fd_rd);

        int matched = 1;
        int exp_len = kstrlen(test_content);
        if (bytes < exp_len) {
            matched = 0;
        } else {
            for (int i = 0; i < exp_len; i++) {
                if (rdbuf[i] != test_content[i]) { matched = 0; break; }
            }
        }

        if (matched) {
            print_pass("FAT32 Read/Write cycle through VFS verified successfully");
            sys_print_color("     Content: [", 0xAAAAAA);
            sys_print_color(rdbuf, 0x55FF55);
            sys_print_color("]\n", 0xAAAAAA);
        } else {
            print_fail("FAT32 read content does not match written data");
        }
    } else {
        print_fail("Failed to re-open /VFSTEST.TXT for reading");
    }

    // -------------------------------------------------------------------------
    // ТЕСТ 6: Наследование дескрипторов при fork() (UNIX fd_table inheritance)
    // -------------------------------------------------------------------------
    sys_print_color("\n[TEST 6] Testing FD Table inheritance across fork()...\n", 0xFFFF55);
    
    // Родитель открывает /dev/zero ДО форка
    int parent_fd = sys_open("/dev/zero", 0);
    if (parent_fd < 0) {
        print_fail("Parent failed to prepare FD for fork test");
    } else {
        uint64_t fork_ret = sys_fork();

        if (fork_ret == 0) {
            // --- Ветка потомка ---
            print_info_num("  [CHILD] Inherited parent_fd = ", (uint64_t)parent_fd);

            uint8_t child_buf[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            int64_t cr = sys_read(parent_fd, child_buf, 8);
            print_info_num("  [CHILD] sys_read returned cr = ", (uint64_t)cr);

            sys_close(parent_fd);

            int ok = (cr == 8);
            for (int i = 0; i < 8; i++) {
                if (child_buf[i] != 0) {
                    ok = 0;
                    print_info_num("  [CHILD] Non-zero byte at index ", i);
                    print_info_num("          Byte value = ", (uint64_t)child_buf[i]);
                    break;
                }
            }

            sys_exit(ok ? 25 : 99);
        } else if ((int64_t)fork_ret > 0) {
            // --- Ветка родителя ---
            int child_status = 0;
            sys_waitpid(fork_ret, &child_status);
            sys_close(parent_fd);

            if (child_status == 25) {
                print_pass("Child successfully inherited and used parent's open FD");
            } else {
                print_info_num("  [PARENT] Child exit code was: ", (uint64_t)child_status);
                print_fail("Child failed to read from inherited parent FD");
            }
        } else {
            print_fail("fork() failed in TEST 6");
        }
    }

    // -------------------------------------------------------------------------
    // ТЕСТ 7: Обработка невалидных дескрипторов
    // -------------------------------------------------------------------------
    sys_print_color("\n[TEST 7] Testing invalid file descriptors handling...\n", 0xFFFF55);
    char dummy[4];
    int64_t bad_r = sys_read(99, dummy, 4);
    int bad_c = sys_close(99);

    if (bad_r == -1 && bad_c == -1) {
        print_pass("Kernel gracefully rejected invalid FD 99");
    } else {
        print_fail("Kernel did not return -1 on invalid FD");
    }

    sys_print_color("\n==========================================================\n", 0x55FFFF);
    sys_print_color(">>> VFS & DEVFS TEST FINISHED. EXITING WITH CODE 0 <<<\n\n", 0x55FF55);

    sys_exit(0);
    return 0;
}