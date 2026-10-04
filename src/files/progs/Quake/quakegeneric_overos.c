#include "quakegeneric/quakegeneric.h"
#include <stdint.h>
#include <stddef.h>

// --- Системные вызовы overOS из doomgeneric_overos.c ---
static inline void sys_set_palette(const uint32_t* pal) {
    __asm__ volatile ("int $0x80" : : "a"(20), "b"(pal) : "memory");
}

static inline uint32_t sys_blit_frame(const uint32_t* fb) {
    uint32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "0"(21), "b"(fb) : "memory");
    return r;
}

static inline uint8_t sys_get_key(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(6) : "memory");
    return (uint8_t)ret;
}

static inline uint32_t sys_tsc_per_ms(void) {
    uint32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "0"(22) : "memory");
    return r;
}

static inline void sys_get_mouse(int* dx, int* dy, uint8_t* buttons) {
    __asm__ volatile (
        "int $0x80"
        :
        : "a"(23), "b"(dx), "c"(dy), "d"(buttons)
        : "memory"
    );
}
// --------------------------------------------------------

static const unsigned char scancode_to_quake[128] = {
    [0x01] = 27,   // Escape (K_ESCAPE)
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = 127,  // Backspace (K_BACKSPACE)
    [0x0F] = 9,    // Tab (K_TAB)
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = 13,   // Enter (K_ENTER)
    [0x1D] = 133,  // Left Ctrl (K_CTRL)
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',
    [0x2A] = 134,  // Left Shift (K_SHIFT)
    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x36] = 134,  // Right Shift (K_SHIFT)
    [0x37] = '*',
    [0x38] = 132,  // Left Alt (K_ALT)
    [0x39] = ' ',  // Пробел (K_SPACE)

    // Функциональные F1 - F10
    [0x3B] = 135,  // F1
    [0x3C] = 136,  // F2
    [0x3D] = 137,  // F3
    [0x3E] = 138,  // F4
    [0x3F] = 139,  // F5
    [0x40] = 140,  // F6
    [0x41] = 141,  // F7
    [0x42] = 142,  // F8
    [0x43] = 143,  // F9
    [0x44] = 144,  // F10

    // Клавиши навигации и стрелки
    [0x47] = 151,  // Home
    [0x48] = 128,  // Стрелка Вверх (K_UPARROW)
    [0x49] = 150,  // Page Up
    [0x4B] = 130,  // Стрелка Влево (K_LEFTARROW)
    [0x4D] = 131,  // Стрелка Вправо (K_RIGHTARROW)
    [0x4F] = 152,  // End
    [0x50] = 129,  // Стрелка Вниз (K_DOWNARROW)
    [0x51] = 149,  // Page Down
    [0x52] = 147,  // Insert
    [0x53] = 148,  // Delete

    [0x57] = 145,  // F11
    [0x58] = 146   // F12
};

// Буферы для конвертации графики
static uint32_t local_palette[256];
static uint32_t rgb_frame_buffer[320 * 200];

void QG_Init(void) {
    // Здесь можно настроить TSC таймер, как это сделано в DG_Init
}

void QG_SetPalette(unsigned char palette[768]) {
    // Quake дает массив 768 байт (RGB для 256 цветов).
    for (int i = 0; i < 256; i++) {
        uint32_t r = palette[i * 3 + 0];
        uint32_t g = palette[i * 3 + 1];
        uint32_t b = palette[i * 3 + 2];
        local_palette[i] = (r << 16) | (g << 8) | b;
    }
    // Отправляем палитру ядру
    sys_set_palette(local_palette);
}

void QG_DrawFrame(void *pixels) {
    uint8_t *p8 = (uint8_t *)pixels;
    
    // Конвертируем 8-битный буфер в 32-битный, как в DG_DrawFrame
    for (int i = 0; i < 320 * 200; i++) {
        rgb_frame_buffer[i] = local_palette[p8[i]];
    }
    
    // Отрисовываем через сисколл
    sys_blit_frame(rgb_frame_buffer);
}

int QG_GetKey(int *down, int *key) {
    uint8_t sc;

    // Считываем сканкод, отсекая префикс 0xE0 для стрелок клавиатуры
    do {
        sc = sys_get_key();
        if (sc == 0) return 0;
    } while (sc == 0xE0);

    *down = !(sc & 0x80); // 1 = нажата, 0 = отпущена
    uint8_t make = sc & 0x7F;

    if (make >= sizeof(scancode_to_quake)) return 0;

    uint8_t qkey = scancode_to_quake[make];
    if (qkey == 0) return 0; // Неизвестная клавиша

    *key = (int)qkey;
    return 1;
}

void QG_GetMouseMove(int *x, int *y) {
    int dx = 0, dy = 0;
    uint8_t buttons = 0;
    
    sys_get_mouse(&dx, &dy, &buttons);

    // В Quake чувствительность можно масштабировать
    *x = dx * 2;
    *y = -dy * 2; // Инвертируем Y, если при движении мыши вверх прицел уходит вниз
}

void QG_GetJoyAxes(float *axes) {
    *axes = 0.0f;
}

void QG_Quit(void) {
    // Сисколл выхода из программы
}