#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>

#define MAX_TTYS 6   // tty1..tty6, как в Linux (их показывает экран, Alt+F1..F6)
// Псевдотерминалы для графических эмуляторов (overTerm): такой же TTY, только
// его буфер никогда не попадает на экран -- его читает и рисует сама программа.
#define MAX_PTYS 4
#define TTY_SLOTS (MAX_TTYS + MAX_PTYS)
#define TTY_KBD_Q_SIZE 32

#define TTY_HIST_HEIGHT 64   // строк прокрутки назад (история экрана TTY)

// История набранных команд -- своя у каждого терминала, как у bash. Живёт в
// драйвере, а не в оболочке: строку собирает ядро (канонический режим), и
// стрелки вверх/вниз может обработать только оно.
#define TTY_HIST_CMDS 24
#define TTY_CMD_MAX   160

typedef struct {
    int id;
    int active;
    // Размер консоли в пикселях. У tty1..tty6 это весь экран, у псевдотерминала
    // -- размер окна программы: перенос строки и прокрутка считаются по ним.
    int w_px, h_px;
    int is_pty;          // 1 = псевдотерминал (на экран не выводится)
    uint64_t owner_pid;  // чей это псевдотерминал (закроем вместе с ним)
    uint32_t* buffer;
    uint32_t* history;   // кольцо прокрутки этого TTY (своё у каждого)
    int cursor_x;
    int cursor_y;
    int prompt_min_x;
    int total_scrolled_px;
    int scroll_offset_px;
    char input_buf[256];
    int input_len;

    // Кольцо последних команд: самая свежая -- (cmd_total - 1) % TTY_HIST_CMDS
    char cmd_hist[TTY_HIST_CMDS][TTY_CMD_MAX];
    int  cmd_total;              // сколько команд было всего (для нумерации)
    int  hist_on;                // копить ли историю (включает оболочка)
    char current_path[128];
    uint32_t bg_color;   
    int gfx_mode;        
    int fg_pid;   // PID процесса-владельца этого TTY (-1 = нет), для Ctrl+C

    // Очередь сканкодов этого TTY. Единственный читатель порта 0x60/0x64 на всю
    // систему — ps2_hw_service() (keyboard.c), вызывается безусловно из простоя
    // kernel_main. Он же раскладывает байты по очередям нужного TTY; оболочка
    // (keyboard_poll_handler), .sys (sys_kbd_poll) и .prg (sys_get_key) просто
    // читают из своей очереди, порт сами больше не трогают.
    uint8_t kbd_q[TTY_KBD_Q_SIZE];
    volatile int kbd_head;
    volatile int kbd_tail;

    // Сеанс пользователя (login.c): кто вошёл в этот TTY
    int  echo_off;               // ввод пароля: /dev/tty не показывает символы (как termios ECHO)
    int  login_stage;            // LOGIN_STAGE_* (0 = ждём имя пользователя)
    int  uid, gid;
    char login_name[32];
    char pending_name[32];       // su/passwd/useradd: над кем операция
    char pending_pass[64];       // первый ввод нового пароля (для подтверждения)
    char pending_old[64];        // passwd: текущий пароль
    int  su_depth;               // вложенные su: exit возвращает предыдущего пользователя
    int  su_uid[4], su_gid[4];
    char su_name[4][32];

    // Первоначальная настройка: выбор до создания пользователя
    int  oobe_lang, oobe_tz, oobe_layout2;
    int  pending_admin;          // useradd -G wheel: новый пользователь — администратор
    // sudo <команда>: ждёт пароля / исполняется от root, затем права возвращаются
    char sudo_cmd[256];
    int  sudo_ready;             // пароль принят, команду можно забрать
    int  sudo_once;              // сейчас идёт команда от root через sudo
    int  sudo_uid, sudo_gid;
    char sudo_name[32];
} tty_t;

// Консоль принадлежит TTY вызывающей задачи, а не системе: оболочка в фоновом
// TTY пишет в свой буфер, а не поверх того, что сейчас на экране.
tty_t* tty_console(void);
// Текущий каталог оболочки этого TTY (раньше один глобальный current_path на всю систему)
char*  tty_cwd(void);
// BSOD: направить весь вывод консоли в указанный TTY (чтобы экран смерти был виден)
void   tty_console_force(int tty_id);

void   tty_init_core(void);
void   tty_switch(int target_id);
int    tty_get_active_id(void);
tty_t* tty_get(int id);

// --- Псевдотерминалы для графического эмулятора ---------------------------
// Открыть: w_px x h_px пикселей, оболочка /sys/sh.prg с правами uid/gid.
int    pty_open(int w_px, int h_px, int uid, int gid, uint64_t owner_pid);
// Скопировать картинку консоли (w_px*h_px пикселей ARGB) в буфер программы
int    pty_snapshot(int id, uint32_t* dst, uint64_t max_bytes);
void   pty_close(int id);
int    pty_alive(int id);
void   print_tty_banner(int tty_id);
void   tty1_task_entry(void);
int    tty_check_hotkey(uint8_t scancode);

// Очередь клавиатуры TTY: push вызывает только ps2_hw_service (единственный
// читатель порта), pop — оболочка/.sys/.prg, каждый свою (по своему tty_id).
void   tty_kbd_push(int tty_id, uint8_t scancode);

// Строчный ввод (канонический режим tty-драйвера UNIX): эхо, Backspace, раскладка
// и Shift обрабатывает ядро, программа Ring 3 просто читает готовую строку.
int    tty_line_read(int tty_id, char* out, int max);

// История команд TTY: сколько доступно, и запись по номеру (0 -- самая старая
// из сохранённых). Для команды history в оболочке -- сисколл 129.
int         tty_hist_count(int tty_id);
const char* tty_hist_get(int tty_id, int idx);
int         tty_hist_number(int tty_id, int idx);   // номер команды, как в bash
void   tty_set_echo(int tty_id, int on);
void   tty_set_history(int tty_id, int on);

struct task;
int    tty_is_foreground(struct task* t);
int    tty_kbd_pop(int tty_id, uint8_t* out);

#endif