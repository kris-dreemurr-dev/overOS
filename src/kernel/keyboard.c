#include <stdint.h>
#include "tty.h"
#include "sched.h"

extern void kputc(char c, uint32_t color);
extern void flush_buffer(void);
extern void execute_command(void);
extern uint8_t inb(uint16_t port);
extern void put_pixel(int x, int y, uint32_t color);
extern void kputs_at(int x, int y, const char* str, uint32_t color);
extern void draw_cursor(uint32_t color);
extern void print_prompt(void);
extern void mouse_feed_byte(uint8_t byte);
extern int  sched_send_signal(uint64_t pid, int sig);

extern int cursor_x;
extern int cursor_y;
extern int total_scrolled_px;
extern int scroll_offset_px;
extern uint16_t screen_width;
extern int prompt_min_x;

#define MAX_INPUT 256
char input_buffer[MAX_INPUT];
int input_len = 0;
int kbd_layout = 0;

static const char scancode_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,
   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0, '*',   0, ' '
};

static const uint8_t scancode_ru[128] = {
    0,   27,  '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '-',  '=', '\b',
  '\t', 0xA9, 0xE6, 0xE3, 0xAA, 0xA5, 0xAD, 0xA3, 0xE8, 0xE9, 0xA7, 0xE5, 0xEA, '\n',
     0, 0xE4, 0xEB, 0xA2, 0xA0, 0xAF, 0xE0, 0xAE, 0xAB, 0xA4, 0xA6, 0xED, 0xA5,    0,
  '\\', 0xEF, 0xE7, 0xE1, 0xAC, 0xA8, 0xE2, 0xEC, 0xA1, 0xEE,  '.',    0,  '*',    0,  ' '
};

void handle_backspace(void) {
    if (input_len > 0 && cursor_x > prompt_min_x) {
        draw_cursor(0x000000);
        cursor_x -= 8;
        for (int r = 0; r < 16; r++) {
            for (int c = 0; c < 8; c++) {
                put_pixel(cursor_x + c, cursor_y + r, 0x000000);
            }
        }
        input_len--;
        input_buffer[input_len] = '\0';
        draw_cursor(0xFFFFFF);
    }
}

static void update_layout_indicator(void) {
    for (int dy = 0; dy < 16; dy++) {
        for (int dx = 0; dx < 48; dx++) put_pixel(screen_width - 60 + dx, 8 + dy, 0x000000);
    }
    kputs_at(screen_width - 60, 8, kbd_layout ? "[RU]" : "[EN]", kbd_layout ? 0x55FF55 : 0x55FFFF);
    flush_buffer();
}


static uint8_t g_ps2_ctrl_pressed = 0;   // Ctrl — состояние на всю систему, как и Alt в tty.c

void ps2_hw_service(void) {
    while (1) {
        // Статус + данные атомарно: таймер может вытеснить нас между двумя inb,
        // но раз это единственный читатель порта, этого и так почти не бывает —
        // атомарность тут скорее для аккуратности, чем от реальной гонки.
        uint64_t fl;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
        uint8_t status = inb(0x64);
        int have = status & 0x01;
        uint8_t sc = have ? inb(0x60) : 0;
        __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");

        if (!have) return;

        if (status & 0x20) { mouse_feed_byte(sc); continue; }   // байт мыши

        if (tty_check_hotkey(sc)) continue;   // Alt+F1/F2 — поглощается здесь, глобально

        if (sc == 0x1D) { g_ps2_ctrl_pressed = 1; continue; }    // Ctrl нажат
        if (sc == 0x9D) { g_ps2_ctrl_pressed = 0; continue; }    // Ctrl отпущен

        // Ctrl+C -> SIGINT процессу-владельцу АКТИВНОГО TTY (tty->fg_pid).
        // SIGINT не перехватывается (обработчиков сигналов пока нет), поэтому
        // сейчас это всегда немедленное завершение — "минимум: kill, Ctrl+C" из плана.
        if (g_ps2_ctrl_pressed && sc == 0x2E && !(sc & 0x80)) {   // 'C' (make-код)
            tty_t* at = tty_get(tty_get_active_id());
            if (at && at->fg_pid > 0) sched_send_signal((uint64_t)at->fg_pid, SIGINT);
            continue;
        }

        tty_kbd_push(tty_get_active_id(), sc);   // остальное — в очередь активного TTY
    }
}

void keyboard_poll_handler(void) {
    task_t* me = sched_get_current_task();
    if (!me || me->tty_id < 0 || me->tty_id != tty_get_active_id()) return;
    int my_tty = me->tty_id;
    int extended_key = 0;
    uint8_t alt_pressed = 0;

    uint8_t scancode;
    while (tty_kbd_pop(my_tty, &scancode)) {
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
            if (alt_pressed == 1) {
                kbd_layout = !kbd_layout;
                update_layout_indicator();
            }
            alt_pressed = 0;
            extended_key = 0;
            continue;
        }

        // Перехват комбинаций Alt + F1 / Alt + F2
        if (alt_pressed) {
            if (scancode == 0x3B) { // F1 -> TTY 1
                alt_pressed = 2; 
                tty_switch(0);
                continue;
            }
            if (scancode == 0x3C) { // F2 -> TTY 2
                alt_pressed = 2;
                tty_switch(1);
                continue;
            }
        }

        // 1. Расширенные клавиши (Стрелки)
        if (extended_key) {
            extended_key = 0;
            if (scancode & 0x80) continue;

            if (scancode == 0x48) { // Вверх
                if (scroll_offset_px < total_scrolled_px) {
                    scroll_offset_px += 16;
                    if (scroll_offset_px > total_scrolled_px) scroll_offset_px = total_scrolled_px;
                    flush_buffer();
                }
                continue;
            } else if (scancode == 0x50) { // Вниз
                if (scroll_offset_px > 0) {
                    scroll_offset_px -= 16;
                    if (scroll_offset_px < 0) scroll_offset_px = 0;
                    flush_buffer();
                }
                continue;
            } else if (scancode == 0x4B) { // Влево
                if (cursor_x > prompt_min_x) {
                    draw_cursor(0x000000);
                    cursor_x -= 8;
                    draw_cursor(0xFFFFFF);
                    flush_buffer();
                }
                continue;
            } else if (scancode == 0x4D) { // Вправо
                if (cursor_x + 8 <= prompt_min_x + (input_len * 8)) {
                    draw_cursor(0x000000);
                    cursor_x += 8;
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
                scroll_offset_px = 0;
                draw_cursor(0x000000);
                input_buffer[input_len] = '\0';
                execute_command();
            } else if (scancode == 0x0E) { // BACKSPACE
                scroll_offset_px = 0;
                handle_backspace();
                flush_buffer();
            } else {
                char ch = kbd_layout ? (char)scancode_ru[scancode] : scancode_ascii[scancode];
                if (ch && input_len < MAX_INPUT - 1) {
                    scroll_offset_px = 0;
                    input_buffer[input_len++] = ch;
                    input_buffer[input_len] = '\0';
                    kputc(ch, 0xFFFFFF);
                    flush_buffer();
                }
            }
        }
    }
}