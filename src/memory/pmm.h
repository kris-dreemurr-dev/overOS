#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

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