#include <stdint.h>
#include "tty.h"
#include "contheme.h"
#include "prog_loader.h"
#include "sched.h"
#include "login.h"
#include "users.h"

extern void kputc(char c, uint32_t color);
extern void flush_buffer(void);
extern void execute_command(void);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void put_pixel(int x, int y, uint32_t color);
extern void kputs_at(int x, int y, const char* str, uint32_t color);
extern void draw_cursor(uint32_t color);
extern void print_prompt(void);
extern void mouse_feed_byte(uint8_t byte);
extern void usb_hid_drain(void);           // drivers/usb/hid.c
extern int  sched_send_signal(uint64_t pid, int sig);
extern uint32_t timer_ticks;
extern volatile uint32_t g_fb_last_write_tick;   // последний кадр игры/desktop (blit или /dev/fb0)
extern volatile int      g_fb_write_tty;

extern uint16_t screen_width;

static const char scancode_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0, '*',   0, ' '
};

// Shift: верхний ряд символов US-раскладки
static const char scancode_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
     0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',   0,
   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0, '*',   0, ' '
};

static int g_shift_down = 0;   // Shift зажат (левый 0x2A или правый 0x36)
static int g_caps_on    = 0;   // Caps Lock (фиксируется нажатием 0x3A)

// Светодиод Caps Lock на клавиатуре: команда 0xED + маска (бит 2 — Caps).
// Контроллер отвечает байтом 0xFA (ACK); он придёт в ps2_hw_service как
// обычный скан-код с установленным битом 7 и там же будет отброшен.
static void kbd_set_leds(void) {
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) { }
    outb(0x60, 0xED);
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) { }
    outb(0x60, (uint8_t)(g_caps_on ? 0x04 : 0x00));
}

// Caps Lock меняет регистр только у букв: на цифрах и знаках он не действует,
// поэтому подменять им Shift целиком нельзя (иначе с Caps «1» давала бы «!»)
static int caps_shift(int shift, int is_letter) {
    return (g_caps_on && is_letter) ? !shift : shift;
}

void handle_backspace(void) {
    tty_t* con = tty_console();
    if (!con) return;
    // Ввод пароля без эха: на экране стирать нечего
    if (login_echo_off()) {
        if (con->input_len > 0) con->input_buf[--con->input_len] = '\0';
        return;
    }
    if (con->input_len > 0 && con->cursor_x > con->prompt_min_x) {
        draw_cursor(0x000000);
        con->cursor_x -= 8;
        for (int r = 0; r < 16; r++) {
            for (int c = 0; c < 8; c++) {
                put_pixel(con->cursor_x + c, con->cursor_y + r, 0x000000);
            }
        }
        con->input_len--;
        con->input_buf[con->input_len] = '\0';
        draw_cursor(0xFFFFFF);
    }
}



static uint8_t g_ps2_ctrl_pressed = 0;   // Ctrl — состояние на всю систему, как и Alt в tty.c
static uint8_t g_ps2_alt_pressed  = 0;   // Alt: нужен только для аварийного Ctrl+Alt+C

// Сброс модификаторов при переключении TTY (tty_switch): нажатие Alt достаётся
// одному TTY, а отпускание уже другому, и без сброса Alt остаётся «зажатым»
void kbd_clear_modifiers(void) {
    g_shift_down = 0;
    g_ps2_ctrl_pressed = 0;
    g_ps2_alt_pressed = 0;
}

// Один скан-код набора 1, откуда бы он ни пришёл: порт PS/2 или USB-клавиатура
// (drivers/usb/hid.c переводит её отчёты в те же коды). Горячие клавиши TTY,
// Caps Lock, Ctrl+C и очередь активного TTY -- для всех клавиатур одни.
void kbd_scancode(uint8_t sc) {
    if (tty_check_hotkey(sc)) return;   // Alt+F1/F2 — поглощается здесь, глобально

    // Caps Lock: состояние всей системы (как Shift выше), светодиод — сразу.
    // Скан-код всё равно уходит в очередь TTY: композитор ведёт своё
    // состояние Caps по нему же (индикатор в трее).
    if (sc == 0x3A) { g_caps_on = !g_caps_on; kbd_set_leds(); }
    if (sc == 0xFA || sc == 0xFE) return;   // ответы контроллера на 0xED — не клавиши

    // Ctrl запоминаем, но НЕ поглощаем: играм (Doom/Quake: Ctrl = огонь) он нужен в очереди TTY.
    // Оболочка его игнорирует (в scancode_ascii[0x1D] = 0).
    if (sc == 0x1D) g_ps2_ctrl_pressed = 1;                  // Ctrl нажат
    if (sc == 0x9D) g_ps2_ctrl_pressed = 0;                  // Ctrl отпущен

    // Ctrl+C -> SIGINT процессу-владельцу АКТИВНОГО TTY (tty->fg_pid).
    // SIGINT не перехватывается (обработчиков сигналов пока нет), поэтому
    // сейчас это всегда немедленное завершение — "минимум: kill, Ctrl+C" из плана.
    // Исключение: если программа сейчас выводит кадры (игра, desktop), Ctrl+C ей НЕ убийца —
    // в Quake Ctrl=огонь, C=вниз, и случайное сочетание не должно закрывать игру.
    // Зависшая программа перестаёт выводить кадры, и через секунду Ctrl+C снова работает.
    if (sc == 0x38) g_ps2_alt_pressed = 1;                   // Alt нажат (для Ctrl+Alt+C)
    if (sc == 0xB8) g_ps2_alt_pressed = 0;

    if (g_ps2_ctrl_pressed && sc == 0x2E) {                  // 'C' (make-код)
        int act = tty_get_active_id();
        tty_t* at = tty_get(act);
        int fg = at ? at->fg_pid : -1;
        int game_running = (g_fb_write_tty == act) && ((timer_ticks - g_fb_last_write_tick) < 1000);

        // Ctrl+Alt+C — аварийное завершение, работает и для программ, рисующих кадры
        // (в них Ctrl+C отдан игре: Ctrl = огонь, C = вниз). Композитор не трогаем:
        // у него есть свой выход из сеанса, как у display manager.
        if (g_ps2_alt_pressed) {
            if (fg > 0 && fg != g_desktop_pid) sched_send_signal((uint64_t)fg, SIGINT);
            g_ps2_alt_pressed = 0;
            return;
        }
        if (!game_running) {
            if (fg > 0) sched_send_signal((uint64_t)fg, SIGINT);
            return;
        }
        // иначе — 'C' уходит игре обычной клавишей (ниже)
    }

    tty_kbd_push(tty_get_active_id(), sc);   // остальное — в очередь активного TTY
}

void ps2_hw_service(void) {
    extern int g_ps2_present;
    for (int n = 0; g_ps2_present && n < 256; n++) {
        // Статус + данные атомарно: таймер может вытеснить нас между двумя inb,
        // но раз это единственный читатель порта, этого и так почти не бывает —
        // атомарность тут скорее для аккуратности, чем от реальной гонки.
        uint64_t fl;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
        uint8_t status = inb(0x64);
        int have = status & 0x01;
        uint8_t sc = have ? inb(0x60) : 0;
        __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");

        if (!have || status == 0xFF) break;      // 0xFF — порта нет (контроллер пропал)

        if (status & 0x20) { mouse_feed_byte(sc); continue; }   // байт мыши
        kbd_scancode(sc);
    }
    usb_hid_drain();      // что накопили USB-клавиатуры и мыши (в том же месте и потоке)
}

void keyboard_poll_handler(void) {
    task_t* me = sched_get_current_task();
    if (!me || me->tty_id < 0 || me->tty_id != tty_get_active_id()) return;
    int my_tty = me->tty_id;
    tty_t* con = tty_get(my_tty);
    if (!con) return;
    int extended_key = 0;
    uint8_t alt_pressed = 0;

    uint8_t scancode;
    while (tty_kbd_pop(my_tty, &scancode)) {
        if (scancode == 0x2A || scancode == 0x36) { g_shift_down = 1; continue; }
        if (scancode == 0xAA || scancode == 0xB6) { g_shift_down = 0; continue; }

        if (scancode == 0xE0) { 
            extended_key = 1; 
            continue; 
        }

        // Фиксация нажатия Alt (0x38)
        if (scancode == 0x38) {
            alt_pressed = 1;
            extended_key = 0;
            continue;
        }

        // Отпускание Alt (0xB8)
        if (scancode == 0xB8) {
            alt_pressed = 0;
            extended_key = 0;
            continue;
        }

        // Перехват комбинаций Alt + F1 / Alt + F2
        if (alt_pressed) {
            if (scancode >= 0x3B && scancode < 0x3B + MAX_TTYS) { // F1..F6 -> TTY 1..6
                alt_pressed = 2;
                tty_switch(scancode - 0x3B);
                continue;
            }
        }

        // 1. Расширенные клавиши (Стрелки)
        if (extended_key) {
            extended_key = 0;
            if (scancode & 0x80) continue;

            if (scancode == 0x48) { // Вверх
                if (con->scroll_offset_px < con->total_scrolled_px) {
                    con->scroll_offset_px += 16;
                    if (con->scroll_offset_px > con->total_scrolled_px) con->scroll_offset_px = con->total_scrolled_px;
                    flush_buffer();
                }
                continue;
            } else if (scancode == 0x50) { // Вниз
                if (con->scroll_offset_px > 0) {
                    con->scroll_offset_px -= 16;
                    if (con->scroll_offset_px < 0) con->scroll_offset_px = 0;
                    flush_buffer();
                }
                continue;
            } else if (scancode == 0x4B) { // Влево
                if (con->cursor_x > con->prompt_min_x) {
                    draw_cursor(0x000000);
                    con->cursor_x -= 8;
                    draw_cursor(0xFFFFFF);
                    flush_buffer();
                }
                continue;
            } else if (scancode == 0x4D) { // Вправо
                if (con->cursor_x + 8 <= con->prompt_min_x + (con->input_len * 8)) {
                    draw_cursor(0x000000);
                    con->cursor_x += 8;
                    draw_cursor(0xFFFFFF);
                    flush_buffer();
                }
                continue;
            }
            continue;
        }

        // 2. Обычные символы и Enter/Backspace
        if (!(scancode & 0x80)) {
            if (scancode == 0x1C) { // ENTER
                con->scroll_offset_px = 0;
                draw_cursor(0x000000);
                con->input_buf[con->input_len] = '\0';
                execute_command();
            } else if (scancode == 0x0E) { // BACKSPACE
                con->scroll_offset_px = 0;
                handle_backspace();
                flush_buffer();
            } else {
                char ch;
                if (scancode >= 128) ch = 0;
                else {
                    char lo = scancode_ascii[scancode];
                    int sh = caps_shift(g_shift_down, lo >= 'a' && lo <= 'z');
                    ch = sh ? scancode_ascii_shift[scancode] : lo;
                }
                if (ch && con->input_len < (int)sizeof(con->input_buf) - 1) {
                    con->scroll_offset_px = 0;
                    con->input_buf[con->input_len++] = ch;
                    con->input_buf[con->input_len] = '\0';
                    if (!login_echo_off()) {   // пароль не отображается, как в login/su
                        kputc(ch, 0xFFFFFF);
                        flush_buffer();
                    }
                }
            }
        }
    }
}
// ============================================================================
// Строчный ввод /dev/tty (канонический режим, как у tty-драйвера UNIX)
//
// Раскладка, Shift, эхо и Backspace — работа ядра: оболочка Ring 3 (sh.prg)
// просто читает готовую строку, как это делает shell в UNIX. Перехват Alt+F1..F6
// и Ctrl+C уже сделан в ps2_hw_service, сюда они не попадают.
// ============================================================================
void tty_set_echo(int tty_id, int on) {
    tty_t* t = tty_get(tty_id);
    if (t) t->echo_off = !on;
}

int tty_line_read(int tty_id, char* out, int max) {
    tty_t* t = tty_get(tty_id);
    if (!t || !out || max <= 1) return 0;

    extern void tty_hist_add(int tty_id, const char* line);

    int len = 0;
    int extended = 0;
    int hist_n = tty_hist_count(tty_id);
    int hist_pos = hist_n;                   // hist_n == "новая, ещё не набранная строка"

    // Стереть с экрана то, что уже набрано, и показать другую строку.
    // Эхо тут посимвольное, поэтому возврат делаем теми же '\b'.
    #define LINE_REPLACE(src) do {                                          \
        if (!t->echo_off) { for (int _i = 0; _i < len; _i++) kputc('\b', 0xFFFFFF); } \
        len = 0;                                                            \
        for (const char* _p = (src); _p && *_p && len + 1 < max; _p++) {    \
            out[len++] = *_p;                                               \
            if (!t->echo_off) kputc(*_p, CON_WHITE);                        \
        }                                                                   \
        out[len] = '\0';                                                    \
        if (!t->echo_off) flush_buffer();                                   \
    } while (0)

    while (1) {
        uint8_t sc;
        while (!tty_kbd_pop(tty_id, &sc)) sched_yield();   // ждём свою клавишу

        if (sc == 0xE0) { extended = 1; continue; }
        if (extended) {
            extended = 0;
            // Стрелки вверх/вниз листают историю команд, как в любой оболочке.
            // В режиме ввода пароля история недоступна -- там её и не копим.
            if (t->echo_off) continue;
            if (sc == 0x48) {                            // вверх
                if (hist_pos > 0) {
                    hist_pos--;
                    LINE_REPLACE(tty_hist_get(tty_id, hist_pos));
                }
            } else if (sc == 0x50) {                     // вниз
                if (hist_pos < hist_n) {
                    hist_pos++;
                    LINE_REPLACE(hist_pos == hist_n ? "" : tty_hist_get(tty_id, hist_pos));
                }
            }
            continue;
        }

        if (sc == 0x2A || sc == 0x36) { g_shift_down = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { g_shift_down = 0; continue; }
        if (sc == 0x38 || sc == 0xB8) continue;             // Alt: комбинации ловит ps2_hw_service
        if (sc & 0x80) continue;                            // отпускание клавиши

        if (sc == 0x1C) {                                  // Enter
            if (!t->echo_off) { kputc('\n', CON_FG); flush_buffer(); }
            out[len] = '\0';
            if (!t->echo_off) tty_hist_add(tty_id, out);
            return len;
        }
        if (sc == 0x0E) {                                  // Backspace
            if (len > 0) {
                // У кириллицы символ занимает два байта UTF-8 — стираем целиком
                len--;
                while (len > 0 && ((uint8_t)out[len] & 0xC0) == 0x80) len--;
                if (!t->echo_off) { kputc('\b', CON_FG); flush_buffer(); }
            }
            continue;
        }

        char utf[4];
        int n = 0;
        {
            char lo = scancode_ascii[sc];
            int sh = caps_shift(g_shift_down, lo >= 'a' && lo <= 'z');
            char ch = sh ? scancode_ascii_shift[sc] : lo;
            if (ch && ch != '\n') { utf[0] = ch; n = 1; }
        }
        if (!n) continue;

        if (len + n < max) {
            for (int k = 0; k < n; k++) {
                out[len++] = utf[k];
                if (!t->echo_off) kputc(utf[k], CON_WHITE);
            }
            if (!t->echo_off) flush_buffer();
        }
    }
    #undef LINE_REPLACE
}

