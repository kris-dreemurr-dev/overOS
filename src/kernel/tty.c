#include "tty.h"
#include "contheme.h"
#include "sched.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../fs/fs.h"
#include "users.h"
#include "prog_loader.h"

extern void sleep_ms(uint32_t ms);

extern uint16_t  screen_width;
extern uint16_t  screen_height;
extern uint16_t  screen_pitch;
extern volatile  uint32_t* lfb;
extern uint32_t  back_buffer[];
extern void      keyboard_poll_handler(void);
extern uint32_t  kernel_screen_history[];   // кольцо прокрутки TTY1 (статическое, как и его буфер)
extern void      check_and_run_autostart(void);

extern void clear_screen(uint32_t color);
extern void print_prompt(void);
extern void flush_buffer(void);
extern void kputs(const char* str, uint32_t color);
extern void itoa(int n, char* str);
extern int  strcmp(const char* s1, const char* s2);

static tty_t g_ttys[TTY_SLOTS];
static int   g_active_tty = 0;

void tty_init_core(void) {
    // TTY 1 привязан к статическому back_buffer ядра
    g_ttys[0].id = 0;
    g_ttys[0].active = 1;
    g_ttys[0].w_px = screen_width;
    g_ttys[0].h_px = screen_height;
    g_ttys[0].buffer = back_buffer;
    g_ttys[0].current_path[0] = '/';
    g_ttys[0].current_path[1] = '\0';
    g_ttys[0].cursor_x = 0;
    g_ttys[0].cursor_y = 0;
    g_ttys[0].prompt_min_x = 0;
    g_ttys[0].total_scrolled_px = 0;
    g_ttys[0].scroll_offset_px = 0;
    g_ttys[0].input_len = 0;
    g_ttys[0].bg_color = CONT_BG;        // текстовая консоль — классический чёрный
    g_ttys[0].gfx_mode = 0;
    g_ttys[0].fg_pid = -1;
    g_ttys[0].kbd_head = 0;
    g_ttys[0].kbd_tail = 0;
    g_ttys[0].history = kernel_screen_history;

    // TTY 2..6 не выделены до первого Alt+F2..F6
    for (int i = 1; i < MAX_TTYS; i++) {
        g_ttys[i].id = i;
        g_ttys[i].active = 0;
        g_ttys[i].buffer = NULL;
    }

    g_active_tty = 0;
}

// Консоль задачи: куда она печатает. Потоки ядра и процессы привязаны к TTY
// (task_t.tty_id); у кого TTY нет (ранняя загрузка, прерывания) — активный.
static tty_t* g_console_force = 0;    // BSOD: писать строго в видимый TTY

// Паника/BSOD: вывод должен попасть на экран, даже если упала задача чужого TTY
void tty_console_force(int tty_id) {
    g_console_force = tty_get(tty_id);
}

tty_t* tty_console(void) {
    if (g_console_force) return g_console_force;
    task_t* me = sched_get_current_task();
    if (me && me->tty_id >= 0 && me->tty_id < TTY_SLOTS && g_ttys[me->tty_id].active)
        return &g_ttys[me->tty_id];
    tty_t* act = &g_ttys[g_active_tty];
    return act->buffer ? act : &g_ttys[0];   // запасной — TTY1 со статическим буфером
}

char* tty_cwd(void) {
    tty_t* t = tty_console();
    return t ? t->current_path : g_ttys[0].current_path;
}

// Каталог, относительно которого ФС разрешает относительные имена для вызывающего:
// у процесса свой (унаследован при запуске), у оболочки TTY — её текущий путь.
const char* fs_caller_cwd(void) {
    task_t* t = sched_get_current_task();
    if (t && t->is_process && t->cwd[0]) return t->cwd;
    return tty_cwd();
}

// Смена каталога вызывающего (из fs_change_dir/fs_go_root)
void fs_caller_set_cwd(const char* abs_path) {
    if (!abs_path || !abs_path[0]) abs_path = "/";
    task_t* t = sched_get_current_task();
    char* dst = (t && t->is_process && t->cwd[0]) ? t->cwd : tty_cwd();
    int max = (t && t->is_process && t->cwd[0]) ? (int)sizeof(t->cwd) : 128;
    int i = 0;
    while (abs_path[i] && i < max - 1) { dst[i] = abs_path[i]; i++; }
    dst[i] = '\0';
}

int tty_get_active_id(void) {
    return g_active_tty;
}

tty_t* tty_get(int id) {
    if (id < 0 || id >= TTY_SLOTS) return NULL;
    return &g_ttys[id];
}

static void tty_shell_loop(void);

// Создаёт TTY id (2..6) при первом обращении: видеобуфер, состояние терминала и задачу с оболочкой.
static int tty_spawn(int id) {
    size_t px_count = (size_t)screen_width * screen_height;
    size_t fb_size  = px_count * sizeof(uint32_t);
    size_t pages    = (fb_size + 4095) / 4096;

    // Выделяем непрерывный видеобуфер под TTY
    void* phys = pmm_alloc_pages(pages);
    if (!phys) return -1;
    tty_t* t = &g_ttys[id];
    t->w_px = screen_width;
    t->h_px = screen_height;
    t->buffer = (uint32_t*)PHYS_TO_VIRT(phys);

    // Своё кольцо прокрутки: раньше история была одна на все TTY и мешалась.
    // Шаг строки — screen_width, ровно как индексирует scroll_screen в kernel.c.
    size_t hist_pages = ((size_t)TTY_HIST_HEIGHT * screen_width * sizeof(uint32_t) + 4095) / 4096;
    void* hist_phys = pmm_alloc_pages(hist_pages);
    if (!hist_phys) { pmm_free_page(phys); return -1; }
    t->history = (uint32_t*)PHYS_TO_VIRT(hist_phys);

    // Зачищаем буфер терминала
    for (size_t i = 0; i < px_count; i++) {
        t->buffer[i] = CONT_BG;
    }

    t->id = id;
    t->cursor_x = 0;
    t->cursor_y = 0;
    t->current_path[0] = '/';
    t->current_path[1] = '\0';
    t->prompt_min_x = 0;
    t->total_scrolled_px = 0;
    t->scroll_offset_px = 0;
    t->input_len = 0;
    t->bg_color = CONT_BG;               // tty2..tty6 — та же классика
    t->gfx_mode = 0;
    t->fg_pid = -1;
    t->kbd_head = 0;
    t->kbd_tail = 0;
    t->active = 1;

    // Оболочка TTY: общий цикл, привязка к TTY через tty_id
    static const char* names[MAX_TTYS] = { "tty1_shell", "tty2_shell", "tty3_shell",
                                           "tty4_shell", "tty5_shell", "tty6_shell" };
    task_t* task = task_create_kernel(tty_shell_loop, names[id]);
    task->tty_id = id;

    return 0;
}

void tty_switch(int target_id) {
    if (target_id == g_active_tty || target_id < 0 || target_id >= MAX_TTYS) return;

    // 1. Создаём TTY при первом обращении (Alt+F2..F6).
    int is_first_open = 0;
    if (target_id > 0 && !g_ttys[target_id].active) {
        if (tty_spawn(target_id) != 0) return;   
        is_first_open = 1;
    }

    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    // Состояние консоли (курсор, прокрутка, строка ввода, путь) лежит в самом
    // tty_t, копировать его в глобали и обратно больше не нужно.
    g_active_tty = target_id;

    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");

    (void)is_first_open;   // баннер печатает сама оболочка TTY при старте

    // Переключение крадёт отпускание модификаторов: Alt нажали в одном TTY, а
    // 0xB8 уходит уже в другой. Потребители в ядре сбрасываем сами, а клиентам
    // Ring 3 (композитор ведёт своё состояние Alt/Shift/Ctrl) досылаем коды
    // отпускания — иначе после Alt+F2 и возврата Alt+F1 рабочий стол считал Alt
    // зажатым и переставал отдавать окнам набранный текст.
    extern void kbd_clear_modifiers(void);
    kbd_clear_modifiers();
    tty_kbd_push(target_id, 0xAA);   // Shift левый
    tty_kbd_push(target_id, 0xB6);   // Shift правый
    tty_kbd_push(target_id, 0x9D);   // Ctrl
    tty_kbd_push(target_id, 0xB8);   // Alt

    // Показываем экран целевого TTY
    flush_buffer();
}

// Вход в систему и оболочка живут в Ring 3 (/sys/login.prg -> /sys/sh.prg).
// Поток ядра остаётся getty: печатает баннер TTY и перезапускает login, если тот
// умер. Если программы нет (старый образ, сломанная ФС) — работает встроенная
// оболочка ядра, чтобы систему можно было починить.
static int tty_start_login(tty_t* me) {
    int err = 0;
    task_t* p = prog_spawn("/sys/login.prg", "", 0, 0, 0, &err);
    if (!p) return 0;
    me->fg_pid = (int)p->pid;
    sched_start_process(p);                 // ввод TTY уже наш -- можно бежать
    int code = sched_wait_child(p);
    (void)code;
    me->fg_pid = -1;
    return 1;
}

static void tty_shell_loop(void) {
    // Баннер печатает сама оболочка TTY: так он попадает в свой TTY, а не в
    // консоль задачи, которая нажала Alt+F2
    tty_t* me = tty_console();
    if (me && me->id != 0) {
        print_tty_banner(me->id);
        flush_buffer();
    }

    // getty: login.prg работает, пока жив; упал — поднимаем снова
    int ring3 = 1;
    while (ring3 && me) {
        ring3 = tty_start_login(me);
        if (ring3) sleep_ms(200);
    }

    // Запасной путь: встроенная оболочка ядра
    print_prompt();
    flush_buffer();
    while (1) {
        keyboard_poll_handler();
        sched_yield();
    }
}

void tty1_task_entry(void) {
    clear_screen(0x000000);

    users_init();                 // /etc/passwd, /etc/shadow, /etc/group, /etc/perms
    check_and_run_autostart();    // desktop стартует от root и сам показывает окно входа

    print_tty_banner(0);
    flush_buffer();
    tty_shell_loop();             // getty: /sys/login.prg, запасной путь — оболочка ядра
}

void tty_kbd_push(int tty_id, uint8_t scancode) {
    tty_t* t = tty_get(tty_id);
    if (!t) return;
    int next = (t->kbd_head + 1) % TTY_KBD_Q_SIZE;
    if (next != t->kbd_tail) {         // очередь полна — байт теряется (редкий случай)
        t->kbd_q[t->kbd_head] = scancode;
        t->kbd_head = next;
    }
}

// ---- История команд -------------------------------------------------------
void tty_set_history(int tty_id, int on) {
    tty_t* t = tty_get(tty_id);
    if (t) t->hist_on = on ? 1 : 0;
}

int tty_hist_count(int tty_id) {
    tty_t* t = tty_get(tty_id);
    if (!t) return 0;
    return t->cmd_total < TTY_HIST_CMDS ? t->cmd_total : TTY_HIST_CMDS;
}

const char* tty_hist_get(int tty_id, int idx) {
    tty_t* t = tty_get(tty_id);
    int n = tty_hist_count(tty_id);
    if (!t || idx < 0 || idx >= n) return 0;
    int first = t->cmd_total - n;                 // номер самой старой сохранённой
    return t->cmd_hist[(first + idx) % TTY_HIST_CMDS];
}

int tty_hist_number(int tty_id, int idx) {
    tty_t* t = tty_get(tty_id);
    int n = tty_hist_count(tty_id);
    if (!t || idx < 0 || idx >= n) return 0;
    return t->cmd_total - n + idx + 1;
}

// Добавить команду. Пустые и повтор предыдущей не пишем -- как в bash с
// HISTCONTROL=ignoredups.
void tty_hist_add(int tty_id, const char* line) {
    tty_t* t = tty_get(tty_id);
    if (!t || !t->hist_on || !line || !line[0]) return;
    if (t->cmd_total > 0) {
        const char* prev = t->cmd_hist[(t->cmd_total - 1) % TTY_HIST_CMDS];
        int i = 0;
        while (prev[i] && prev[i] == line[i]) i++;
        if (!prev[i] && !line[i]) return;
    }
    char* dst = t->cmd_hist[t->cmd_total % TTY_HIST_CMDS];
    int i = 0;
    while (line[i] && i < TTY_CMD_MAX - 1) { dst[i] = line[i]; i++; }
    dst[i] = '\0';
    t->cmd_total++;
}

int tty_kbd_pop(int tty_id, uint8_t* out) {
    tty_t* t = tty_get(tty_id);
    if (!t || t->kbd_head == t->kbd_tail) return 0;
    *out = t->kbd_q[t->kbd_tail];
    t->kbd_tail = (t->kbd_tail + 1) % TTY_KBD_Q_SIZE;
    return 1;
}

static int g_alt_state = 0;

int tty_check_hotkey(uint8_t scancode) {
    if (scancode == 0x38) { // Нажат Alt
        g_alt_state = 1;
        return 0; 
    }
    if (scancode == 0xB8) { // Отпущен Alt
        int was_combo = (g_alt_state == 2);
        g_alt_state = 0;
        return was_combo ? 1 : 0;
    }

    // Alt+F1..F6 -> TTY 1..6 (скан-коды F1..F6 идут подряд: 0x3B..0x40)
    if (g_alt_state && scancode >= 0x3B && scancode < 0x3B + MAX_TTYS) {
        g_alt_state = 2;
        tty_switch(scancode - 0x3B);
        return 1;
    }
    return 0;
}
// Процесс на переднем плане своего TTY (его запустила оболочка: desktop, игра) или поток ядра.
// Только ему можно читать клавиатуру/мышь напрямую и открывать /dev/fb0: иначе любой фоновый
// процесс прошлого сеанса читал бы пароль, набранный в окне входа следующего пользователя.
int tty_is_foreground(task_t* t) {
    if (!t) return 0;
    if (!t->is_process) return 1;
    if (t->tty_id < 0 || t->tty_id >= TTY_SLOTS) return 0;
    tty_t* tt = &g_ttys[t->tty_id];
    // У псевдотерминала один-единственный клиент -- оболочка, которую он сам и
    // запустил. Пока её pid не записан (она успела стартовать первой), считаем
    // её передним планом: иначе первое же чтение /dev/tty получало отказ.
    if (tt->is_pty && tt->fg_pid < 0) return 1;
    return tt->fg_pid == (int)t->pid;
}

// ===========================================================================
// Псевдотерминалы: TTY, который не попадает на экран
//
// Графическому эмулятору (overTerm) не нужен свой разбор команд -- ему нужен
// обычный TTY системы. Такой TTY устроен ровно как tty1..tty6: та же консоль,
// та же очередь клавиатуры, та же оболочка /sys/sh.prg со всеми командами,
// включая fastfetch. Отличий два: размер консоли задаёт окно программы, и её
// картинка никуда не выводится -- программа забирает её pty_snapshot() и
// рисует у себя. Это и есть наш аналог /dev/pts.
// ===========================================================================

// Страницы консоли отдаёт тот, кто точно в неё больше не пишет: сам поток
// оболочки, уже после того как окно закрыло псевдотерминал.
static void pty_free_buffers(tty_t* t) {
    if (t->buffer) {
        size_t pages = ((size_t)t->w_px * t->h_px * sizeof(uint32_t) + 4095) / 4096;
        uint64_t phys = virt_to_phys((uint64_t)t->buffer);
        for (size_t i = 0; i < pages; i++) pmm_free_page((void*)(phys + i * 4096));
        t->buffer = NULL;
    }
    if (t->history) {
        size_t pages = ((size_t)TTY_HIST_HEIGHT * t->w_px * sizeof(uint32_t) + 4095) / 4096;
        uint64_t phys = virt_to_phys((uint64_t)t->history);
        for (size_t i = 0; i < pages; i++) pmm_free_page((void*)(phys + i * 4096));
        t->history = NULL;
    }
    t->is_pty = 0;
}

static void pty_shell_loop(void) {
    tty_t* me = tty_console();
    if (!me) return;

    // Входить заново не надо: окно открыл уже вошедший пользователь, поэтому
    // запускаем сразу оболочку с его правами -- как это делает xterm.
    while (me->active) {
        int err = 0;
        task_t* p = prog_spawn("/sys/sh.prg", "", me->uid, me->gid, 0, &err);
        if (!p) {
            kputs("\n[pty] не удалось запустить /sys/sh.prg\n", 0xFF5555);
            break;
        }
        me->fg_pid = (int)p->pid;
        sched_start_process(p);
        sched_wait_child(p);
        me->fg_pid = -1;
        if (!me->active) break;
        sleep_ms(100);
    }

    // Оболочка закрыта (exit) -- сообщаем окну и ждём, пока оно закроет псевдотерминал
    me->login_stage = -1;
    while (me->active) sleep_ms(100);
    pty_free_buffers(me);
}

int pty_open(int w_px, int h_px, int uid, int gid, uint64_t owner_pid) {
    if (w_px < 160) w_px = 160;
    if (h_px < 64)  h_px = 64;
    if (w_px > 4096) w_px = 4096;
    if (h_px > 4096) h_px = 4096;
    w_px &= ~7;                       // целое число знакомест 8x16
    h_px &= ~15;

    int id = -1;
    for (int i = MAX_TTYS; i < TTY_SLOTS; i++)
        if (!g_ttys[i].active) { id = i; break; }
    if (id < 0) return -1;

    tty_t* t = &g_ttys[id];
    size_t px   = (size_t)w_px * h_px;
    size_t pages = (px * sizeof(uint32_t) + 4095) / 4096;
    void* phys = pmm_alloc_pages(pages);
    if (!phys) return -1;

    size_t hist_pages = ((size_t)TTY_HIST_HEIGHT * w_px * sizeof(uint32_t) + 4095) / 4096;
    void* hist_phys = pmm_alloc_pages(hist_pages);
    if (!hist_phys) { pmm_free_page(phys); return -1; }

    for (size_t i = 0; i < sizeof(tty_t); i++) ((uint8_t*)t)[i] = 0;

    t->id = id;
    t->is_pty = 1;
    t->owner_pid = owner_pid;
    t->w_px = w_px;
    t->h_px = h_px;
    t->buffer  = (uint32_t*)PHYS_TO_VIRT(phys);
    t->history = (uint32_t*)PHYS_TO_VIRT(hist_phys);
    for (size_t i = 0; i < px; i++) t->buffer[i] = CON_BG;

    t->current_path[0] = '/';
    t->current_path[1] = '\0';
    t->bg_color = CON_BG;                // псевдотерминал — тема окна
    t->fg_pid = -1;
    t->uid = uid;
    t->gid = gid;
    t->login_stage = 0;
    t->active = 1;

    // Домашний каталог пользователя, как после обычного входа
    user_info_t ui;
    if (users_find_by_uid(uid, &ui) && ui.home[0]) {
        int k = 0;
        while (ui.home[k] && k < (int)sizeof(t->current_path) - 1) { t->current_path[k] = ui.home[k]; k++; }
        t->current_path[k] = '\0';
        int n = 0;
        while (ui.name[n] && n < (int)sizeof(t->login_name) - 1) { t->login_name[n] = ui.name[n]; n++; }
        t->login_name[n] = '\0';
    }

    // Поток создаём с запрещёнными прерываниями: иначе планировщик может
    // выбрать его раньше, чем мы успеем записать tty_id, и первые строки
    // оболочки уйдут в активный TTY -- прямо на экран.
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    task_t* task = task_create_kernel(pty_shell_loop, "pty_shell");
    if (task) task->tty_id = id;
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    if (!task) { t->active = 0; return -1; }

    return id;
}

int pty_alive(int id) {
    tty_t* t = tty_get(id);
    return (t && t->is_pty && t->active && t->login_stage >= 0) ? 1 : 0;
}

int pty_snapshot(int id, uint32_t* dst, uint64_t max_bytes) {
    tty_t* t = tty_get(id);
    if (!t || !t->is_pty || !t->active || !t->buffer || !dst) return -1;
    uint64_t need = (uint64_t)t->w_px * t->h_px * sizeof(uint32_t);
    if (need > max_bytes) return -1;
    const uint32_t* src = t->buffer;
    uint64_t n = (uint64_t)t->w_px * t->h_px;
    for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
    return (int)need;
}

void pty_close(int id) {
    tty_t* t = tty_get(id);
    if (!t || !t->is_pty || !t->active) return;
    t->active = 0;
    // Оболочку убиваем, страницы освободит её же поток ядра (pty_free_buffers):
    // он единственный, кто ещё может писать в эту консоль.
    if (t->fg_pid > 0) sched_send_signal((uint64_t)t->fg_pid, 9);
}
