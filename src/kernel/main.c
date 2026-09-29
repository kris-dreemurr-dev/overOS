#include <stdint.h>
#include <stddef.h>
#include "../drivers/acpi.h"
#include "../drivers/pci.h"
#include "../drivers/usb/ehci-msc.h"
#include "../fs/fs.h"
#include "sys_loader.h"
#include "prog_loader.h"
#include "sched.h"
#include "config.h"

#define MAX_INPUT 256
extern char input_buffer[MAX_INPUT];
extern int input_len;
extern int kbd_layout;

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
extern int strcmp(const char* s1, const char* s2);

// Программы и визуальные режимы ядра
extern void enter_render_mode(void);
extern void kernel_panic(const char* reason);
extern void cmd_fastfetch(void);
extern void cmd_fastfetch_audio(void);
extern void acpi_power_off(void);
extern void cmd_ehci_log(void);
extern void run_nano_editor(void);
extern void intel_set_backlight(int level, int silent);

// Текущий путь в файловой системе (корень "/")
extern char current_path[128];

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

static void cmd_self_update(int raw_root_mode, int update_boot_sector) {
    kputs("\n[SELF-UPDATE] Starting kernel flash procedure...\n", 0x55FFFF);

    // Переходим в системный каталог /sys
    fs_go_root();
    fs_change_dir("sys");

    kputs("  -> Loading KERNEL.BIN from /sys partition...\n", 0xAAAAAA);
    int bytes = fs_read_file("KERNEL.BIN", file_buf, sizeof(file_buf));
    
    if (bytes <= 0) {
        if (raw_root_mode) {
            kputs("  [!] KERNEL.BIN missing! [RAW MODE (-r)] Forcing raw override with zero data...\n", 0xFFFF55);
            bytes = 0;
            for (int i = 0; i < 512; i++) file_buf[i] = 0;
        } else {
            kputs("  [!] KERNEL.BIN not found in /sys directory! Aborted.\n", 0xFF5555);
            fs_go_root();
            return;
        }
    }

    char buf[16];
    if (bytes > 0) {
        kputs("  [+] Image size: ", 0x55FF55); itoa(bytes, buf); kputs(buf, 0xFFFF55); kputs(" bytes.\n", 0);
    }

    uint16_t sectors_needed = (bytes > 0) ? (bytes + 511) / 512 : 600;
    if (sectors_needed > 2047) {
        kputs("  [!] File is too big for MBR Gap (> 2047 sectors)!\n", 0xFF5555);
        fs_go_root();
        return;
    }

    kputs("  -> Flashing kernel to LBA 1 in chunks...\n", 0xAAAAAA);
    
    uint32_t sectors_written = 0;
    int flash_error = 0;

    while (sectors_written < sectors_needed) {
        uint16_t chunk = sectors_needed - sectors_written;
        if (chunk > 32) chunk = 32;

        uint8_t* write_ptr = (bytes > 0) ? (file_buf + (sectors_written * 512)) : file_buf;

        if (!ehci_msc_write_sectors(1 + sectors_written, chunk, write_ptr)) {
            flash_error = 1;
            break;
        }
        sectors_written += chunk;
    }

    if (flash_error) {
        kputs("  [!] SCSI WRITE ERROR during chunked flash!\n", 0xFF5555);
        fs_go_root();
        return;
    }
    
    if (bytes > 0) {
        kputs("  [+] Kernel flashed successfully to LBA 1.\n", 0x55FF55);
    } else {
        kputs("  [!] RAW OVERWRITE: Kernel destroyed (-r active). System is bricked!\n", 0xFF5555);
    }

    if (update_boot_sector) {
        kputs("  -> Updating Stage0 in LBA 0 (preserving partition table)...\n", 0xAAAAAA);
        int boot_bytes = fs_read_file("BOOT.BIN", file_buf, 512);

        if (boot_bytes >= 446) {
            if (!ehci_msc_read_sectors(0, 1, mbr_tmp)) { fs_go_root(); return; }
            for (int i = 0; i < 446; i++) mbr_tmp[i] = file_buf[i];
            ehci_msc_write_sectors(0, 1, mbr_tmp);
            kputs("  [+] Bootloader stage0 updated safely in LBA 0!\n", 0x55FF55);
        } else {
            kputs("  [i] BOOT.BIN not found in /sys; LBA 0 left untouched.\n", 0xFFFF55);
        }
    }

    fs_go_root();
    kputs("\n[+] SUCCESS! Type 'reboot' to test.\n", 0x55FF55);
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

void execute_command(void) {
    kputc('\n', 0xFFFFFF);

    if (input_len <= 0) {
        input_len = 0;
        input_buffer[0] = '\0';
        print_prompt();
        return;
    }

    if (input_len >= MAX_INPUT) input_len = MAX_INPUT - 1;
    input_buffer[input_len] = '\0';

    char* cmd = input_buffer;
    while (*cmd == ' ') cmd++;

    if (*cmd == '\0') {
        input_len = 0;
        input_buffer[0] = '\0';
        print_prompt();
        return;
    }

    // --- Обработка встроенных команд ---
    if (strcmp(cmd, "clear") == 0) {
        clear_screen(0x000000);
    } else if (strcmp(cmd, "help") == 0) {
        kputs("================Command List================\n", 0x55FFFF);
        kputs("help                ", 0xFFFF55); kputs("- Show this help message\n", 0xFFFFFF);
        kputs("clear               ", 0xFFFF55); kputs("- Clear the screen\n", 0xFFFFFF);
        kputs("ver                 ", 0xFFFF55); kputs("- Display OS version\n", 0xFFFFFF);
        kputs("rus                 ", 0xFFFF55); kputs("- Turning Russian lang if ALT is broken\n", 0xFFFFFF);
        kputs("англ                ", 0xFFFF55); kputs("- Turning English lang if ALT is broken\n", 0xFFFFFF);

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
        kputs("insmod <file.sys>   ", 0xFFFF55); kputs("- Load Ring 0 module explicitly\n", 0xFFFFFF);

        kputs("\n--- Hardware & Diagnostics ---\n", 0x55FF55);
        kputs("fastfetch           ", 0xFFFF55); kputs("- Display system information\n", 0xFFFFFF);
        kputs("fastfetch -a        ", 0xFFFF55); kputs("- System info with audio controllers\n", 0xFFFFFF);
        kputs("brightness <0-100>  ", 0xFFFF55); kputs("- Set screen backlight brightness\n", 0xFFFFFF);
        kputs("ehci-log            ", 0xFFFF55); kputs("- Display EHCI host controller debug log\n", 0xFFFFFF);
        kputs("taskmgr             ", 0xFFFF55); kputs("- Task Manager\n", 0xFFFFFF);

        kputs("\n--- System Control ---\n", 0x55FF55);
        kputs("update [-r] [-b]    ", 0xFFFF55); kputs("- Self-update kernel (-r raw, -b bootloader)\n", 0xFFFFFF);
        kputs("reboot / r          ", 0xFFFF55); kputs("- Restart system\n", 0xFFFFFF);
        kputs("shutdown / s        ", 0xFFFF55); kputs("- Shutdown system via ACPI\n", 0xFFFFFF);

    // --- Встроенные приложения и диагностика ---
    } else if (strcmp(cmd, "render") == 0) {
        enter_render_mode();
    } else if (strcmp(cmd, "fastfetch -a") == 0 || strcmp(cmd, "fastfetch-audio") == 0) {
        cmd_fastfetch_audio();
    } else if (strcmp(cmd, "fastfetch") == 0) {
        cmd_fastfetch();
    } else if (strcmp(cmd, "ehci-log") == 0) {
        cmd_ehci_log();
    } else if (strcmp(cmd, "taskmgr") == 0) {
        sched_dump_tasks();

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
        fs_dir();
} else if (strncmp(cmd, "cd ", 3) == 0) {
        char* target = cmd + 3;
        while (*target == ' ') target++;

        // Убираем хвостовые пробелы
        int tlen = strlen(target);
        while (tlen > 0 && target[tlen - 1] == ' ') {
            target[--tlen] = '\0';
        }

        if (strcmp(target, "..") == 0) {
            if (fs_change_dir("..")) {
                int len = strlen(current_path);
                while (len > 1 && current_path[len - 1] != '/') {
                    current_path[len - 1] = '\0';
                    len--;
                }
                if (len > 1 && current_path[len - 1] == '/') {
                    current_path[len - 1] = '\0';
                }
            } else {
                kputs("[-] Already at root!\n", 0xFF5555);
            }
        } else if (strcmp(target, "/") == 0) {
            fs_go_root();
            current_path[0] = '/';
            current_path[1] = '\0';
        } else {
            if (fs_change_dir(target)) {
                if (strcmp(current_path, "/") != 0) {
                    strcat(current_path, "/");
                }
                const char* real_name = fs_get_last_dir_name();
                strcat(current_path, (real_name && real_name[0]) ? real_name : target);
            } else {
                kputs("[-] Directory not found!\n", 0xFF5555);
            }
        }
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        char* target = cmd + 4;
        while (*target == ' ') target++;
        int bytes = fs_read_file(target, file_buf, sizeof(file_buf) - 1);
        if (bytes > 0) {
            file_buf[bytes] = '\0';
            kputs((char*)file_buf, 0xFFFFFF);
            kputc('\n', 0xFFFFFF);
        } else {
            kputs("[-] File not found or read error.\n", 0xFF5555);
        }
    } else if (strncmp(cmd, "touch ", 6) == 0) {
        char* target = cmd + 6;
        while (*target == ' ') target++;
        if (fs_touch(target)) {
            kputs("[+] File created successfully.\n", 0x55FF55);
        } else {
            kputs("[-] Failed to create file (already exists or disk full).\n", 0xFF5555);
        }
    } else if (strncmp(cmd, "mkdir ", 6) == 0) {
        char* target = cmd + 6;
        while (*target == ' ') target++;
        if (fs_make_folder(target)) {
            kputs("[+] Directory created successfully.\n", 0x55FF55);
        } else {
            kputs("[-] Failed to create directory (already exists or disk full).\n", 0xFF5555);
        }
    } else if (strncmp(cmd, "rm ", 3) == 0) {
        char* target = cmd + 3;
        while (*target == ' ') target++;
        if (fs_remove_file(target)) {
            kputs("[+] File deleted successfully.\n", 0x55FF55);
        } else {
            kputs("[-] File not found or error deleting.\n", 0xFF5555);
        }

    // --- Управление системой и питанием ---
    } else if (strncmp(cmd, "update", 6) == 0) {
        int force_raw = 0;
        int update_boot = 0;
        for (int i = 6; cmd[i] != '\0'; i++) {
            if (cmd[i] == '-' && cmd[i + 1] == 'r') force_raw = 1;
            if (cmd[i] == '-' && cmd[i + 1] == 'b') update_boot = 1;
        }
        cmd_self_update(force_raw, update_boot);
    } else if (strcmp(cmd, "ver") == 0) {
        kputs(OS_NAME " " OS_ARCH " (process patch) 1.0\n", 0xFFFF55);
    } else if (strcmp(cmd, "rus") == 0) {
        kputs("lang turned to russian\n", 0xFFFF55);
        kbd_layout = 1;
    } else if (strcmp(cmd, "англ") == 0 || strcmp(cmd, "1972") == 0)  {
        kputs("lang turned to english\n", 0xFFFF55);
        kbd_layout = 0;
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
        if (res < 0) {
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
        kputs("Unknown command: ", 0xFF5555);
        kputs(cmd, 0xFFFFFF);
        kputc('\n', 0xFFFFFF);
    }

    input_len = 0;
    input_buffer[0] = '\0';

    print_prompt();
}

// Экспортируемая функция исполнения команд шелла из оконных модулей
void sys_exec_cmd(const char* cmd_str) {
    if (!cmd_str || cmd_str[0] == '\0') return;
    int i = 0;
    while (cmd_str[i] && i < MAX_INPUT - 1) {
        input_buffer[i] = cmd_str[i];
        i++;
    }
    input_buffer[i] = '\0';
    input_len = i;
    execute_command();
}
