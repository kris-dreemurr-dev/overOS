#include "doomgeneric/doomgeneric.h"
#include <stdint.h>
#include <stdio.h>

// Таблица трансляции сканкодов PS/2 (Set 1) в коды клавиш движка Doom
static const unsigned char scancode_to_doom[128] = {
    [0x01] = 27,          // Escape (KEY_ESCAPE)
    [0x02] = '1',         // 1 (Кастет / Бензопила)
    [0x03] = '2',         // 2 (Пистолет)
    [0x04] = '3',         // 3 (Дробовик / Двустволка)
    [0x05] = '4',         // 4 (Пулемет)
    [0x06] = '5',         // 5 (Ракетница)
    [0x07] = '6',         // 6 (Плазмоган)
    [0x08] = '7',         // 7 (BFG9000)
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',         // Уменьшить экран / масштаб карты
    [0x0D] = '=',         // Увеличить экран / масштаб карты
    [0x0E] = 127,         // Backspace (KEY_BACKSPACE) — удаление символа в сейвах
    [0x0F] = 9,           // Tab (KEY_TAB) — Автокарта
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',         // Pause
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = 13,          // Enter (KEY_ENTER)
    [0x1D] = 0x80 + 0x1D, // Left Ctrl (KEY_RCTRL) — Огонь
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
    [0x2A] = 0x80 + 0x36, // Left Shift (KEY_RSHIFT) — Бег
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
    [0x36] = 0x80 + 0x36, // Right Shift
    [0x37] = '*',
    [0x38] = 0x80 + 0x38, // Left Alt (KEY_RALT) — Стрейф
    [0x39] = ' ',         // Space — Открыть дверь / переключатель
    
    // Функциональный ряд (F1 - F10)
    [0x3B] = 0x80 + 0x3B, // F1 (Помощь)
    [0x3C] = 0x80 + 0x3C, // F2 (Сохранить)
    [0x3D] = 0x80 + 0x3D, // F3 (Загрузить)
    [0x3E] = 0x80 + 0x3E, // F4 (Настройки)
    [0x3F] = 0x80 + 0x3F, // F5 (Детализация)
    [0x40] = 0x80 + 0x40, // F6 (Быстрое сохранение)
    [0x41] = 0x80 + 0x41, // F7 (Завершить игру)
    [0x42] = 0x80 + 0x42, // F8 (Сообщения)
    [0x43] = 0x80 + 0x43, // F9 (Быстрая загрузка)
    [0x44] = 0x80 + 0x44, // F10 (Выход)

    // Стрелки управления
    [0x48] = 0xAD,        // Стрелка Вверх (KEY_UPARROW)
    [0x4B] = 0xAC,        // Стрелка Влево (KEY_LEFTARROW)
    [0x4D] = 0xAE,        // Стрелка Вправо (KEY_RIGHTARROW)
    [0x50] = 0xAF,        // Стрелка Вниз (KEY_DOWNARROW)

    // Дополнительные функциональные
    [0x57] = 0x80 + 0x57, // F11 (Гамма-коррекция)
    [0x58] = 0x80 + 0x58  // F12
};

// Локальный кэш палитры и буфер 32-битных RGB пикселей в User-Space
static uint32_t local_palette[256];
static uint32_t rgb_frame_buffer[320 * 200];

// Системные вызовы devOS (int 0x80)
static inline void sys_set_palette(const uint32_t* pal) {
    __asm__ volatile ("int $0x80" : : "a"(20), "b"(pal) : "memory");
}

static inline uint32_t sys_blit_frame(const uint32_t* fb) {
    uint32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "0"(21), "b"(fb) : "memory");
    return r;   // мс, которые Doom простоял в фоне (его TTY не был на экране)
}

static inline uint32_t sys_tsc_per_ms(void) {
    uint32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "0"(22) : "memory");
    return r;
}

static inline void sys_sleep(uint32_t ms) {
    __asm__ volatile ("int $0x80" : : "a"(4), "b"(ms) : "memory");
}

static inline uint8_t sys_get_key(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(6) : "memory");
    return (uint8_t)ret;
}

// Внутренний 8-битный буфер отрисовки Doom (screens[0] = 320x200 индексов)
extern uint8_t* screens[5];
extern void* memset(void* dest, int c, size_t n);
extern void sys_print(const char* str);
extern char __bss_start[];
extern char _end[];

extern uint8_t* I_VideoBuffer;
static uint64_t tsc_per_ms = 2000000;
static uint64_t start_tsc = 0;

static uint32_t paused_ms = 0;   // сколько времени Doom простоял в фоне

void DG_Init(void) {
    tsc_per_ms = sys_tsc_per_ms();   // частоту TSC меряет ядро при загрузке
    if (tsc_per_ms == 0) tsc_per_ms = 2000000;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    start_tsc = ((uint64_t)hi << 32) | lo;
}

extern uint8_t* I_VideoBuffer;

void DG_DrawFrame() {
    if (I_VideoBuffer) {
        // Конвертируем индексированный буфер Doom в полноценный 32-битный RGB
        for (int i = 0; i < 320 * 200; i++) {
            rgb_frame_buffer[i] = local_palette[I_VideoBuffer[i]];
        }
        // Передаем готовый массив пикселей в безопасный сисколл ядру
        paused_ms += sys_blit_frame(rgb_frame_buffer);   // пауза часов, пока TTY скрыт
    }
}

void DG_SleepMs(uint32_t ms) {
    if (ms == 0) return;
    uint32_t start = DG_GetTicksMs();
    while ((DG_GetTicksMs() - start) < ms) {
        __asm__ volatile ("pause"); // Оптимизация spin-wait для процессора
    }
}

uint32_t DG_GetTicksMs(void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    uint64_t cur = ((uint64_t)high << 32) | low;

    // Переводим такты процессора в миллисекунды по калиброванной частоте
    if (tsc_per_ms == 0) tsc_per_ms = 2000000;
    return (uint32_t)((cur - start_tsc) / tsc_per_ms) - paused_ms;
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    uint8_t sc = sys_get_key();
    if (sc == 0) return 0;

    // Игнорируем префикс E0, но можно тоже отладочно вывести
    if (sc == 0xE0) return 0;

    *pressed = !(sc & 0x80);
    uint8_t make = sc & 0x7F;

    // Возвращаем единицу, чтобы игра видела событие
    *doomKey = make; 
    return 1;
}

// Перехват смены палитры Doom
void devos_set_palette(const uint8_t* raw_rgb) {
    for (int i = 0; i < 256; i++) {
        uint32_t r = raw_rgb[i * 3 + 0];
        uint32_t g = raw_rgb[i * 3 + 1];
        uint32_t b = raw_rgb[i * 3 + 2];
        local_palette[i] = (r << 16) | (g << 8) | b;
    }
    // Отправляем палитру ядру на всякий случай
    sys_set_palette(local_palette);
}

void DG_SetWindowTitle(const char *title) {
    (void)title; // В devOS оконной рамки нет, выводим на весь экран
}