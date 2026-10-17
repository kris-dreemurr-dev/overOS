#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

// Карта памяти от BIOS: stage2 кладёт её сюда до перехода в защищённый режим
#define E820_COUNT_ADDR 0x5800
#define E820_LIST_ADDR  0x5810
#define E820_USABLE     1

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
} __attribute__((packed)) e820_entry_t;

// Сколько физической памяти нашлось всего (байт)
extern uint64_t g_ram_bytes;

// Инициализация PMM (передаем размер доступной памяти в байтах)
void pmm_init(uint64_t mem_size_bytes);

// Выделить одну физическую страницу (возвращает физический адрес)
void* pmm_alloc_page(void);

// Освободить физическую страницу
void pmm_free_page(void* p);

// Отметить диапазон памяти как занятый (чтобы не затереть ядро или загрузчик)
void pmm_mark_region_used(uint64_t base, size_t size);

// Отметить диапазон памяти как свободный
void pmm_mark_region_free(uint64_t base, size_t size);

void* pmm_alloc_pages(size_t count);

uint64_t pmm_get_used_blocks(void);
uint64_t pmm_get_total_blocks(void);

#endif