#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

typedef struct {
    int x, y;
    int dx, dy;
    uint8_t buttons;
    uint32_t raw_bytes;
    uint32_t packets;
    uint8_t last_raw[3];
    uint8_t status_64;
} __attribute__((packed)) mouse_debug_info_t;

void init_ps2_mouse(void);
void update_mouse_state(void);
void mouse_feed_byte(uint8_t byte);

int get_mouse_x(void);
int get_mouse_y(void);
int get_mouse_btn(void);
uint8_t get_mouse_buttons_raw(void);
void get_mouse_debug(mouse_debug_info_t* dbg);

#endif