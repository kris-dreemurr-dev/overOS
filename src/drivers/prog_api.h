#ifndef PROG_API_H
#define PROG_API_H

#include <stdint.h>

// Единая точка перехода через аппаратный шлюз INT 0x80
static inline uint32_t syscall(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "0"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

// -----------------------------------------------------------------------------
// ВЫСОКОУРОВНЕВЫЙ СИ-ИНТЕРФЕЙС ДЛЯ ПРИЛОЖЕНИЙ
// -----------------------------------------------------------------------------

static inline void exit(int code) {
    syscall(0, (uint32_t)code, 0, 0);
    while (1);
}

static inline void print(const char* str) {
    syscall(1, (uint32_t)(uintptr_t)str, 0, 0);
}

static inline void print_color(const char* str, uint32_t color) {
    syscall(1, (uint32_t)(uintptr_t)str, color, 0);
}

static inline void print_num(int val) {
    syscall(7, (uint32_t)val, 0, 0);
}

static inline void put_pixel(int x, int y, uint32_t color) {
    syscall(2, (uint32_t)x, (uint32_t)y, color);
}

static inline void flush(void) {
    syscall(3, 0, 0, 0);
}

static inline void sleep(uint32_t ms) {
    syscall(4, ms, 0, 0);
}

static inline void clear(uint32_t color) {
    syscall(5, color, 0, 0);
}

static inline uint8_t get_key(void) {
    return (uint8_t)syscall(6, 0, 0, 0);
}

// Файловые системные вызовы
static inline int open(const char* filename) {
    return (int)syscall(10, (uint32_t)(uintptr_t)filename, 0, 0);
}

static inline int read(int fd, void* buf, int size) {
    return (int)syscall(11, (uint32_t)fd, (uint32_t)(uintptr_t)buf, (uint32_t)size);
}

static inline void close(int fd) {
    syscall(12, (uint32_t)fd, 0, 0);
}

// Графические системные вызовы для Doom (320x200 8-bpp)
static inline void set_palette(const uint32_t* pal) {
    syscall(20, (uint32_t)(uintptr_t)pal, 0, 0);
}

static inline void blit_frame(const uint8_t* frame_buf) {
    syscall(21, (uint32_t)(uintptr_t)frame_buf, 0, 0);
}

#endif // PROG_API_H