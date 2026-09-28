#ifndef PROG_LOADER_H
#define PROG_LOADER_H

#include <stdint.h>

#define PROG_LOAD_BASE 0x0000004000000000ULL

// 64-байтный заголовок формата devOS PRG (DPRG)
typedef struct {
    char     magic[4];       // Сигнатура "DPRG" (0x44, 0x50, 0x52, 0x47)
    uint32_t version;        // Версия формата (1)
    uint64_t load_vaddr;     // Базовый адрес (0x0000004000000000)
    uint64_t entry_point;    // Точка входа в программу (_start)
    uint64_t code_size;      // Размер бинарного кода (байты)
    uint64_t bss_size;       // Сколько байт обнулить под .bss и .lbss
    uint64_t stack_size;     // Стек процесса (512 КБ = 524288)
    uint64_t reserved[2];    // Резерв (0, 0)
} __attribute__((packed)) devos_prg_header_t;

int prog_load_module(const char* filename, const char* args);

#endif