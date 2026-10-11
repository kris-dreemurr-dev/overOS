#ifndef PROG_LOADER_H
#define PROG_LOADER_H

#include <stdint.h>

#define PROG_LOAD_BASE 0x0000004000000000ULL

#define PRG_FLAG_CLI      0x00ULL
#define PRG_FLAG_DESKTOP  0x01ULL

typedef struct {
    char     magic[4];       // "DPRG"
    uint32_t version;        // 1
    uint64_t load_vaddr;     // 0x4000000000
    uint64_t entry_point;    // Точка входа
    uint64_t code_size;      // Байт кода
    uint64_t bss_size;       // Байт .bss
    uint64_t stack_size;     // Стек
    uint64_t flags;          // Битовые флаги подсистемы (0 = TTY, 1 = Desktop)
    uint64_t reserved;       // Резерв (0)
} __attribute__((packed)) devos_prg_header_t;

int prog_load_module(const char* filename, const char* args);
int prog_read_file_by_path(const char* path, uint8_t* buffer, uint32_t max_size);

#endif