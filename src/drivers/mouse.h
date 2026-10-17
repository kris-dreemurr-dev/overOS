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
extern int g_ps2_present;   // 0 — контроллера PS/2 нет (ввод только с USB)
void update_mouse_state(void);
void mouse_feed_byte(uint8_t byte);
// Движение в экранных осях (вправо +, вниз +), колесо (+ вниз), кнопки: PS/2 и USB
void mouse_feed_report(int dx, int dy, int wheel, uint8_t buttons);

int get_mouse_x(void);
int get_mouse_y(void);
int get_mouse_btn(void);
uint8_t get_mouse_buttons_raw(void);
void get_mouse_debug(mouse_debug_info_t* dbg);

int  get_mouse_wheel_delta(void);

// Чувствительность указателя: percent 25..400 (100 = как есть), accel 0/1
void mouse_set_speed(int percent, int accel);
int  mouse_get_speed(void);
int  mouse_get_accel(void);

#endif