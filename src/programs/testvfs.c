// testvfs.c — Комплексный стресс-тест подсистемы VFS и devfs для devOS (Ring 3).
//
// Проверяет:
// 1. Потоковый вывод через write(1, ...) (FD 1 привязан к /dev/tty)
// 2. Виртуальные устройства: /dev/null, /dev/zero
// 3. ioctl к /dev/fb0 (получение геометрии экрана)
// 4. Реальные файлы FAT32: запись и чтение через дескрипторы
// 5. Наследование таблицы fd_table при ov_fork()
// 6. Ошибки на невалидных FD (fd = 99)

#include "ovos.h"   // общие обёртки сисколов (src/lib)

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
    ov_print_color("  [PASS] ", 0x55FF55);
    ov_print_color(name, 0xFFFFFF);
    ov_print_color("\n", 0xFFFFFF);
}

static void print_fail(const char* name) {
    ov_print_color("  [FAIL] ", 0xFF5555);
    ov_print_color(name, 0xFFFFFF);
    ov_print_color("\n", 0xFFFFFF);
}

static void print_info_num(const char* prefix, uint64_t val) {
    ov_print_color(prefix, 0xAAAAAA);
    char buf[24];
    u64_to_str(val, buf);
    ov_print_color(buf, 0xFFFF55);
    ov_print_color("\n", 0xFFFFFF);
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
    ov_print_color("\n================ [ devOS VFS TEST SUITE ] ================\n", 0x55FFFF);

    // -------------------------------------------------------------------------
    // ТЕСТ 1: Проверка stdout (FD 1)
    // -------------------------------------------------------------------------
    ov_print_color("[TEST 1] Testing ov_write() to FD 1 (stdout / devfs tty)...\n", 0xFFFF55);
    const char* msg = "  -> Hello from write(fd=1)! This text is routed via VFS.\n";
    int64_t w_ret = ov_write(1, msg, kstrlen(msg));
    if (w_ret == kstrlen(msg)) {
        print_pass("write(1, ...) succeeded");
    } else {
        print_fail("write(1, ...) failed");
    }

    // -------------------------------------------------------------------------
    // ТЕСТ 2: Проверка виртуального устройства /dev/null
    // -------------------------------------------------------------------------
    ov_print_color("\n[TEST 2] Testing /dev/null device node...\n", 0xFFFF55);
    int fd_null = ov_open("/dev/null", 0);
    if (fd_null >= 0) {
        char dump_buf[16] = "DISCARD_THIS";
        int64_t nw = ov_write(fd_null, dump_buf, 12);
        int64_t nr = ov_read(fd_null, dump_buf, 16);
        ov_close(fd_null);

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
    ov_print_color("\n[TEST 3] Testing /dev/zero device node...\n", 0xFFFF55);
    int fd_zero = ov_open("/dev/zero", 0);
    if (fd_zero >= 0) {
        uint8_t zbuf[32];
        for (int i = 0; i < 32; i++) zbuf[i] = 0xAA; // Заполняем мусором

        int64_t zr = ov_read(fd_zero, zbuf, sizeof(zbuf));
        ov_close(fd_zero);

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
    ov_print_color("\n[TEST 4] Testing /dev/fb0 and ov_ioctl()...\n", 0xFFFF55);
    int fd_fb = ov_open("/dev/fb0", 0);
    if (fd_fb >= 0) {
        fb_var_info_t fb_info;
        fb_info.width = 0;
        fb_info.height = 0;
        fb_info.bpp = 0;

        int ioctl_ret = ov_ioctl(fd_fb, FBIOGET_VSCREENINFO, &fb_info);
        ov_close(fd_fb);

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
    ov_print_color("\n[TEST 5] Testing FAT32 File I/O via VFS...\n", 0xFFFF55);
    const char* test_file = "/VFSTEST.TXT";
    const char* test_content = "OVEROS_VFS_VERIFIED_DATA";

    // 1. Запись файла
    int fd_wr = ov_open(test_file, 1); // 1 = O_WRONLY
    if (fd_wr >= 0) {
        ov_write(fd_wr, test_content, kstrlen(test_content));
        ov_close(fd_wr);
        ov_print_color("  -> File written via VFS\n", 0xAAAAAA);
    }

    // 2. Чтение файла обратно
    int fd_rd = ov_open(test_file, 0); // 0 = O_RDONLY
    if (fd_rd >= 0) {
        char rdbuf[32];
        for (int i = 0; i < 32; i++) rdbuf[i] = 0;

        int64_t bytes = ov_read(fd_rd, rdbuf, sizeof(rdbuf) - 1);
        ov_close(fd_rd);

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
            ov_print_color("     Content: [", 0xAAAAAA);
            ov_print_color(rdbuf, 0x55FF55);
            ov_print_color("]\n", 0xAAAAAA);
        } else {
            print_fail("FAT32 read content does not match written data");
        }
    } else {
        print_fail("Failed to re-open /VFSTEST.TXT for reading");
    }

    // -------------------------------------------------------------------------
    // ТЕСТ 6: Наследование дескрипторов при fork() (UNIX fd_table inheritance)
    // -------------------------------------------------------------------------
    ov_print_color("\n[TEST 6] Testing FD Table inheritance across fork()...\n", 0xFFFF55);
    
    // Родитель открывает /dev/zero ДО форка
    int parent_fd = ov_open("/dev/zero", 0);
    if (parent_fd < 0) {
        print_fail("Parent failed to prepare FD for fork test");
    } else {
        uint64_t fork_ret = ov_fork();

        if (fork_ret == 0) {
            // --- Ветка потомка ---
            print_info_num("  [CHILD] Inherited parent_fd = ", (uint64_t)parent_fd);

            uint8_t child_buf[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            int64_t cr = ov_read(parent_fd, child_buf, 8);
            print_info_num("  [CHILD] sys_read returned cr = ", (uint64_t)cr);

            ov_close(parent_fd);

            int ok = (cr == 8);
            for (int i = 0; i < 8; i++) {
                if (child_buf[i] != 0) {
                    ok = 0;
                    print_info_num("  [CHILD] Non-zero byte at index ", i);
                    print_info_num("          Byte value = ", (uint64_t)child_buf[i]);
                    break;
                }
            }

            ov_exit(ok ? 25 : 99);
        } else if ((int64_t)fork_ret > 0) {
            // --- Ветка родителя ---
            int child_status = 0;
            ov_waitpid((int)fork_ret, &child_status, 0);
            ov_close(parent_fd);

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
    ov_print_color("\n[TEST 7] Testing invalid file descriptors handling...\n", 0xFFFF55);
    char dummy[4];
    int64_t bad_r = ov_read(99, dummy, 4);
    int bad_c = ov_close(99);

    if (bad_r == -1 && bad_c == -1) {
        print_pass("Kernel gracefully rejected invalid FD 99");
    } else {
        print_fail("Kernel did not return -1 on invalid FD");
    }

    ov_print_color("\n==========================================================\n", 0x55FFFF);
    ov_print_color(">>> VFS & DEVFS TEST FINISHED. EXITING WITH CODE 0 <<<\n\n", 0x55FF55);

    ov_exit(0);
    return 0;
}