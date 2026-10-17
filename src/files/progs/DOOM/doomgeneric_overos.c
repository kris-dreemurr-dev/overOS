#include "doomgeneric/doomgeneric.h"
#include "ovos.h"   // общие обёртки сисколов (src/lib)
#include <stdio.h>

// Локальный кэш палитры и буфер 32-битных RGB пикселей в User-Space
static uint32_t local_palette[256];
static uint32_t rgb_frame_buffer[320 * 200];

// Внутренний 8-битный буфер отрисовки Doom (screens[0] = 320x200 индексов)
extern uint8_t* screens[5];
extern void* memset(void* dest, int c, size_t n);
extern void ov_print(const char* str);
extern char __bss_start[];
extern char _end[];

extern uint8_t* I_VideoBuffer;
static uint64_t tsc_per_ms = 2000000;
static uint64_t start_tsc = 0;

static uint32_t paused_ms = 0;   // сколько времени Doom простоял в фоне

void DG_Init(void) {
    tsc_per_ms = ov_tsc_per_ms();   // частоту TSC меряет ядро при загрузке
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
        paused_ms += ov_blit_frame(rgb_frame_buffer);   // пауза часов, пока TTY скрыт
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
    uint8_t sc = ov_get_key();
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
    ov_set_palette(local_palette);
}

void DG_SetWindowTitle(const char *title) {
    (void)title; // В devOS оконной рамки нет, выводим на весь экран
}