#include "mouse.h"
#include <stdint.h>

extern uint16_t screen_width;
extern uint16_t screen_height;
extern void ps2_hw_service(void);

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Единые координаты и состояние
static int g_mouse_x = 512;
static int g_mouse_y = 384;
static int g_last_dx = 0;
static int g_last_dy = 0;
static uint8_t g_mouse_btn = 0;

static uint8_t g_packet[3];
static uint8_t g_cycle = 0;

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) { if ((inb(0x64) & 1) == 1) return; }
    } else {
        while (timeout--) { if ((inb(0x64) & 2) == 0) return; }
    }
}

static void mouse_write(uint8_t val) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, val);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

void init_ps2_mouse(void) {
    uint8_t status;

    // Включаем порт мыши
    mouse_wait(1);
    outb(0x64, 0xA8);

    // Читаем Command Byte
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    status = inb(0x60);

    // Разрешаем мышь, отключаем IRQ12 во избежание зависания PIC без EOI
    status |= 0x41;
    status &= ~(0x02 | 0x20);

    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    // Сброс и дефолты (0xF6)
    mouse_write(0xF6);
    mouse_read();

    // Разрешаем поток данных (0xF4)
    mouse_write(0xF4);
    mouse_read();

    while (inb(0x64) & 1) inb(0x60);

    if (screen_width)  g_mouse_x = screen_width / 2;
    if (screen_height) g_mouse_y = screen_height / 2;
    g_cycle = 0;
}

// Приём байта из ps2_hw_service()
void mouse_feed_byte(uint8_t byte) {
    if (g_cycle == 0) {
        // Бит 3 первого байта PS/2 всегда равен 1
        if ((byte & 0x08) == 0) return;
        g_packet[0] = byte;
        g_cycle = 1;
    } else if (g_cycle == 1) {
        g_packet[1] = byte;
        g_cycle = 2;
    } else if (g_cycle == 2) {
        g_packet[2] = byte;
        g_cycle = 0;

        g_mouse_btn = g_packet[0] & 0x07;

        // Прямое знаковое приведение байт 1 и 2 (аппаратный int8_t)
        int dx = (int8_t)g_packet[1];
        int dy = (int8_t)g_packet[2];

        dy = -dy; // Инвертируем Y для экранных координат

        g_last_dx = dx;
        g_last_dy = dy;

        g_mouse_x += dx;
        g_mouse_y += dy;

        // Ограничение по экрану
        int max_w = screen_width ? (int)screen_width - 2 : 1022;
        int max_h = screen_height ? (int)screen_height - 2 : 766;

        if (g_mouse_x < 0) g_mouse_x = 0;
        if (g_mouse_x > max_w) g_mouse_x = max_w;
        if (g_mouse_y < 0) g_mouse_y = 0;
        if (g_mouse_y > max_h) g_mouse_y = max_h;
    }
}

void update_mouse_state(void) {
    ps2_hw_service();
}

int get_mouse_x(void)   { return g_mouse_x; }
int get_mouse_y(void)   { return g_mouse_y; }
int get_mouse_btn(void) { return g_mouse_btn & 0x01; }
uint8_t get_mouse_buttons_raw(void) { return g_mouse_btn; }

int get_mouse_last_dx(void) { return g_last_dx; }
int get_mouse_last_dy(void) { return g_last_dy; }