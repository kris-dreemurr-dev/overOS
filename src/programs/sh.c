// sh.c — оболочка overOS в Ring 3 (/sys/sh.prg).
//
// Раньше оболочка жила в ядре (main.c): она разбирала команды, ходила по ФС и
// проверяла права с правами ядра — любая ошибка в разборе строки роняла систему.
// Теперь это обычная программа Ring 3: ввод читает из /dev/tty (строку собирает
// драйвер TTY, как в UNIX), файлы трогает через сисколы и проверку прав ядра, а
// при падении умирает только она — login перезапустит.
//
// Сервисные команды ядра (taskmgr, модули .sys, сеть, звук, демо) пока вызываются
// через сисколл 118: они ещё не переехали в userspace.

#include "ovos.h"
#include "contheme.h"

#define LINE_MAX 256
#define ARG_MAX  128

static char g_user[32] = "";
static int  g_session_uid = -1;
static int  g_session_gid = -1;

// ---------------------------------------------------------------- строки
static int s_len(const char* s) { int n = 0; while (s[n]) n++; return n; }

static int s_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int s_starts(const char* s, const char* pre) {
    while (*pre) { if (*s != *pre) return 0; s++; pre++; }
    return 1;
}

static void s_cpy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void s_cat(char* d, const char* s, int max) {
    int i = s_len(d), j = 0;
    while (s[j] && i < max - 1) d[i++] = s[j++];
    d[i] = '\0';
}

static void u_to_str(uint64_t v, char* out) {
    char tmp[24];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (i) out[j++] = tmp[--i];
    out[j] = '\0';
}

// ---------------------------------------------------------------- вывод
static void pr(const char* s)                 { ov_print(s); }
static void prc(const char* s, uint32_t c)    { ov_print_color(s, c); }
static void pr_num(uint64_t v) { char b[24]; u_to_str(v, b); pr(b); }

// Размер файла с единицами (Б/КБ/МБ/ГБ), как печатал встроенный dir ядра
static void pr_size(uint32_t bytes) {
    const char* unit = "Б";
    uint32_t whole = bytes, frac = 0;
    if (bytes >= 1024u * 1024u * 1024u) {
        whole = bytes / (1024u * 1024u * 1024u);
        frac  = (bytes % (1024u * 1024u * 1024u)) / (1024u * 1024u * 102u);   // одна цифра после точки
        unit  = "ГБ";
    } else if (bytes >= 1024u * 1024u) {
        whole = bytes / (1024u * 1024u);
        frac  = (bytes % (1024u * 1024u)) / (1024u * 102u);
        unit  = "МБ";
    } else if (bytes >= 1024u) {
        whole = bytes / 1024u;
        frac  = (bytes % 1024u) / 102u;
        unit  = "КБ";
    }
    pr_num(whole);
    if (frac > 0 && frac < 10) { pr("."); pr_num(frac); }
    pr(" ");
    pr(unit);
}

static void pr_pad(const char* s, int width) {
    pr(s);
    for (int i = s_len(s); i < width; i++) pr(" ");
}

// ---------------------------------------------------------------- приглашение
static void prompt(void) {
    char cwd[128];
    ov_getcwd(cwd, sizeof(cwd));
    prc(g_user[0] ? g_user : "user", CON_GREEN);
    prc("@overos", CON_GREEN);
    prc(":", CON_FG);
    prc(cwd, CON_CYAN);
    prc(ov_getuid() == 0 ? "# " : "$ ", CON_FG);
}

// Пароль: ядро на время гасит эхо (ioctl TTY_IOCTL_ECHO), как termios
static void ask_pass(const char* label, char* out, int max) {
    prc(label, CON_FG);
    ov_tty_echo(0);
    ov_readline(out, max);
    ov_tty_echo(1);
    pr("\n");
}

// ---------------------------------------------------------------- запуск программ
// Ошибки запуска из ядра (prog_loader.h): их нельзя путать с кодом выхода
// программы — убитая по Ctrl+C возвращает -2 (-SIGINT), а не "файл не найден".
#define PROG_EPERM   (-126)
#define PROG_ENOENT  (-127)
#define PROG_ENOEXEC (-125)
#define PROG_ENOMEM  (-124)

// Программа работает на переднем плане: ей отдаём клавиатуру TTY и ждём конца.
// Возвращает 0, если программа запустилась (её код выхода нас не интересует),
// или отрицательный PROG_E* — тогда запустить не удалось.
static int run_program(const char* path, const char* args) {
    int pid = ov_spawn(path, args, (uint64_t)-1);
    if (pid <= 0) return (pid == 0) ? PROG_ENOENT : pid;

    ov_tty_setfg(pid);
    int status = 0;
    while (ov_waitpid(pid, &status, 0) <= 0) ov_sleep(5);
    ov_tty_setfg(0);                  // клавиатура снова у оболочки
    return 0;
}

static int is_prog_name(const char* s) {
    int n = 0;
    while (s[n] && s[n] != ' ') n++;
    if (n < 5) return 0;
    const char* e = &s[n - 4];
    return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') && (e[2] == 'r' || e[2] == 'R')
                       && (e[3] == 'g' || e[3] == 'G');
}

// ---------------------------------------------------------------- команды ФС
static void cmd_ls(const char* arg) {
    char path[128];
    if (arg && arg[0]) s_cpy(path, arg, sizeof(path));
    else ov_getcwd(path, sizeof(path));

    static ov_dirent_t list[64];
    ov_memset(list, 0, sizeof(list));
    int n = ov_listdir(path, list, 64);
    if (n < 0) { prc("Доступ запрещён\n", CON_RED); return; }

    prc("ИМЯ                         РАЗМЕР\n", CON_DIM);
    for (int i = 0; i < n; i++) {
        const char* nm = list[i].clean_name[0] ? list[i].clean_name : list[i].name;
        int dir = (list[i].attr & OV_ATTR_DIR) != 0;
        pr_pad(nm, 28);
        if (dir) prc("<КАТАЛОГ>\n", CON_CYAN);
        else { pr_size(list[i].size); pr("\n"); }
    }
}

static void cmd_cat(const char* arg) {
    if (!arg || !arg[0]) { prc("cat: укажите файл\n", CON_RED); return; }
    int fd = ov_open(arg, OV_O_RDONLY);
    if (fd < 0) { prc("cat: нет файла или доступ запрещён\n", CON_RED); return; }

    static char buf[4096];
    int64_t n;
    while ((n = ov_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        pr(buf);
    }
    ov_close(fd);
    pr("\n");
}

// echo текст > файл — простая запись (перенаправления разбирает сама оболочка)
static void cmd_echo(char* arg) {
    char* redir = 0;
    for (char* p = arg; *p; p++) if (*p == '>') { redir = p; break; }

    if (!redir) { pr(arg); pr("\n"); return; }

    *redir = '\0';
    char* file = redir + 1;
    while (*file == ' ' || *file == '>') file++;
    int len = s_len(arg);
    while (len > 0 && arg[len - 1] == ' ') arg[--len] = '\0';

    ov_create(file);
    int fd = ov_open(file, OV_O_WRONLY);
    if (fd < 0) { prc("echo: не могу открыть файл\n", CON_RED); return; }
    ov_write(fd, arg, (uint64_t)len);
    ov_write(fd, "\n", 1);
    ov_close(fd);
}

// kill <pid> [сигнал] -- как в UNIX: по умолчанию SIGKILL (9)
static void cmd_kill(char* arg) {
    if (!arg || !*arg) { prc("kill: нужен PID\n", CON_RED); return; }
    int pid = 0;
    char* p = arg;
    while (*p >= '0' && *p <= '9') pid = pid * 10 + (*p++ - '0');
    while (*p == ' ') p++;
    int sig = 9;
    if (*p >= '0' && *p <= '9') { sig = 0; while (*p >= '0' && *p <= '9') sig = sig * 10 + (*p++ - '0'); }
    if (pid <= 0) { prc("kill: неверный PID\n", CON_RED); return; }
    if (ov_kill((uint64_t)pid, sig) != 0) prc("kill: нет такого процесса или нет прав\n", CON_RED);
}

// free -- сколько памяти в системе и сколько занято (как free -m)
static void cmd_free(void) {
    ov_meminfo_t mi;
    if (ov_meminfo(&mi) != 0) { prc("free: ядро не ответило\n", CON_RED); return; }
    uint64_t total = mi.total / (1024 * 1024), used = mi.used / (1024 * 1024);
    prc("            всего      занято    свободно\n", CON_DIM);
    char b[24];
    pr("Память: ");
    u_to_str(total, b); s_cat(b, " МБ", sizeof(b)); pr_pad(b, 12);
    u_to_str(used, b);  s_cat(b, " МБ", sizeof(b)); pr_pad(b, 12);
    u_to_str(total > used ? total - used : 0, b); s_cat(b, " МБ", sizeof(b)); pr(b);
    pr("\n");
}

// history -- список последних команд этого терминала (ведёт драйвер TTY)
static void cmd_history(void) {
    int n = ov_tty_history(-1, 0, 0);
    if (n <= 0) { prc("history: пока пусто\n", CON_DIM); return; }
    for (int i = 0; i < n; i++) {
        char line[192], num[16];
        int no = ov_tty_history(i, line, sizeof(line));
        if (no <= 0) continue;
        u_to_str((uint64_t)no, num);
        for (int s = s_len(num); s < 4; s++) pr(" ");
        prc(num, CON_DIM);
        pr("  ");
        prc(line, CON_FG);
        pr("\n");
    }
}

static void cmd_ps(void) {
    static ov_proc_t procs[64];
    int n = ov_ps(procs, 64);
    prc("  PID TTY   UID   СОСТОЯНИЕ  ПАМЯТЬ      ИМЯ\n", CON_DIM);
    for (int i = 0; i < n; i++) {
        char b[24];
        u_to_str((uint64_t)procs[i].pid, b);
        for (int s = s_len(b); s < 5; s++) pr(" ");
        pr(b);
        pr(" ");
        if (procs[i].tty >= 0) { pr("tty"); pr_num((uint64_t)procs[i].tty + 1); }
        else pr("  - ");
        pr("  ");
        u_to_str((uint64_t)procs[i].uid, b);
        pr_pad(b, 6);
        static const char* st[] = { "READY  ", "RUN    ", "SLEEP  ", "ZOMBIE ", "DEAD   " };
        pr_pad((procs[i].state >= 0 && procs[i].state <= 4) ? st[procs[i].state] : "?      ", 11);
        if (procs[i].mem_kb >= 1024) {
            u_to_str(procs[i].mem_kb / 1024, b);
            s_cat(b, " МБ", sizeof(b));
        } else {
            u_to_str(procs[i].mem_kb, b);
            s_cat(b, " КБ", sizeof(b));
        }
        pr_pad(b, 10);
        prc(procs[i].name, procs[i].is_process ? CON_GREEN : CON_YELLOW);
        pr("\n");
    }
}

static void cmd_id(void) {
    int uid = ov_getuid(), gid = ov_getgid();
    ov_user_t u;
    pr("uid=");
    pr_num((uint64_t)uid);
    if (ov_getpwuid(uid, &u)) { pr("("); pr(u.name); pr(")"); }
    pr(" gid=");
    pr_num((uint64_t)gid);
    if (ov_is_admin(uid)) pr(" группы=wheel");
    pr("\n");
}

static void cmd_stat(const char* path) {
    if (!path || !path[0]) { prc("stat: укажите путь\n", CON_RED); return; }
    ov_perm_t p;
    if (!ov_stat_perm(path, &p)) { prc("stat: нет такого пути\n", CON_RED); return; }
    ov_user_t u;
    pr("Путь:      "); pr(path); pr("\n");
    pr("Владелец:  ");
    if (ov_getpwuid(p.uid, &u)) pr(u.name); else pr_num((uint64_t)p.uid);
    pr(" (uid "); pr_num((uint64_t)p.uid); pr(", gid "); pr_num((uint64_t)p.gid); pr(")\n");
    pr("Права:     ");
    char oct[8];
    oct[0] = (char)('0' + ((p.mode >> 6) & 7));
    oct[1] = (char)('0' + ((p.mode >> 3) & 7));
    oct[2] = (char)('0' + (p.mode & 7));
    oct[3] = '\0';
    pr(oct); pr("\n");
}

// ---------------------------------------------------------------- сеанс
// sudo <команда>: процесс становится root по СВОЕМУ паролю (как sudo с wheel),
// запускает команду и сразу возвращает себе прежние права.
static int run_line(char* line);

static void cmd_sudo(char* rest) {
    if (!rest || !rest[0]) { prc("sudo: укажите команду\n", CON_RED); return; }
    if (ov_getuid() == 0) { run_line(rest); return; }
    if (!ov_is_admin(ov_getuid())) {
        prc("sudo: пользователь не в группе wheel\n", CON_RED);
        return;
    }

    char pass[64];
    ask_pass("[sudo] пароль: ", pass, sizeof(pass));
    if (ov_sudo(pass) != 0) {
        prc("sudo: неверный пароль\n", CON_RED);
        return;
    }
    run_line(rest);
    ov_setgid(g_session_gid);          // права сеанса обратно (мы ещё root — можно)
    ov_setuid(g_session_uid);
}

static void cmd_passwd(const char* who) {
    char name[32];
    if (who && who[0]) s_cpy(name, who, sizeof(name));
    else s_cpy(name, g_user, sizeof(name));

    char oldp[64] = "", p1[64], p2[64];
    if (ov_getuid() != 0) ask_pass("Текущий пароль: ", oldp, sizeof(oldp));
    ask_pass("Новый пароль: ", p1, sizeof(p1));
    ask_pass("Ещё раз: ", p2, sizeof(p2));
    if (!s_eq(p1, p2)) { prc("passwd: пароли не совпадают\n", CON_RED); return; }
    if (ov_passwd(name, p1, oldp) != 0) { prc("passwd: отказано\n", CON_RED); return; }
    prc("passwd: пароль обновлён\n", CON_GREEN);
}

static void cmd_useradd(char* rest) {
    int admin = 0;
    while (s_starts(rest, "-G wheel")) { admin = 1; rest += 8; while (*rest == ' ') rest++; }
    if (!rest[0]) { prc("useradd: укажите имя\n", CON_RED); return; }
    char p1[64], p2[64];
    ask_pass("Пароль: ", p1, sizeof(p1));
    ask_pass("Ещё раз: ", p2, sizeof(p2));
    if (!s_eq(p1, p2)) { prc("useradd: пароли не совпадают\n", CON_RED); return; }
    int uid = ov_useradd(rest, p1, admin);
    if (uid < 0) { prc("useradd: не удалось (нужен root или имя занято)\n", CON_RED); return; }
    prc("useradd: создан пользователь ", CON_GREEN);
    pr(rest);
    pr(" (uid ");
    pr_num((uint64_t)uid);
    pr(")\n");
}

// su [пользователь]: вложенная оболочка от его имени, exit возвращает обратно.
// Права меняет РЕБЁНОК после fork: обратно повысить себя мы бы уже не смогли
// (setuid — право root), и оболочка навсегда осталась бы чужим пользователем.
static void cmd_su(const char* who) {
    char name[32];
    s_cpy(name, (who && who[0]) ? who : "root", sizeof(name));

    char pass[64] = "";
    if (ov_getuid() != 0) ask_pass("Пароль: ", pass, sizeof(pass));

    int pid = ov_fork();
    if (pid == 0) {
        if (ov_su(name, pass) != 0) {
            prc("su: неверный пароль\n", CON_RED);
            ov_exit(1);
        }
        ov_execve("/sys/sh.prg", "");
        ov_exit(1);
    }
    if (pid < 0) { prc("su: не удалось создать процесс\n", CON_RED); return; }

    ov_tty_setfg(pid);
    int status = 0;
    while (ov_waitpid(pid, &status, 0) <= 0) ov_sleep(5);
    ov_tty_setfg(0);
}

// ---------------------------------------------------------------- разбор строки
static void help(void) {
    prc("Оболочка overOS (Ring 3)\n", CON_CYAN);
    prc("Файлы:   ", CON_YELLOW); pr("ls, cd, pwd, cat, touch, mkdir, rm, echo текст > файл, stat\n");
    prc("Сеанс:   ", CON_YELLOW); pr("whoami, id, su, sudo, passwd, useradd [-G wheel], chmod, chown, exit\n");
    prc("Система: ", CON_YELLOW); pr("ps, kill, free, ver, clear, help, reboot, shutdown\n");
    prc("История: ", CON_YELLOW); pr("стрелки вверх и вниз листают набранное, history — весь список\n");
    prc("Запуск:  ", CON_YELLOW); pr("имя.prg [аргументы], имя.sys (модуль ядра, нужен root)\n");
    prc("Прочее:  ", CON_YELLOW); pr("команды ядра (taskmgr, ping, playsound, fastfetch, render)\n");
}

// 0 — обычное завершение, 1 — выйти из оболочки (exit/logout)
static int run_line(char* line) {
    while (*line == ' ') line++;
    if (!*line) return 0;

    char whole[LINE_MAX];
    s_cpy(whole, line, sizeof(whole));   // целая строка для сервисных команд ядра

    // команда и остаток строки (аргументы)
    char* arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = '\0'; arg++; while (*arg == ' ') arg++; }
    const char* cmd = line;

    if (s_eq(cmd, "exit") || s_eq(cmd, "logout")) return 1;
    if (s_eq(cmd, "help")) { help(); return 0; }
    if (s_eq(cmd, "pwd")) {
        char cwd[128];
        ov_getcwd(cwd, sizeof(cwd));
        pr(cwd); pr("\n");
        return 0;
    }
    if (s_eq(cmd, "cd")) {
        const char* target = arg[0] ? arg : "/";
        if (ov_chdir(target) != 0) prc("cd: нет такого каталога или доступ запрещён\n", CON_RED);
        return 0;
    }
    if (s_eq(cmd, "ls") || s_eq(cmd, "dir")) { cmd_ls(arg); return 0; }
    if (s_eq(cmd, "cat")) { cmd_cat(arg); return 0; }
    if (s_eq(cmd, "echo")) { cmd_echo(arg); return 0; }
    if (s_eq(cmd, "touch")) {
        if (ov_create(arg) <= 0) prc("touch: не удалось создать файл\n", CON_RED);
        return 0;
    }
    if (s_eq(cmd, "mkdir")) {
        if (ov_mkdir(arg) <= 0) prc("mkdir: не удалось создать каталог\n", CON_RED);
        return 0;
    }
    if (s_eq(cmd, "whoami")) { pr(g_user[0] ? g_user : "root"); pr("\n"); return 0; }
    if (s_eq(cmd, "id")) { cmd_id(); return 0; }
    if (s_eq(cmd, "ps")) { cmd_ps(); return 0; }
    if (s_eq(cmd, "kill")) { cmd_kill(arg); return 0; }
    if (s_eq(cmd, "free")) { cmd_free(); return 0; }
    if (s_eq(cmd, "history")) { cmd_history(); return 0; }
    if (s_eq(cmd, "stat")) { cmd_stat(arg); return 0; }
    if (s_eq(cmd, "su")) { cmd_su(arg); return 0; }
    if (s_eq(cmd, "sudo")) { cmd_sudo(arg); return 0; }
    if (s_eq(cmd, "passwd")) { cmd_passwd(arg); return 0; }
    if (s_eq(cmd, "useradd")) { cmd_useradd(arg); return 0; }
    if (s_eq(cmd, "chmod")) {
        char* path = arg;
        while (*path && *path != ' ') path++;
        if (*path) { *path = '\0'; path++; while (*path == ' ') path++; }
        int mode = 0;
        for (const char* p = arg; *p >= '0' && *p <= '7'; p++) mode = mode * 8 + (*p - '0');
        if (ov_chmod(path, mode) != 0) prc("chmod: отказано\n", CON_RED);
        return 0;
    }
    if (s_eq(cmd, "chown")) {
        char* path = arg;
        while (*path && *path != ' ') path++;
        if (*path) { *path = '\0'; path++; while (*path == ' ') path++; }
        ov_user_t u;
        int uid = -1, gid = -1;
        if (arg[0] >= '0' && arg[0] <= '9') {
            uid = 0;
            for (const char* p = arg; *p >= '0' && *p <= '9'; p++) uid = uid * 10 + (*p - '0');
            gid = uid;
        } else {
            for (int i = 0; i < 64 && uid < 0; i++) {
                if (ov_getpwuid(i == 0 ? 0 : 999 + i, &u) && s_eq(u.name, arg)) { uid = u.uid; gid = u.gid; }
            }
        }
        if (uid < 0 || ov_chown(path, uid, gid) != 0) prc("chown: отказано\n", CON_RED);
        return 0;
    }
    if (s_eq(cmd, "reboot")) { ov_reboot(); return 0; }
    if (s_eq(cmd, "shutdown")) { ov_shutdown(); return 0; }

    // Программа Ring 3: имя.prg [аргументы]
    if (is_prog_name(cmd)) {
        char path[160];
        if (cmd[0] == '/') s_cpy(path, cmd, sizeof(path));
        else s_cpy(path, cmd, sizeof(path));
        int st = run_program(path, arg);
        if (st == PROG_ENOENT) {
            prc("Команда не найдена: ", CON_RED);
            pr(cmd);
            pr("\n");
        } else if (st == PROG_EPERM) {
            prc("Доступ запрещён: ", CON_RED);
            pr(cmd);
            pr("\n");
        } else if (st == PROG_ENOEXEC) {
            prc("Не программа для консоли или битый файл\n", CON_RED);
        } else if (st == PROG_ENOMEM) {
            prc("Недостаточно памяти\n", CON_RED);
        }
        return 0;
    }

    // Остальное (модули .sys, сервисные команды) пока исполняет ядро
    if (!ov_kcmd(whole)) {
        // Как PATH в Linux: "notify ..." -- это /programs/notify.prg
        int plain = cmd[0] != '\0';
        for (int i = 0; cmd[i]; i++) if (cmd[i] == '/' || cmd[i] == '.') plain = 0;
        if (plain) {
            char path[160];
            s_cpy(path, "/programs/", sizeof(path));
            int n = 0; while (path[n]) n++;
            for (int i = 0; cmd[i] && n < (int)sizeof(path) - 5; i++) path[n++] = cmd[i];
            path[n++] = '.'; path[n++] = 'p'; path[n++] = 'r'; path[n++] = 'g'; path[n] = '\0';
            int fd = ov_open(path, 0);
            if (fd >= 0) {
                ov_close(fd);
                if (run_program(path, arg) == 0) return 0;
            }
        }
        prc("Команда не найдена: ", CON_RED);
        pr(whole);
        pr("\n");
    }
    return 0;
}

int main(void) {
    g_session_uid = ov_getuid();
    g_session_gid = ov_getgid();
    ov_user_t me;
    if (ov_getpwuid(g_session_uid, &me)) s_cpy(g_user, me.name, sizeof(g_user));
    else s_cpy(g_user, g_session_uid == 0 ? "root" : "user", sizeof(g_user));

    ov_tty_setfg(0);                   // ввод TTY наш, пока не запустим программу
    ov_tty_hist(1);                    // с этого момента строки идут в историю команд

    char line[LINE_MAX];
    while (1) {
        prompt();
        int n = ov_readline(line, sizeof(line));
        if (n < 0) {                   // ввод TTY сейчас у другого процесса — подождём
            ov_tty_setfg(0);
            ov_sleep(50);
            continue;
        }
        if (run_line(line)) break;
    }
    ov_tty_hist(0);                    // дальше спрашивать будет login.prg -- имена в историю не пишем
    ov_exit(0);
    return 0;
}
