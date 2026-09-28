#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>

#define MAX_TTYS 2

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
    uint32_t bg_color;   // фон консоли этого TTY (current_bg_color у каждого TTY свой)
    int gfx_mode;        // 1 = экраном владеет модуль (.sys): курсор консоли не рисуем
} tty_t;

void   tty_init_core(void);
void   tty_switch(int target_id);
int    tty_get_active_id(void);
tty_t* tty_get(int id);
void   print_tty_banner(int tty_id);
void   tty1_task_entry(void);
int    tty_check_hotkey(uint8_t scancode);

#endif