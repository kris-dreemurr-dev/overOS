#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define VMM_FLAG_PRESENT  (1 << 0)
#define VMM_FLAG_WRITABLE (1 << 1)
#define VMM_FLAG_USER     (1 << 2)

#define PHYS_TO_VIRT(p) ((void*)((uint64_t)(p) + 0xFFFFFFFF80000000ULL))

// Инициализация VMM (захват системного PML4)
void vmm_init(void);

// Создать новый изолированный каталог страниц (PML4) с клонированием ядра
uint64_t* vmm_create_address_space(void);

// Замапить виртуальную страницу на физическую
int vmm_map_page(uint64_t* pml4, uint64_t virt_addr, uint64_t phys_addr, uint32_t flags);

// Переключить адресное пространство (загрузка в CR3)
void vmm_switch_directory(uint64_t* pml4);

void vmm_destroy_address_space(uint64_t* pml4);

uint64_t* vmm_clone_address_space(uint64_t* pml4);

#endif