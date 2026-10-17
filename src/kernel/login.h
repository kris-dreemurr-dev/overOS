#ifndef LOGIN_H
#define LOGIN_H

// Вход в систему на TTY (как agetty + login) и команды сеанса оболочки ядра

#define LOGIN_STAGE_USER        0   // "overos login:"
#define LOGIN_STAGE_PASS        1   // "Password:"
#define LOGIN_STAGE_SHELL       2   // вошли, работает оболочка
#define LOGIN_STAGE_SU_PASS     3
#define LOGIN_STAGE_PASSWD_OLD  4
#define LOGIN_STAGE_PASSWD_NEW  5
#define LOGIN_STAGE_PASSWD_NEW2 6
#define LOGIN_STAGE_ADD_PASS    7
#define LOGIN_STAGE_ADD_PASS2   8
// Первоначальная настройка (OOBE) — вместо "login:", пока нет ни одного пользователя
#define LOGIN_STAGE_OOBE_NAME   9
#define LOGIN_STAGE_OOBE_PASS   10
#define LOGIN_STAGE_OOBE_PASS2  11
#define LOGIN_STAGE_OOBE_LANG   12   // язык -> часовой пояс -> раскладка -> пользователь
#define LOGIN_STAGE_OOBE_TZ     13
#define LOGIN_STAGE_OOBE_LAYOUT 14
#define LOGIN_STAGE_SUDO_PASS   15   // "[sudo] password for user:"

// Строку ввода целиком забирает login (вход или ввод пароля), а не оболочка
int  login_wants_line(void);
void login_handle_line(char* line);

// Ввод пароля: символы не отображаются
int  login_echo_off(void);

// Приглашение: "login:", "Password:" или "user@overos:/path$ "
void login_print_prompt(void);

// Команды сеанса: whoami, id, su, exit, logout, passwd, useradd, chmod, chown, stat.
// 1 = команда обработана.
int  login_builtin(char* cmd);

// Разрешено ли текущему пользователю оболочки действие с путём (печатает отказ сам)
int  login_may(const char* path, int want);
int  login_may_parent(const char* path, int want);

// sudo <команда>: после ввода пароля login отдаёт команду оболочке на исполнение от root.
// 1 — команда есть (скопирована в out); после неё оболочка вызывает login_after_command.
int  login_take_sudo_cmd(char* out, int max);
// Конец команды оболочки: вернуть права пользователя после sudo <команда>
void login_after_command(void);

// cd по абсолютному пути (из нормализованного perm_normalize). 0 — нет такого каталога.
int  login_chdir_abs(const char* path);

#endif
