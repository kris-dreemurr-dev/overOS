#include "login.h"
#include "users.h"
#include "tty.h"
#include "sched.h"
#include "config.h"
#include "../fs/fs.h"

extern void kputs(const char* str, uint32_t color);
extern void kputc(char c, uint32_t color);
extern void flush_buffer(void);
extern void clear_screen(uint32_t color);
extern void print_tty_banner(int tty_id);
extern void sleep_ms(uint32_t ms);

#include "contheme.h"

#define C_ERR   CON_RED
#define C_OK    CON_GREEN
#define C_INFO  CON_DIM
#define C_TEXT  CON_FG

// ----------------------------------------------------------------------------
// Мелочи
// ----------------------------------------------------------------------------
static int l_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static int l_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void l_strcpy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void l_wipe(char* s, int n) {
    volatile char* v = s;
    for (int i = 0; i < n; i++) v[i] = 0;
}

// "cmd" или "cmd <args>": возвращает аргументы или NULL
static char* l_cmd(char* line, const char* name) {
    int n = l_strlen(name);
    for (int i = 0; i < n; i++) if (line[i] != name[i]) return 0;
    if (line[n] == '\0') return line + n;
    if (line[n] != ' ') return 0;
    char* a = line + n;
    while (*a == ' ') a++;
    return a;
}

// Следующее слово из строки (на месте)
static char* l_word(char** p) {
    char* s = *p;
    while (*s == ' ') s++;
    if (!*s) { *p = s; return 0; }
    char* w = s;
    while (*s && *s != ' ') s++;
    if (*s) *s++ = '\0';
    *p = s;
    return w;
}

static void l_print_int(int v) {
    char b[16];
    int n = 0;
    if (v < 0) { kputc('-', C_TEXT); v = -v; }
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 15);
    while (n) kputc(b[--n], C_TEXT);
}

// Ввод и вывод оболочки всегда идут через активный TTY: keyboard_poll_handler работает
// только в задаче-оболочке активного TTY, а tty_switch печатает приглашение нового TTY
// из контекста любой задачи — поэтому берём активный, а не TTY текущей задачи.
static tty_t* my_tty(void) {
    return tty_get(tty_get_active_id());
}

// Учётные данные оболочки: TTY + задача, от которой запускаются программы
static void apply_creds(tty_t* tty, int uid, int gid, const char* name) {
    tty->uid = uid;
    tty->gid = gid;
    l_strcpy(tty->login_name, name, sizeof(tty->login_name));
    task_t* t = sched_get_current_task();
    if (t) { t->uid = uid; t->gid = gid; }
}

// Переход оболочки в каталог по абсолютному пути. 0 — каталога нет, оболочка остаётся где была.
// Позицию FAT-драйвера двигать больше не нужно: её выставляет каждая операция
// по каталогу вызывающего (fs_caller_cwd), а здесь меняется только путь TTY.
int login_chdir_abs(const char* path) {
    if (!path || !path[0]) path = "/";
    fs_lock();
    int ok = fs_dir_exists(path);
    fs_unlock();
    if (ok) l_strcpy(tty_cwd(), path, 128);
    return ok;
}

static int shell_chdir_abs(const char* path) {
    if (login_chdir_abs(path)) return 1;
    login_chdir_abs("/");
    return 0;
}

// ----------------------------------------------------------------------------
// Вход / выход
// ----------------------------------------------------------------------------
static void session_start(tty_t* tty, int uid) {
    user_info_t info;
    if (!users_find_by_uid(uid, &info)) {
        l_strcpy(info.name, "?", sizeof(info.name));
        info.gid = uid;
        l_strcpy(info.home, "/", sizeof(info.home));
    }
    apply_creds(tty, uid, info.gid, info.name);
    tty->su_depth = 0;
    tty->login_stage = LOGIN_STAGE_SHELL;

    if (!shell_chdir_abs(info.home)) {
        kputs("No directory, logging in with HOME=/\n", C_ERR);
    }
    kputs("\nWelcome to " OS_NAME ", ", C_OK);
    kputs(info.name, C_TEXT);
    kputs("!\n\n", C_OK);
    fs_dir();
    kputc('\n', C_TEXT);
}

static void session_end(tty_t* tty) {
    tty->su_depth = 0;
    tty->login_stage = LOGIN_STAGE_USER;
    tty->login_name[0] = '\0';
    // Оболочка ядра снова принадлежит root (как getty), но команды до входа не принимает
    apply_creds(tty, 0, 0, "");
    tty->login_name[0] = '\0';
    shell_chdir_abs("/");
    clear_screen(0x000000);
    print_tty_banner(tty->id);
}

int login_wants_line(void) {
    tty_t* t = my_tty();
    return t && t->login_stage != LOGIN_STAGE_SHELL;
}

int login_echo_off(void) {
    tty_t* t = my_tty();
    if (!t) return 0;
    switch (t->login_stage) {
        case LOGIN_STAGE_SHELL: case LOGIN_STAGE_USER: case LOGIN_STAGE_OOBE_NAME:
        case LOGIN_STAGE_OOBE_LANG: case LOGIN_STAGE_OOBE_TZ: case LOGIN_STAGE_OOBE_LAYOUT:
            return 0;
        default:
            return 1;
    }
}

// Строка на языке первоначальной настройки (до её конца — выбранный в мастере, потом системный)
#define OL(t, ru, en) ((t)->oobe_lang == SYS_LANG_EN ? (en) : (ru))

static void print_offset(int off) {
    kputs(off < 0 ? "-" : "+", C_INFO);
    l_print_int(off < 0 ? -off : off);
}

// Первоначальная настройка в консоли (когда desktop нет или он не запущен):
// язык -> часовой пояс -> вторая раскладка -> пользователь-администратор
static void oobe_begin(tty_t* t) {
    t->oobe_lang = SYS_LANG_RU;
    t->oobe_tz = -1;
    t->oobe_layout2 = 1;
    kputs("\n=== " OS_NAME " setup / Первоначальная настройка ===\n\n", CON_CYAN);
    kputs("Select system language / Выберите язык системы:\n", C_TEXT);
    kputs("  1) Русский\n  2) English\n\n", C_TEXT);
    t->login_stage = LOGIN_STAGE_OOBE_LANG;
}

static void oobe_show_tz(tty_t* t) {
    kputs(OL(t, "\nЧасовой пояс (Регион/Город):\n", "\nTime zone (Region/City):\n"), CON_CYAN);
    for (int i = 0; i < g_timezone_count; i++) {
        kputs("  ", C_TEXT);
        if (i + 1 < 10) kputc(' ', C_TEXT);
        l_print_int(i + 1);
        kputs(") ", C_TEXT);
        kputs(g_timezones[i].name, C_TEXT);
        kputs("  UTC", C_INFO);
        print_offset(g_timezones[i].offset);
        if (t->oobe_lang == SYS_LANG_RU) { kputs("  ", C_INFO); kputs(g_timezones[i].city_ru, C_INFO); }
        kputc('\n', C_TEXT);
    }
    kputc('\n', C_TEXT);
}

static void oobe_show_layout(tty_t* t) {
    kputs(OL(t, "\nДополнительная раскладка клавиатуры (основная - английская):\n",
                "\nAdditional keyboard layout (primary is English US):\n"), CON_CYAN);
    kputs(OL(t, "  1) Русская (переключение - Alt)\n  2) Нет\n\n",
                "  1) Russian (switch with Alt)\n  2) None\n\n"), C_TEXT);
}

static void oobe_show_user(tty_t* t) {
    kputs(OL(t, "\nСоздание пользователя. Он будет администратором (sudo).\n",
                "\nCreate your user. It will be an administrator (sudo).\n"), CON_CYAN);
    kputs(OL(t, "Имя: строчные латинские буквы, цифры, '_' и '-'.\n\n",
                "Name: lowercase latin letters, digits, '_' and '-'.\n\n"), C_INFO);
}

// Число из строки ввода (0 — не число)
static int l_atoi(const char* s) {
    while (*s == ' ') s++;
    int v = 0, any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    return any ? v : 0;
}

void login_print_prompt(void) {
    tty_t* t = my_tty();
    if (!t) return;
    switch (t->login_stage) {
        case LOGIN_STAGE_USER:
            if (users_needs_oobe()) {
                oobe_begin(t);
                kputs("> ", C_TEXT);
                break;
            }
            kputs(OS_LOWER_NAME " login: ", C_TEXT);
            break;
        case LOGIN_STAGE_OOBE_LANG:
        case LOGIN_STAGE_OOBE_TZ:
        case LOGIN_STAGE_OOBE_LAYOUT:
            kputs("> ", C_TEXT);
            break;
        case LOGIN_STAGE_OOBE_NAME:
            kputs(OL(t, "Имя пользователя: ", "Username: "), C_TEXT);
            break;
        case LOGIN_STAGE_OOBE_PASS:
            kputs(OL(t, "Пароль: ", "Password: "), C_TEXT);
            break;
        case LOGIN_STAGE_OOBE_PASS2:
            kputs(OL(t, "Повторите пароль: ", "Retype password: "), C_TEXT);
            break;
        case LOGIN_STAGE_SUDO_PASS:
            kputs("[sudo] password for ", C_TEXT);
            kputs(t->login_name, C_TEXT);
            kputs(": ", C_TEXT);
            break;
        case LOGIN_STAGE_PASS:
        case LOGIN_STAGE_SU_PASS:
            kputs("Password: ", C_TEXT);
            break;
        case LOGIN_STAGE_PASSWD_OLD:
            kputs("Current password: ", C_TEXT);
            break;
        case LOGIN_STAGE_PASSWD_NEW:
        case LOGIN_STAGE_ADD_PASS:
            kputs("New password: ", C_TEXT);
            break;
        case LOGIN_STAGE_PASSWD_NEW2:
        case LOGIN_STAGE_ADD_PASS2:
            kputs("Retype new password: ", C_TEXT);
            break;
        default: {
            int root = (t->uid == UID_ROOT);
            kputs(t->login_name[0] ? t->login_name : "?", root ? C_ERR : C_OK);
            kputs("@" OS_LOWER_NAME ":", root ? C_ERR : C_OK);
            kputs(tty_cwd(), CON_CYAN);
            kputs(root ? "# " : "$ ", C_TEXT);
            break;
        }
    }
    flush_buffer();
}

static void su_push(tty_t* t) {
    if (t->su_depth >= 4) return;
    t->su_uid[t->su_depth] = t->uid;
    t->su_gid[t->su_depth] = t->gid;
    l_strcpy(t->su_name[t->su_depth], t->login_name, 32);
    t->su_depth++;
}

static void su_become(tty_t* t, const char* name) {
    user_info_t info;
    if (!users_find_by_name(name, &info)) return;
    su_push(t);
    apply_creds(t, info.uid, info.gid, info.name);
}

void login_handle_line(char* line) {
    tty_t* t = my_tty();
    if (!t) return;
    int len = l_strlen(line);

    switch (t->login_stage) {
        case LOGIN_STAGE_OOBE_LANG: {
            int n = l_atoi(line);
            if (n != 1 && n != 2) { kputs("1 / 2\n", C_ERR); break; }
            t->oobe_lang = (n == 2) ? SYS_LANG_EN : SYS_LANG_RU;
            oobe_show_tz(t);
            t->login_stage = LOGIN_STAGE_OOBE_TZ;
            break;
        }

        case LOGIN_STAGE_OOBE_TZ: {
            int n = l_atoi(line);
            if (n < 1 || n > g_timezone_count) {
                kputs(OL(t, "Введите номер из списка\n", "Enter a number from the list\n"), C_ERR);
                break;
            }
            t->oobe_tz = n - 1;
            oobe_show_layout(t);
            t->login_stage = LOGIN_STAGE_OOBE_LAYOUT;
            break;
        }

        case LOGIN_STAGE_OOBE_LAYOUT: {
            int n = l_atoi(line);
            if (n != 1 && n != 2) { kputs("1 / 2\n", C_ERR); break; }
            t->oobe_layout2 = (n == 1);
            oobe_show_user(t);
            t->login_stage = LOGIN_STAGE_OOBE_NAME;
            break;
        }

        case LOGIN_STAGE_OOBE_NAME: {
            char* p = line;
            while (*p == ' ') p++;
            if (!*p) break;
            user_info_t info;
            if (!users_valid_name(p)) {
                kputs(OL(t, "Недопустимое имя. Пример: kris, user_1\n", "Invalid name. Example: kris, user_1\n"), C_ERR);
            } else if (users_find_by_name(p, &info)) {
                kputs(OL(t, "Такой пользователь уже есть\n", "User already exists\n"), C_ERR);
            } else {
                l_strcpy(t->pending_name, p, sizeof(t->pending_name));
                t->login_stage = LOGIN_STAGE_OOBE_PASS;
            }
            break;
        }

        case LOGIN_STAGE_OOBE_PASS:
            if (len == 0) { kputs(OL(t, "Пароль не может быть пустым\n", "Password cannot be empty\n"), C_ERR); break; }
            l_strcpy(t->pending_pass, line, sizeof(t->pending_pass));
            t->login_stage = LOGIN_STAGE_OOBE_PASS2;
            break;

        case LOGIN_STAGE_OOBE_PASS2:
            if (!l_streq(line, t->pending_pass)) {
                kputs(OL(t, "Пароли не совпадают, попробуйте еще раз\n", "Passwords do not match, try again\n"), C_ERR);
                l_wipe(t->pending_pass, sizeof(t->pending_pass));
                t->login_stage = LOGIN_STAGE_OOBE_PASS;
                break;
            }
            if (!users_needs_oobe()) {
                // Пока мы тут вводили, настройку уже завершили (например, в desktop)
                kputs(OL(t, "Настройка уже выполнена.\n", "Setup is already done.\n"), C_INFO);
            } else {
                int uid = users_add(t->pending_name, t->pending_pass, 1);
                if (uid < 0) {
                    kputs(OL(t, "Не удалось создать пользователя\n", "Failed to create user\n"), C_ERR);
                    t->login_stage = LOGIN_STAGE_OOBE_NAME;
                    break;
                }
                users_lock("root");      // как в Ubuntu/CachyOS-установщиках: root без пароля, права — через sudo
                users_set_system(t->oobe_lang, t->oobe_tz, t->oobe_layout2);
                users_finish_oobe();
                kputs(OL(t, "\nГотово! Пользователь ", "\nDone! User "), C_OK);
                kputs(t->pending_name, C_TEXT);
                kputs(OL(t, " создан (администратор, sudo). Теперь войдите в систему.\n\n",
                            " created (administrator, sudo). Now log in.\n\n"), C_OK);
            }
            t->pending_name[0] = '\0';
            t->login_stage = LOGIN_STAGE_USER;
            break;

        case LOGIN_STAGE_SUDO_PASS: {
            t->login_stage = LOGIN_STAGE_SHELL;
            int ok = users_is_admin(t->uid) && users_auth(t->login_name, line) == t->uid;
            if (!ok) {
                sleep_ms(1000);
                if (!users_is_admin(t->uid)) {
                    kputs(t->login_name, C_ERR);
                    kputs(" is not in the sudoers file (group wheel).\n", C_ERR);
                } else {
                    kputs("sudo: incorrect password\n", C_ERR);
                }
                t->sudo_cmd[0] = '\0';
                break;
            }
            if (t->sudo_cmd[0] == '\0') {
                // sudo -i / sudo -s / sudo su: оболочка root до exit
                user_info_t r;
                if (users_find_by_uid(0, &r)) { su_push(t); apply_creds(t, 0, 0, r.name); }
            } else {
                t->sudo_ready = 1;
            }
            break;
        }

        case LOGIN_STAGE_USER: {
            char* p = line;
            while (*p == ' ') p++;
            if (!*p) break;
            l_strcpy(t->pending_name, p, sizeof(t->pending_name));
            t->login_stage = LOGIN_STAGE_PASS;
            break;
        }

        case LOGIN_STAGE_PASS: {
            int uid = users_auth(t->pending_name, line);
            if (uid < 0) {
                sleep_ms(1500);                    // как pam_faildelay
                kputs("\nLogin incorrect\n", C_ERR);
                t->login_stage = LOGIN_STAGE_USER;
            } else {
                session_start(t, uid);
            }
            t->pending_name[0] = '\0';
            break;
        }

        case LOGIN_STAGE_SU_PASS: {
            t->login_stage = LOGIN_STAGE_SHELL;
            user_info_t info;
            if (users_find_by_name(t->pending_name, &info) && users_auth(t->pending_name, line) == info.uid) {
                su_become(t, t->pending_name);
            } else {
                sleep_ms(1000);
                kputs("su: Authentication failure\n", C_ERR);
            }
            break;
        }

        case LOGIN_STAGE_PASSWD_OLD: {
            user_info_t info;
            if (users_find_by_name(t->pending_name, &info) && users_auth(t->pending_name, line) == info.uid) {
                l_strcpy(t->pending_old, line, sizeof(t->pending_old));
                t->login_stage = LOGIN_STAGE_PASSWD_NEW;
            } else {
                sleep_ms(1000);
                kputs("passwd: Authentication token manipulation error\n", C_ERR);
                t->login_stage = LOGIN_STAGE_SHELL;
            }
            break;
        }

        case LOGIN_STAGE_PASSWD_NEW:
        case LOGIN_STAGE_ADD_PASS:
            if (len == 0) {
                kputs("No password has been supplied.\n", C_ERR);
                t->login_stage = LOGIN_STAGE_SHELL;
                break;
            }
            l_strcpy(t->pending_pass, line, sizeof(t->pending_pass));
            t->login_stage = (t->login_stage == LOGIN_STAGE_ADD_PASS) ? LOGIN_STAGE_ADD_PASS2
                                                                      : LOGIN_STAGE_PASSWD_NEW2;
            break;

        case LOGIN_STAGE_PASSWD_NEW2:
        case LOGIN_STAGE_ADD_PASS2: {
            int is_add = (t->login_stage == LOGIN_STAGE_ADD_PASS2);
            t->login_stage = LOGIN_STAGE_SHELL;
            if (!l_streq(line, t->pending_pass)) {
                kputs("Sorry, passwords do not match.\n", C_ERR);
                break;
            }
            if (is_add) {
                int uid = users_add(t->pending_name, t->pending_pass, t->pending_admin);
                if (uid < 0) {
                    kputs("useradd: cannot create user ", C_ERR);
                    kputs(t->pending_name, C_TEXT);
                    kputc('\n', C_TEXT);
                } else {
                    kputs("useradd: created ", C_OK);
                    kputs(t->pending_name, C_TEXT);
                    kputs(" (uid ", C_OK); l_print_int(uid);
                    kputs(", home /home/", C_OK); kputs(t->pending_name, C_TEXT);
                    kputs(")\n", C_OK);
                }
            } else if (users_set_password(t->pending_name, t->pending_pass)) {
                kputs("passwd: password updated successfully\n", C_OK);
            } else {
                kputs("passwd: failed to update password\n", C_ERR);
            }
            break;
        }

        default:
            break;
    }

    // Пароли из памяти стираем, как только сценарий (passwd/useradd) закончился
    if (t->login_stage == LOGIN_STAGE_SHELL || t->login_stage == LOGIN_STAGE_USER) {
        l_wipe(t->pending_pass, sizeof(t->pending_pass));
        l_wipe(t->pending_old, sizeof(t->pending_old));
    }
    l_wipe(line, len);
}

int login_take_sudo_cmd(char* out, int max) {
    tty_t* t = my_tty();
    if (!t || !t->sudo_ready) return 0;
    t->sudo_ready = 0;
    l_strcpy(out, t->sudo_cmd, max);
    t->sudo_cmd[0] = '\0';
    // Временно root только на эту команду
    t->sudo_uid = t->uid;
    t->sudo_gid = t->gid;
    l_strcpy(t->sudo_name, t->login_name, sizeof(t->sudo_name));
    t->sudo_once = 1;
    user_info_t r;
    apply_creds(t, 0, 0, users_find_by_uid(0, &r) ? r.name : "root");
    return 1;
}

void login_after_command(void) {
    tty_t* t = my_tty();
    if (!t || !t->sudo_once) return;
    t->sudo_once = 0;
    apply_creds(t, t->sudo_uid, t->sudo_gid, t->sudo_name);
}

// ----------------------------------------------------------------------------
// Права для встроенных команд оболочки
// ----------------------------------------------------------------------------
int login_may(const char* path, int want) {
    tty_t* t = my_tty();
    int uid = t ? t->uid : 0, gid = t ? t->gid : 0;
    if (perm_check(uid, gid, path, tty_cwd(), want)) return 1;
    kputs("Permission denied: ", C_ERR);
    kputs(path, C_TEXT);
    kputc('\n', C_TEXT);
    return 0;
}

int login_may_parent(const char* path, int want) {
    tty_t* t = my_tty();
    int uid = t ? t->uid : 0, gid = t ? t->gid : 0;
    if (perm_check_parent(uid, gid, path, tty_cwd(), want)) return 1;
    kputs("Permission denied: ", C_ERR);
    kputs(path, C_TEXT);
    kputc('\n', C_TEXT);
    return 0;
}

// ----------------------------------------------------------------------------
// Команды сеанса
// ----------------------------------------------------------------------------
static void print_user_group(int uid, int gid) {
    user_info_t info;
    char gname[32];
    kputs("uid=", C_INFO); l_print_int(uid);
    kputs("(", C_INFO); kputs(users_find_by_uid(uid, &info) ? info.name : "?", C_TEXT); kputs(")", C_INFO);
    users_group_name(gid, gname, sizeof(gname));
    kputs(" gid=", C_INFO); l_print_int(gid);
    kputs("(", C_INFO); kputs(gname, C_TEXT); kputs(")\n", C_INFO);
}

int login_builtin(char* cmd) {
    tty_t* t = my_tty();
    if (!t) return 0;
    char* a;

    if ((a = l_cmd(cmd, "whoami"))) {
        kputs(t->login_name, C_TEXT);
        kputc('\n', C_TEXT);
        return 1;
    }

    if ((a = l_cmd(cmd, "id"))) {
        print_user_group(t->uid, t->gid);
        return 1;
    }

    if ((a = l_cmd(cmd, "logout"))) {
        session_end(t);
        return 1;
    }

    if ((a = l_cmd(cmd, "exit"))) {
        if (t->su_depth > 0) {
            t->su_depth--;
            apply_creds(t, t->su_uid[t->su_depth], t->su_gid[t->su_depth], t->su_name[t->su_depth]);
        } else {
            session_end(t);
        }
        return 1;
    }

    if ((a = l_cmd(cmd, "su"))) {
        char* p = a;
        char* name = l_word(&p);
        if (!name || l_streq(name, "-")) name = "root";
        user_info_t info;
        if (!users_find_by_name(name, &info)) {
            kputs("su: user ", C_ERR); kputs(name, C_TEXT); kputs(" does not exist\n", C_ERR);
            return 1;
        }
        if (t->su_depth >= 4) {
            kputs("su: too many nested sessions, use exit\n", C_ERR);
            return 1;
        }
        if (t->uid == UID_ROOT) {
            su_become(t, name);                  // root переключается без пароля
        } else {
            l_strcpy(t->pending_name, name, sizeof(t->pending_name));
            t->login_stage = LOGIN_STAGE_SU_PASS;
        }
        return 1;
    }

    if ((a = l_cmd(cmd, "passwd"))) {
        char* p = a;
        char* name = l_word(&p);
        if (!name) name = t->login_name;
        user_info_t info;
        if (!users_find_by_name(name, &info)) {
            kputs("passwd: user '", C_ERR); kputs(name, C_TEXT); kputs("' does not exist\n", C_ERR);
            return 1;
        }
        if (t->uid != UID_ROOT && info.uid != t->uid) {
            kputs("passwd: You may not view or modify password information for ", C_ERR);
            kputs(name, C_TEXT); kputs(".\n", C_ERR);
            return 1;
        }
        l_strcpy(t->pending_name, name, sizeof(t->pending_name));
        kputs("Changing password for ", C_INFO); kputs(name, C_TEXT); kputs(".\n", C_INFO);
        t->login_stage = (t->uid == UID_ROOT) ? LOGIN_STAGE_PASSWD_NEW : LOGIN_STAGE_PASSWD_OLD;
        return 1;
    }

    if ((a = l_cmd(cmd, "sudo"))) {
        char* p = a;
        while (*p == ' ') p++;
        if (!*p) {
            kputs("usage: sudo <command> | sudo -i\n", C_ERR);
            return 1;
        }
        int shell = l_streq(p, "-i") || l_streq(p, "-s") || l_streq(p, "su") || l_streq(p, "su -");
        if (shell) t->sudo_cmd[0] = '\0';
        else l_strcpy(t->sudo_cmd, p, sizeof(t->sudo_cmd));

        if (t->uid == UID_ROOT) {           // уже root — пароль не нужен
            if (!shell) t->sudo_ready = 1;
            return 1;
        }
        if (!users_is_admin(t->uid)) {
            kputs(t->login_name, C_ERR);
            kputs(" is not in the sudoers file (group wheel).\n", C_ERR);
            t->sudo_cmd[0] = '\0';
            return 1;
        }
        t->login_stage = LOGIN_STAGE_SUDO_PASS;
        return 1;
    }

    if ((a = l_cmd(cmd, "useradd"))) {
        char* p = a;
        // useradd [-G wheel] <name>
        int admin = 0;
        if (p[0] == '-' && p[1] == 'G') {
            l_word(&p);
            char* grp = l_word(&p);
            if (grp && l_streq(grp, "wheel")) admin = 1;
        }
        t->pending_admin = admin;
        char* name = l_word(&p);
        if (t->uid != UID_ROOT) {
            kputs("useradd: Permission denied.\n", C_ERR);
            return 1;
        }
        if (!name) {
            kputs("Usage: useradd [-G wheel] <name>\n", C_ERR);
            return 1;
        }
        user_info_t info;
        if (users_find_by_name(name, &info)) {
            kputs("useradd: user '", C_ERR); kputs(name, C_TEXT); kputs("' already exists\n", C_ERR);
            return 1;
        }
        l_strcpy(t->pending_name, name, sizeof(t->pending_name));
        t->login_stage = LOGIN_STAGE_ADD_PASS;
        return 1;
    }

    if ((a = l_cmd(cmd, "chmod"))) {
        char* p = a;
        char* mode_s = l_word(&p);
        char* path = l_word(&p);
        if (!mode_s || !path) {
            kputs("Usage: chmod <octal mode> <path>   (e.g. chmod 750 /home/user)\n", C_ERR);
            return 1;
        }
        int mode = 0;
        for (char* m = mode_s; *m; m++) {
            if (*m < '0' || *m > '7') { mode = -1; break; }
            mode = mode * 8 + (*m - '0');
        }
        if (mode < 0 || mode > 0777) {
            kputs("chmod: invalid mode: ", C_ERR); kputs(mode_s, C_TEXT); kputc('\n', C_TEXT);
            return 1;
        }
        char n[PERM_PATH_MAX];
        perm_normalize(path, tty_cwd(), n, sizeof(n));
        perm_info_t pi;
        perm_get(n, &pi);
        if (t->uid != UID_ROOT && t->uid != pi.uid) {
            kputs("chmod: changing permissions of '", C_ERR); kputs(n, C_TEXT);
            kputs("': Operation not permitted\n", C_ERR);
            return 1;
        }
        perm_set(n, "/", pi.uid, pi.gid, mode);
        return 1;
    }

    if ((a = l_cmd(cmd, "chown"))) {
        char* p = a;
        char* owner = l_word(&p);
        char* path = l_word(&p);
        if (!owner || !path) {
            kputs("Usage: chown <user>[:<group>] <path>\n", C_ERR);
            return 1;
        }
        if (t->uid != UID_ROOT) {
            kputs("chown: Operation not permitted\n", C_ERR);
            return 1;
        }
        char* grp = 0;
        for (char* c = owner; *c; c++) if (*c == ':') { *c = '\0'; grp = c + 1; break; }
        user_info_t u;
        if (!users_find_by_name(owner, &u)) {
            kputs("chown: invalid user: ", C_ERR); kputs(owner, C_TEXT); kputc('\n', C_TEXT);
            return 1;
        }
        int gid = u.gid;
        if (grp && *grp) {
            user_info_t g;   // группа задаётся именем пользователя с личной группой или числом
            if (users_find_by_name(grp, &g)) gid = g.gid;
            else {
                gid = 0;
                for (char* c = grp; *c >= '0' && *c <= '9'; c++) gid = gid * 10 + (*c - '0');
            }
        }
        char n[PERM_PATH_MAX];
        perm_normalize(path, tty_cwd(), n, sizeof(n));
        perm_info_t pi;
        perm_get(n, &pi);
        perm_set(n, "/", u.uid, gid, pi.mode);
        return 1;
    }

    if ((a = l_cmd(cmd, "stat"))) {
        char* p = a;
        char* path = l_word(&p);
        if (!path) path = tty_cwd();
        char n[PERM_PATH_MAX];
        perm_normalize(path, tty_cwd(), n, sizeof(n));
        perm_info_t pi;
        perm_get(n, &pi);
        char ms[11];
        perm_format_mode(pi.mode, 0, ms);
        user_info_t u;
        char gname[32];
        users_group_name(pi.gid, gname, sizeof(gname));
        kputs(ms + 1, C_OK);                       // тип файла по таблице прав неизвестен
        kputs("  ", C_TEXT);
        kputs(users_find_by_uid(pi.uid, &u) ? u.name : "?", C_TEXT);
        kputs(" ", C_TEXT);
        kputs(gname, C_TEXT);
        kputs("  ", C_TEXT);
        kputs(n, CON_CYAN);
        kputc('\n', C_TEXT);
        return 1;
    }

    return 0;
}
