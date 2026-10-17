#include "tty.h"
#include <stdint.h>
#include <stddef.h>
#include "../drivers/acpi.h"
#include "../drivers/pci.h"
#include "../drivers/ethernet.h"
#include "../drivers/hda.h"
#include "../drivers/usb/xhci.h"
#include "../drivers/usb/usb.h"
#include "../drivers/blk.h"
#include "../fs/mount.h"
#include "../fs/fs.h"
#include "sys_loader.h"
#include "prog_loader.h"
#include "sched.h"
#include "config.h"
#include "users.h"
#include "login.h"

#define MAX_INPUT 256

extern void kputs(const char* str, uint32_t color);
extern void kputc(char c, uint32_t color);
extern void itoa(int n, char* str);
extern void hex_str(uint32_t val, char* out);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void sleep_ms(uint32_t ms);
extern void clear_screen(uint32_t color);
extern void flush_buffer(void);
extern void sys_reboot(void);
extern void print_prompt(void);
extern int  strcmp(const char* s1, const char* s2);

// Программы и визуальные режимы ядра
extern void enter_render_mode(void);
extern void cmd_fastfetch(void);
extern void acpi_power_off(void);
extern void intel_set_backlight(int level, int silent);

// Текущий путь в файловой системе (корень "/")

static uint8_t file_buf[1024 * 1024] __attribute__((aligned(4096)));
static uint8_t mbr_tmp[512] __attribute__((aligned(64)));

static int strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int strncmp(const char* s1, const char* s2, int n) {
    for (int i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}

static char* strcat(char* dest, const char* src) {
    char* ptr = dest + strlen(dest);
    while (*src) {
        *ptr++ = *src++;
    }
    *ptr = '\0';
    return dest;
}

static int strchr_slash(const char* s) {
    while (*s) if (*s++ == '/') return 1;
    return 0;
}

static int is_sys_command(const char* cmd) {
    int len = 0;
    while (cmd[len] && cmd[len] != ' ') len++;
    if (len < 5) return 0;

    const char* ext = &cmd[len - 4];
    if (ext[0] == '.' &&
       (ext[1] == 's' || ext[1] == 'S') &&
       (ext[2] == 'y' || ext[2] == 'Y') &&
       (ext[3] == 's' || ext[3] == 'S')) {
        return 1;
    }
    return 0;
}

static void copy_to_sys_name(const char* cmd, char* out_sys, int max_len) {
    int i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < (max_len - 1)) {
        char c = cmd[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out_sys[i] = c;
        i++;
    }
    out_sys[i] = '\0';
}

// Распознает как .prg, так и .prog (в любом регистре)
static int is_prog_command(const char* cmd) {
    int len = 0;
    while (cmd[len] && cmd[len] != ' ') len++;

    // Проверка окончания на .prg (4 символа)
    if (len >= 5) {
        const char* ext = &cmd[len - 4];
        if (ext[0] == '.' &&
           (ext[1] == 'p' || ext[1] == 'P') &&
           (ext[2] == 'r' || ext[2] == 'R') &&
           (ext[3] == 'g' || ext[3] == 'G')) {
            return 1;
        }
    }

    return 0;
}

// Нормализует имя программы: отсекает старое расширение и всегда делает "ИМЯ.PRG"
static void copy_to_prog_name(const char* cmd, char* out_prog, int max_len) {
    int len = 0;
    while (cmd[len] && cmd[len] != ' ') len++;

    // Ищем точку перед расширением
    int dot_pos = -1;
    for (int i = 0; i < len; i++) {
        if (cmd[i] == '.') dot_pos = i;
    }

    int name_len = (dot_pos != -1) ? dot_pos : len;
    int out_len = 0;

    // Копируем имя в верхнем регистре (до 8 символов формата 8.3 fs)
    for (int i = 0; i < name_len && out_len < (max_len - 5) && out_len < 8; i++) {
        char c = cmd[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out_prog[out_len++] = c;
    }

    // Всегда дописываем чистое 3-буквенное расширение fs: .PRG
    out_prog[out_len++] = '.';
    out_prog[out_len++] = 'P';
    out_prog[out_len++] = 'R';
    out_prog[out_len++] = 'G';
    out_prog[out_len] = '\0';
}

void cmd_timer_test(void) {
    kputs("Запуск теста таймера на 10 секунд...\n", 0x55FFFF);
    flush_buffer();

    for (int i = 1; i <= 10; i++) {
        sleep_ms(1000); // Ждем ровно 1000 тиков PIT
        
        char buf[16];
        itoa(i, buf);
        kputs(buf, 0xFFFFFF);
        kputs("... ", 0xAAAAAA);
        flush_buffer();
    }
    
    kputs("\n[ OK ] Тест завершен!\n", 0x00AA00);
    flush_buffer();
}

static int s_no_newline = 0;   // повторный вход в execute_command для sudo: без лишней пустой строки

void execute_command(void);
int  shell_kernel_builtin(const char* cmd_in);

// sudo <команда>: пароль принят — исполняем её от root, затем права возвращаются
static int run_pending_sudo(void) {
    tty_t* con = tty_console();
    char cmd_copy[MAX_INPUT];
    if (!login_take_sudo_cmd(cmd_copy, sizeof(cmd_copy))) return 0;
    int i = 0;
    while (cmd_copy[i] && i < (int)sizeof(con->input_buf) - 1) { con->input_buf[i] = cmd_copy[i]; i++; }
    con->input_buf[i] = '\0';
    con->input_len = i;
    s_no_newline = 1;
    execute_command();          // в конце вызовет login_after_command и напечатает приглашение
    return 1;
}

void execute_command(void) {
    tty_t* con = tty_console();
    if (!con) return;
    if (s_no_newline) s_no_newline = 0;
    else kputc('\n', 0xFFFFFF);

    // До входа в систему (и при вводе пароля) строку забирает login, а не оболочка
    if (login_wants_line()) {
        if (con->input_len >= (int)sizeof(con->input_buf)) con->input_len = (int)sizeof(con->input_buf) - 1;
        con->input_buf[con->input_len] = '\0';
        login_handle_line(con->input_buf);
        con->input_len = 0;
        con->input_buf[0] = '\0';
        if (run_pending_sudo()) return;
        print_prompt();
        return;
    }

    if (con->input_len <= 0) {
        con->input_len = 0;
        con->input_buf[0] = '\0';
        print_prompt();
        return;
    }

    if (con->input_len >= (int)sizeof(con->input_buf)) con->input_len = (int)sizeof(con->input_buf) - 1;
    con->input_buf[con->input_len] = '\0';

    char* cmd = con->input_buf;
    while (*cmd == ' ') cmd++;

    if (*cmd == '\0') {
        con->input_len = 0;
        con->input_buf[0] = '\0';
        print_prompt();
        return;
    }

    // --- Команды сеанса: whoami, id, su, exit, logout, passwd, useradd, chmod, chown, stat ---
    if (login_builtin(cmd)) {
        con->input_len = 0;
        con->input_buf[0] = '\0';
        if (run_pending_sudo()) return;   // root вызвал sudo <команда> — без пароля
        login_after_command();
        print_prompt();
        return;
    }

    // Сервисные команды ядра (taskmgr, модули, сеть, звук, запуск .prg)
    shell_kernel_builtin(cmd);

    con->input_len = 0;
    con->input_buf[0] = '\0';

    login_after_command();   // команда через sudo закончилась — права пользователя обратно
    print_prompt();
}

// Экспортируемая функция исполнения команд шелла из оконных модулей
void sys_exec_cmd(const char* cmd_str) {
    if (!cmd_str || cmd_str[0] == '\0') return;
    tty_t* con = tty_console();
    if (!con) return;
    int i = 0;
    while (cmd_str[i] && i < (int)sizeof(con->input_buf) - 1) {
        con->input_buf[i] = cmd_str[i];
        i++;
    }
    con->input_buf[i] = '\0';
    con->input_len = i;
    execute_command();
}
// ============================================================================
// Сервисные команды ядра: диспетчер задач, модули .sys, сеть, звук, демо, запуск
// программ. Оболочка пользователя уже живёт в Ring 3 (sh.prg) и зовёт это через
// sys_kcmd(118) для того, что ещё не переехало в userspace.
// Возвращает 1, если команда распознана.
// ============================================================================
int shell_kernel_builtin(const char* cmd_in) {
    char cmd_storage[256];
    int ci = 0;
    while (cmd_in[ci] && ci < (int)sizeof(cmd_storage) - 1) { cmd_storage[ci] = cmd_in[ci]; ci++; }
    cmd_storage[ci] = '\0';
    char* cmd = cmd_storage;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return 1;

    if (strcmp(cmd, "clear") == 0) {
        clear_screen(0x000000);
    } else if (strcmp(cmd, "help") == 0) {
        kputs("================Command List================\n", 0x55FFFF);
        kputs("help                ", 0xFFFF55); kputs("- Show this help message\n", 0xFFFFFF);
        kputs("clear               ", 0xFFFF55); kputs("- Clear the screen\n", 0xFFFFFF);
        kputs("ver                 ", 0xFFFF55); kputs("- Display Kernel version\n", 0xFFFFFF);
        kputs("ping <ip>           ", 0xFFFF55); kputs("- Ping IP-Address\n", 0xFFFFFF);
        kputs("pong                ", 0xFFFF55); kputs("- Get IP-Ping from another PC/Device\n", 0xFFFFFF);
        kputs("play-sound          ", 0xFFFF55); kputs("- Playing Sound on HDA Device\n", 0xFFFFFF);
        kputs("rus                 ", 0xFFFF55); kputs("- Turning Russian lang if ALT is broken\n", 0xFFFFFF);
        kputs("1972                ", 0xFFFF55); kputs("- Turning English lang if ALT is broken\n", 0xFFFFFF);

        kputs("\n--- Applications & GUI ---\n", 0x55FF55);
        kputs("render              ", 0xFFFF55); kputs("- Enter 3D rendering mode demo\n", 0xFFFFFF);
        kputs("<app>.sys           ", 0xFFFF55); kputs("- Run module (Kernel Mode)\n", 0xFFFFFF);
        kputs("<app>.prg          ", 0xFFFF55); kputs("- Run application (User Mode)\n", 0xFFFFFF);

        kputs("\n--- File Operations ---\n", 0x55FF55);
        kputs("ls / dir            ", 0xFFFF55); kputs("- List files in current directory\n", 0xFFFFFF);
        kputs("cd <dir>            ", 0xFFFF55); kputs("- Change directory (e.g. cd dir / cd ..)\n", 0xFFFFFF);
        kputs("cat <file>          ", 0xFFFF55); kputs("- Read file contents\n", 0xFFFFFF);
        kputs("touch <file>        ", 0xFFFF55); kputs("- Create an empty file\n", 0xFFFFFF);
        kputs("mkdir <dir>         ", 0xFFFF55); kputs("- Create a directory\n", 0xFFFFFF);
        kputs("rm <file/dir>       ", 0xFFFF55); kputs("- Delete file or directory\n", 0xFFFFFF);

        kputs("\n--- Hardware & Diagnostics ---\n", 0x55FF55);
        kputs("fastfetch           ", 0xFFFF55); kputs("- Display system information\n", 0xFFFFFF);
        kputs("fastfetch -a        ", 0xFFFF55); kputs("- Pci scaner all pc (USB, Audio, Net)\n", 0xFFFFFF);
        kputs("brightness <0-100>  ", 0xFFFF55); kputs("- Set screen backlight brightness\n", 0xFFFFFF);
        kputs("netinfo             ", 0xFFFF55); kputs("- Network info\n", 0xFFFFFF);
        kputs("lspci / pci         ", 0xFFFF55); kputs("- List PCI devices (id, class, subsystem)\n", 0xFFFFFF);
        kputs("taskmgr / ps        ", 0xFFFF55); kputs("- Task Manager\n", 0xFFFFFF);
        kputs("pkill <pid> [sig]   ", 0xFFFF55); kputs("- Send a signal to PID (default SIGKILL=9)\n", 0xFFFFFF);

        kputs("\n--- System Control ---\n", 0x55FF55);
        kputs("reboot / r          ", 0xFFFF55); kputs("- Restart system\n", 0xFFFFFF);
        kputs("shutdown / s        ", 0xFFFF55); kputs("- Shutdown system via ACPI\n", 0xFFFFFF);

    // --- Встроенные приложения и диагностика ---
    } else if (strcmp(cmd, "render") == 0) {
        enter_render_mode();
    } else if (strcmp(cmd, "time") == 0) {
        cmd_timer_test();
    } else if (strcmp(cmd, "fastfetch -a") == 0 || strcmp(cmd, "fastfetch-audio") == 0) {
        cmd_fastfetch_audio();
    } else if (strcmp(cmd, "fastfetch") == 0) {
        cmd_fastfetch();
    } else if (strcmp(cmd, "taskmgr") == 0 || strcmp(cmd, "ps") == 0) {
        sched_dump_tasks();
    } else if (strcmp(cmd, "crash") == 0) {
        // Намеренное падение ядра для проверки BSOD (как Alt+SysRq+C в Linux).
        // Иначе синий экран нечем проверить, кроме настоящей ошибки.
        if (cred_uid() != UID_ROOT) {
            kputs("crash: нужны права root\n", 0xFF5555);
        } else {
            kputs("crash: роняем ядро по запросу...\n", 0xFFFF55);
            flush_buffer();
            volatile uint64_t* bad = (volatile uint64_t*)0xFFFFFF0000DEAD00ULL;
            *bad = 0x1234;
        }
    } else if (strncmp(cmd, "ping", 4) == 0) {
        char* arg = cmd + 4;
        while (*arg == ' ') arg++;
        cmd_ping(arg);
    } else if (strcmp(cmd, "pong") == 0 || strcmp(cmd, "pingd") == 0) {
        cmd_pong();
    } else if (strcmp(cmd, "usb") == 0 || strcmp(cmd, "lsusb") == 0) {
        // Какой контроллер ведёт шину и что на портах. Нужно и мне при отладке,
        // и пользователю: загрузочный экран теперь краток, как у systemd.
        int drv = usb_active_driver();
        kputs("Драйвер USB: ", 0xAAAAAA);
        kputs(drv == USB_DRV_XHCI ? "xHCI (USB 3.0)\n" : drv == USB_DRV_EHCI ? "EHCI (USB 2.0)\n" : "нет\n", 0xFFFFFF);
        char b[16];
        if (xhci_present()) {
            kputs("xHCI: портов ", 0xAAAAAA); itoa(xhci_port_count(), b); kputs(b, 0xFFFFFF);
            kputs(", занято ", 0xAAAAAA); itoa(xhci_ports_connected(), b); kputs(b, 0xFFFFFF);
            kputs("\n", 0xFFFFFF);
        }
        {
            extern void usb_hid_counts(int* kbds, int* mice);
            int nk = 0, nm = 0;
            usb_hid_counts(&nk, &nm);
            kputs("USB-клавиатур: ", 0xAAAAAA); itoa(nk, b); kputs(b, 0xFFFFFF);
            kputs(", USB-мышей: ", 0xAAAAAA); itoa(nm, b); kputs(b, 0xFFFFFF);
            kputs("\n", 0xFFFFFF);
        }
        // Диски из реестра и куда они подключены
        mount_info_t mi[8];
        int nm = mount_list(mi, 8);
        for (int i = 0; i < BLK_MAX; i++) {
            blk_dev_t* d = blk_get(i);
            if (!d) continue;
            char nm_dev[40];
            blk_name(i, nm_dev, sizeof(nm_dev));
            kputs("диск ", 0xAAAAAA); itoa(i, b); kputs(b, 0xFFFFFF); kputs(": ", 0xAAAAAA);
            kputs(nm_dev, 0xFFFFFF);
            kputs(d->bus == BLK_BUS_XHCI ? " (xHCI), " : " (EHCI), ", 0xAAAAAA);
            itoa((int)blk_size_mb(i), b); kputs(b, 0xFFFFFF); kputs(" МБ", 0xAAAAAA);
            if (d->gone) kputs(", пропал", 0xFF5555);
            kputs("\n", 0);
        }
        for (int i = 0; i < nm; i++) {
            kputs("  ", 0); kputs(mi[i].path, 0x55FF55);
            if (mi[i].label[0]) { kputs("  [", 0xAAAAAA); kputs(mi[i].label, 0xFFFFFF); kputs("]", 0xAAAAAA); }
            kputs("\n", 0);
        }
    } else if (strcmp(cmd, "lspci") == 0 || strcmp(cmd, "pci") == 0) {
        cmd_lspci();
    } else if (strcmp(cmd, "usb-debug") == 0) {
        static int on = 0;
        on = !on;
        usb_set_verbose(on);
        kputs(on ? "Подробный вывод USB включён\n" : "Подробный вывод USB выключен\n", 0xFFFF55);
    } else if (strcmp(cmd, "hda-dump") == 0) {
        hda_dump_state();
    } else if (strcmp(cmd, "play sound") == 0 || strcmp(cmd, "play-sound") == 0 || strcmp(cmd, "playsound") == 0) {
        // 1. Проверяем инициализацию HDA
        if (hda_get_status() == HDA_STATUS_NOT_FOUND) {
            kputs("[HDA] Контроллер не был инициализирован. Пробуем инициализацию...\n", 0xFFFF55);
            flush_buffer();
            if (hda_init() != 0) {
                kputs("[HDA] Ошибка: Intel HDA контроллер не найден на шине PCI!\n", 0xFF5555);
                flush_buffer();
                return 1;
            }
        }

        // 2. Запуск низкого тестового тона (100 Гц, ~2% амплитуды)
        kputs("[HDA] Запуск тестового тона (100 Гц, безопасная громкость)\n", 0x55FFFF);
        kputs("[HDA] Воспроизведение на 10 секунд: ", 0xAAAAAA);
        flush_buffer();
        
        hda_play_test_tone();

        // 3. Обратный отсчёт на 10 секунд с шагом в 1 секунду
        for (int sec = 10; sec > 0; sec--) {
            // Печатаем оставшиеся секунды
            char sec_char = '0' + (sec % 10);
            if (sec == 10) {
                kputs("10.. ", 0x55FF55);
            } else {
                char buf[5] = { sec_char, '.', '.', ' ', '\0' };
                kputs(buf, 0x55FF55);
            }
            flush_buffer();

            sleep_ms(1000);
        }

        // 4. Остановка звукового DMA-стрима
        hda_stop();

        kputs("СТОП\n[HDA] Воспроизведение завершено, поток остановлен.\n", 0x55FFFF);
        flush_buffer();

    } else if (strncmp(cmd, "pkill ", 6) == 0) {
        char* arg = cmd + 6;
        while (*arg == ' ') arg++;

        // PID (обязательный)
        long pid = 0;
        int has_pid = 0;
        while (*arg >= '0' && *arg <= '9') { pid = pid * 10 + (*arg - '0'); arg++; has_pid = 1; }

        while (*arg == ' ') arg++;

        // Сигнал (необязательный, по умолчанию SIGKILL — как у настоящего pkill)
        int sig = SIGKILL;
        if (*arg >= '0' && *arg <= '9') {
            sig = 0;
            while (*arg >= '0' && *arg <= '9') { sig = sig * 10 + (*arg - '0'); arg++; }
        }

        if (!has_pid || pid <= 0) {
            kputs("Usage: pkill <pid> [signal]   (signal defaults to 9 = SIGKILL)\n", 0xFF5555);
        } else if (pid == 0) {
            kputs("pkill: refusing to signal PID 0 (kernel_main)\n", 0xFF5555);
        } else if (cred_uid() != UID_ROOT && sched_pid_uid((uint64_t)pid) != cred_uid()) {
            kputs("pkill: Operation not permitted (not your process)\n", 0xFF5555);
        } else {
            int r = sched_send_signal((uint64_t)pid, sig);
            char nb[16];
            if (r == 0) {
                kputs("Signal ", 0x55FF55); itoa(sig, nb); kputs(nb, 0xFFFFFF);
                kputs(" sent to PID ", 0x55FF55); itoa((int)pid, nb); kputs(nb, 0xFFFFFF);
                kputc('\n', 0xFFFFFF);
            } else {
                kputs("pkill: no such PID ", 0xFF5555); itoa((int)pid, nb); kputs(nb, 0xFFFFFF);
                kputs(" (not running, already dead, or that's you)\n", 0xFF5555);
            }
        }

    // --- Файловая система и драйверы ---
} else if (strncmp(cmd, "brightness ", 11) == 0 || strncmp(cmd, "bright ", 7) == 0) {
        int offset = (strncmp(cmd, "brightness ", 11) == 0) ? 11 : 7;
        int val = 0;
        char* arg = cmd + offset;
        while (*arg == ' ') arg++;
        
        while (*arg >= '0' && *arg <= '9') {
            val = val * 10 + (*arg - '0');
            arg++;
        }
        
        if (val < 0) val = 0;
        if (val > 100) val = 100;

        // Вызываем реальную функцию управления подсветкой
        intel_set_backlight(val, 0);

        kputs("[+] Backlight brightness successfully set to ", 0x55FF55);
        char buf[16];
        itoa(val, buf);
        kputs(buf, 0xFFFF55);
        kputs("%\n", 0x55FF55);
    } else if (strcmp(cmd, "dir") == 0 || strcmp(cmd, "ls") == 0) {
        if (login_may(".", PERM_R | PERM_X)) fs_dir();
} else if (strncmp(cmd, "cd ", 3) == 0) {
        char* target = cmd + 3;
        while (*target == ' ') target++;

        // Убираем хвостовые пробелы
        int tlen = strlen(target);
        while (tlen > 0 && target[tlen - 1] == ' ') {
            target[--tlen] = '\0';
        }

        if (strcmp(target, "..") == 0) {
            if (!fs_change_dir("..")) kputs("[-] Already at root!\n", 0xFF5555);
        } else if (strcmp(target, "/") == 0) {
            fs_go_root();
        } else if (!login_may(target, PERM_X)) {
            // нет права входа в каталог
        } else if (target[0] == '/' || strchr_slash(target)) {
            // Путь с '/' (абсолютный или a/b): разбираем целиком от корня
            char abs_path[128];
            perm_normalize(target, tty_cwd(), abs_path, sizeof(abs_path));
            if (!login_chdir_abs(abs_path)) kputs("[-] Directory not found!\n", 0xFF5555);
        } else {
            if (!fs_change_dir(target)) kputs("[-] Directory not found!\n", 0xFF5555);
        }
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        char* target = cmd + 4;
        while (*target == ' ') target++;
        int bytes = login_may(target, PERM_R) ? fs_read_file(target, file_buf, sizeof(file_buf) - 1) : -2;
        if (bytes == -2) {
            // отказ уже напечатан
        } else if (bytes > 0) {
            file_buf[bytes] = '\0';
            kputs((char*)file_buf, 0xFFFFFF);
            kputc('\n', 0xFFFFFF);
        } else {
            kputs("[-] File not found or read error.\n", 0xFF5555);
        }
    } else if (strncmp(cmd, "touch ", 6) == 0) {
        char* target = cmd + 6;
        while (*target == ' ') target++;
        if (!login_may_parent(target, PERM_W)) {
            // отказ уже напечатан
        } else if (fs_touch(target)) {
            perm_on_create(cred_uid(), cred_gid(), target, tty_cwd(), 0);
            kputs("[+] File created successfully.\n", 0x55FF55);
        } else {
            kputs("[-] Failed to create file (already exists or disk full).\n", 0xFF5555);
        }
    } else if (strncmp(cmd, "mkdir ", 6) == 0) {
        char* target = cmd + 6;
        while (*target == ' ') target++;
        if (!login_may_parent(target, PERM_W)) {
            // отказ уже напечатан
        } else if (fs_make_folder(target)) {
            perm_on_create(cred_uid(), cred_gid(), target, tty_cwd(), 1);
            kputs("[+] Directory created successfully.\n", 0x55FF55);
        } else {
            kputs("[-] Failed to create directory (already exists or disk full).\n", 0xFF5555);
        }
    } else if (strncmp(cmd, "rm ", 3) == 0) {
        char* target = cmd + 3;
        while (*target == ' ') target++;
        if (!login_may_parent(target, PERM_W)) {
            // отказ уже напечатан
        } else if (fs_remove_file(target)) {
            perm_on_remove(target, tty_cwd());
            kputs("[+] File deleted successfully.\n", 0x55FF55);
        } else {
            kputs("[-] File not found or error deleting.\n", 0xFF5555);
        }

    // --- Управление системой и питанием ---
    } else if (strcmp(cmd, "ver") == 0) {
        kputs(OS_NAME " " OS_ARCH " (process patch) 1.0\n", 0xFFFF55);
    } else if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "r") == 0) {
        kputs("Rebooting system...\n", 0xFF5555);
        flush_buffer();
        sys_reboot();
    } else if (strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "s") == 0) {
        kputs("Shutting down...\n", 0xFF5555);
        flush_buffer();
        acpi_power_off();

    // --- Пользовательские программы Ring 3 (.PROG) ---
    } else if (is_prog_command(cmd)) {
        char prog_filename[32];
        copy_to_prog_name(cmd, prog_filename, sizeof(prog_filename));

        const char* args = cmd;
        while (*args && *args != ' ') args++;
        while (*args == ' ') args++;

        int res = prog_load_module(prog_filename, args);
        if (res != PROG_ENOENT) {
            // программа отработала (код выхода, в т.ч. отрицательный после сбоя/сигнала)
            // или загрузчик уже объяснил отказ
        } else {
            kputs("Unknown command: ", 0xFF5555);
            kputs(cmd, 0xFFFFFF);
            kputc('\n', 0xFFFFFF);
        }

    } else if (is_sys_command(cmd)) {
        char sys_filename[32];
        copy_to_sys_name(cmd, sys_filename, sizeof(sys_filename));

        // Извлекаем аргументы после имени модуля (например: "redactor.sys /TEST.TXT")
        const char* args = cmd;
        while (*args && *args != ' ') args++;
        while (*args == ' ') args++;

        if (!sys_load_module(sys_filename, args)) {
            kputs("Unknown command: ", 0xFF5555);
            kputs(cmd, 0xFFFFFF);
            kputc('\n', 0xFFFFFF);
        }
    } else {
        return 0;          // команду не знаем — пусть решает вызывающая оболочка
    }
    return 1;
}

