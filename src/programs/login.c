// login.c — вход в систему в Ring 3 (/sys/login.prg), аналог getty+login в UNIX.
//
// Ядро запускает эту программу от root на каждом TTY. Она спрашивает имя и
// пароль (эхо гасит драйвер TTY), проверяет их сисколлом 84, переключается на
// найденного пользователя (setuid/setgid — право root) и запускает его оболочку
// /sys/sh.prg. Когда оболочка закончится, login снова показывает приглашение.
//
// Раньше тем же занимался login.c внутри ядра: состояние сеанса лежало в tty_t,
// а строка пароля проходила через ядерный разбор команд.

#include "ovos.h"

static char g_hostname[32] = "overos";

static int s_len(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void s_cpy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void ask(const char* label, char* out, int max, int echo) {
    ov_print_color(label, 0xFFFFFF);
    ov_tty_echo(echo);
    ov_readline(out, max);
    ov_tty_echo(1);
    if (!echo) ov_print("\n");
}

// Запуск оболочки от имени пользователя. Сам login остаётся root (как getty в
// UNIX: он переживает сеанс и показывает приглашение следующему).
static void run_shell(int uid, int gid) {
    uint64_t uidgid = ((uint64_t)(uint32_t)uid << 32) | (uint32_t)gid;
    int pid = ov_spawn("/sys/sh.prg", "", uidgid);
    if (pid <= 0) {
        ov_print_color("login: не удалось запустить /sys/sh.prg\n", 0x00FF5555);
        ov_sleep(1000);
        return;
    }
    ov_tty_setfg(pid);
    int status = 0;
    while (ov_waitpid(pid, &status, 0) <= 0) ov_sleep(5);
    ov_tty_setfg(0);
}


// ============================================================================
// Первоначальная настройка в консоли (OOBE): язык -> часовой пояс -> вторая
// раскладка -> администратор. Раньше этот мастер жил в ядре (login.c, стадии
// LOGIN_STAGE_OOBE_*); теперь это обычный диалог программы Ring 3.
// ============================================================================
static int s_lang = OV_LANG_RU;

static const char* T(const char* ru, const char* en) { return s_lang == OV_LANG_EN ? en : ru; }

static void print_num(int v) {
    char b[16];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i++] = '0';
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) b[i++] = '-';
    char out[18];
    int j = 0;
    while (i) out[j++] = b[--i];
    out[j] = '\0';
    ov_print(out);
}

static int read_int(const char* label, int lo, int hi, int def) {
    char buf[32];
    while (1) {
        ov_print_color(label, 0x00FFFF55);
        ov_readline(buf, sizeof(buf));
        if (!buf[0]) return def;
        int v = 0, ok = 0;
        for (const char* p = buf; *p; p++) {
            if (*p < '0' || *p > '9') { ok = 0; break; }
            v = v * 10 + (*p - '0');
            ok = 1;
        }
        if (ok && v >= lo && v <= hi) return v;
        ov_print_color(T("Нужно число из списка\n", "Enter a number from the list\n"), 0x00FF5555);
    }
}

static void oobe_run(void) {
    ov_print_color("\n=== overOS setup / Первоначальная настройка ===\n\n", 0x0055FFFF);

    // 1. Язык системы
    ov_print("Select system language / Выберите язык системы:\n");
    ov_print("  1) Русский\n  2) English\n");
    s_lang = (read_int("> ", 1, 2, 1) == 2) ? OV_LANG_EN : OV_LANG_RU;

    // 2. Часовой пояс (страна/город по UTC)
    ov_print_color(T("\nЧасовой пояс (регион/город):\n", "\nTime zone (region/city):\n"), 0x0055FFFF);
    int count = ov_tz_info(-1, 0);
    if (count > 40) count = 40;
    ov_tz_t tz;
    for (int i = 0; i < count; i++) {
        if (!ov_tz_info(i, &tz)) continue;
        ov_print("  ");
        print_num(i + 1);
        ov_print(") ");
        ov_print(s_lang == OV_LANG_RU ? tz.city : tz.name);
        ov_print("  UTC");
        if (tz.offset >= 0) ov_print("+");
        print_num(tz.offset);
        ov_print("\n");
    }
    int tz_sel = read_int(T("Номер пояса: ", "Zone number: "), 1, count > 0 ? count : 1, 1) - 1;

    // 3. Вторая раскладка клавиатуры (основная — английская)
    ov_print_color(T("\nДополнительная раскладка (основная - English US):\n",
                     "\nSecond keyboard layout (main is English US):\n"), 0x0055FFFF);
    ov_print(T("  1) Русская (Alt+Shift)\n  2) Нет\n", "  1) Russian (Alt+Shift)\n  2) No\n"));
    int layout2 = (read_int("> ", 1, 2, 1) == 1) ? 1 : 0;

    ov_set_system(s_lang, tz_sel, layout2);

    // 4. Администратор (root остаётся заблокированным, права — через sudo)
    char name[32], p1[64], p2[64];
    while (1) {
        ov_print_color(T("\nИмя пользователя-администратора: ", "\nAdministrator user name: "), 0x0055FFFF);
        ov_readline(name, sizeof(name));
        if (!name[0]) continue;

        ask(T("Пароль: ", "Password: "), p1, sizeof(p1), 0);
        ask(T("Ещё раз: ", "Again: "), p2, sizeof(p2), 0);
        int same = 1;
        for (int i = 0; same; i++) {
            if (p1[i] != p2[i]) same = 0;
            if (!p1[i]) break;
        }
        if (!same) {
            ov_print_color(T("Пароли не совпадают\n", "Passwords do not match\n"), 0x00FF5555);
            continue;
        }
        if (ov_useradd(name, p1, 1) < 0) {
            ov_print_color(T("Не удалось создать пользователя\n", "Could not create the user\n"), 0x00FF5555);
            continue;
        }
        break;
    }

    ov_lock_user("root");      // как в Ubuntu: вход под root закрыт, остаётся sudo
    ov_oobe_finish();
    ov_memset(p1, 0, sizeof(p1));
    ov_memset(p2, 0, sizeof(p2));
    ov_print_color(T("\nНастройка завершена.\n", "\nSetup complete.\n"), 0x0055FF55);
}

int main(void) {
    char name[32], pass[64];

    // Пользователей ещё нет — сначала первоначальная настройка
    if (ov_oobe_state() == 1) oobe_run();

    while (1) {
        ov_print("\n");
        ov_print_color(g_hostname, 0x0055FF55);
        ask(" login: ", name, sizeof(name), 1);
        if (!name[0]) continue;

        ask("Пароль: ", pass, sizeof(pass), 0);

        int uid = ov_auth(name, pass);
        ov_memset(pass, 0, sizeof(pass));      // пароль не держим в памяти дольше нужного
        if (uid < 0) {
            ov_print_color("Неверное имя пользователя или пароль\n", 0x00FF5555);
            ov_sleep(700);                     // пауза против перебора, как в login(1)
            continue;
        }

        ov_user_t info;
        if (!ov_getpwuid(uid, &info)) {
            ov_print_color("login: нет такой учётной записи\n", 0x00FF5555);
            continue;
        }

        ov_print_color("Добро пожаловать в overOS, ", 0x0055FF55);
        ov_print_color(info.name, 0x0055FF55);
        ov_print_color("!\n", 0x0055FF55);

        // Оболочка стартует в домашнем каталоге пользователя (его наследует потомок)
        char home[64];
        s_cpy(home, info.home[0] ? info.home : "/", sizeof(home));
        if (s_len(home) > 0) ov_chdir(home);

        run_shell(info.uid, info.gid);
        ov_chdir("/");

        // Оболочка закончилась: login снова root (он им и остался в своём процессе)
        ov_print_color("\nСеанс завершён.\n", 0x00FFFF55);
    }

    ov_exit(0);
    return 0;
}
