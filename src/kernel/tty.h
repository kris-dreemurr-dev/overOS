#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>

#define MAX_TTYS 2
#define TTY_KBD_Q_SIZE 32

typedef struct {
    int id;
    int active;
    uint32_t* buffer;
    int cursor_x;
    int cursor_y;
    int prompt_min_x;
    int total_scrolled_px;
    int scroll_offset_px;
    char input_buf[256];
    int input_len;
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
} tty_t;

void   tty_init_core(void);
void   tty_switch(int target_id);
int    tty_get_active_id(void);
tty_t* tty_get(int id);
void   print_tty_banner(int tty_id);
void   tty1_task_entry(void);
int    tty_check_hotkey(uint8_t scancode);

// Очередь клавиатуры TTY: push вызывает только ps2_hw_service (единственный
// читатель порта), pop — оболочка/.sys/.prg, каждый свою (по своему tty_id).
void   tty_kbd_push(int tty_id, uint8_t scancode);
int    tty_kbd_pop(int tty_id, uint8_t* out);

#endif