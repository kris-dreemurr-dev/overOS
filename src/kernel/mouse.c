#include <stdint.h>

extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void put_pixel(int x, int y, uint32_t color);
extern uint16_t screen_width;
extern uint16_t screen_height;

static int mouse_x = 200;
static int mouse_y = 200;
static uint8_t mouse_buttons = 0;
static uint8_t mouse_bytes[3];
static int mouse_cycle = 0;

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

    // Включаем порт AUX (мышь)
    mouse_wait(1);
    outb(0x64, 0xA8);

    // Считываем Command Byte
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    status = inb(0x60);

    // Разрешаем порты мыши и клавиатуры, включаем трансляцию
    status |= 0x43;
    status &= ~0x30;

    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    // Сброс настроек по умолчанию (Set Defaults)
    mouse_write(0xF6);
    mouse_read();

    // Задаем частоту дискретизации (Sample Rate) 100 пакетов/сек
    mouse_write(0xF3);
    mouse_read();
    mouse_write(100);
    mouse_read();

    // Задаем разрешение сенсора (Resolution: 4 counts/mm)
    mouse_write(0xE8);
    mouse_read();
    mouse_write(3);
    mouse_read();

    // Разрешаем непрерывный поток данных (Enable Data Reporting)
    mouse_write(0xF4);
    mouse_read();

    // Очищаем оставшиеся байты подтверждения
    while (inb(0x64) & 1) {
        inb(0x60);
    }

    mouse_cycle = 0;
    mouse_buttons = 0;
}

void draw_cursor_shape(int x, int y) {
    static const char cursor_bitmap[12][9] = {
        "X.......",
        "XX......",
        "X.X.....",
        "X..X....",
        "X...X...",
        "X....X..",
        "X.....X.",
        "X......X",
        "X....***",
        "X..X....",
        "X.X.....",
        "XX......"
    };

    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 8; col++) {
            if (cursor_bitmap[row][col] == 'X') {
                put_pixel(x + col, y + row, 0xFFFFFF);
            } else if (cursor_bitmap[row][col] == '*') {
                put_pixel(x + col, y + row, 0x000000);
            }
        }
    }
}

// Разбор одного байта мыши (AUX). Вызывается и из update_mouse_state, и из чтения
// клавиатуры модулей (sys_kbd_poll), чтобы байты мыши не терялись, когда их
// вычитал не тот, кто их ждал.
void mouse_feed_byte(uint8_t byte) {
    if (mouse_cycle == 0) {
        // Первый байт пакета PS/2: бит 3 обязан быть 1, биты переполнения — 0
        if ((byte & 0x08) == 0 || (byte & 0xC0) != 0) {
            return;
        }
        mouse_bytes[0] = byte;
        mouse_cycle = 1;
    } else if (mouse_cycle == 1) {
        mouse_bytes[1] = byte;
        mouse_cycle = 2;
    } else if (mouse_cycle == 2) {
        mouse_bytes[2] = byte;
        mouse_cycle = 0;

        // Бит 0: Левая кнопка (ЛКМ), Бит 1: Правая (ПКМ), Бит 2: Средняя (СКМ)
        mouse_buttons = mouse_bytes[0] & 0x07;

        int dx = (int)mouse_bytes[1];
        int dy = (int)mouse_bytes[2];

        // Знаковое расширение 9-битных дельт смещения
        if (mouse_bytes[0] & 0x10) dx |= ~0xFF;
        if (mouse_bytes[0] & 0x20) dy |= ~0xFF;

        mouse_x += dx;
        mouse_y -= dy; // В PS/2 вертикальная ось инвертирована относительно экрана

        // Ограничение по видимой области экрана
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= (int)screen_width - 8) mouse_x = screen_width - 8;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= (int)screen_height - 12) mouse_y = screen_height - 12;
    }
}

void update_mouse_state(void) {
    while (1) {
        // Статус и данные читаем атомарно: таймер может вытеснить задачу между ними,
        // и другой читатель заберёт байт, который мы только что увидели в статусе
        uint64_t fl;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
        uint8_t status = inb(0x64);
        int take = (status & 0x01) && (status & 0x20);   // только байт мыши; клавиатуру не трогаем
        uint8_t byte = take ? inb(0x60) : 0;
        __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");

        if (!take) break;
        mouse_feed_byte(byte);
    }
}

int get_mouse_x(void) { return mouse_x; }
int get_mouse_y(void) { return mouse_y; }

// Возвращает 1, если зажата левая кнопка мыши (ЛКМ)
int get_mouse_btn(void) {
    return mouse_buttons & 0x01;
}

// Возвращает полное битовое состояние всех кнопок (0: ЛКМ, 1: ПКМ, 2: СКМ)
uint8_t get_mouse_buttons_raw(void) {
    return mouse_buttons;
}