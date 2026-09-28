#include "pmm.h"

// Переводим указатель битмапа в Higher-Half адрес ядра (0xFFFFFFFF81000000)
#define PHYS_TO_VIRT(p) ((void*)((uint64_t)(p) + 0xFFFFFFFF80000000ULL))

static uint8_t* pmm_bitmap = (uint8_t*)PHYS_TO_VIRT(0x1F00000);   // 31 МБ: за загрузочным стеком, до 1 МБ (32 ГБ памяти)
static uint64_t pmm_max_blocks = 0;
static uint64_t pmm_used_blocks = 0;

static inline void bitmap_set(uint64_t bit) {
    pmm_bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(uint64_t bit) {
    pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline int bitmap_test(uint64_t bit) {
    return (pmm_bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

void pmm_init(uint64_t mem_size_bytes) {
    pmm_max_blocks = mem_size_bytes / PAGE_SIZE;
    pmm_used_blocks = pmm_max_blocks;

    uint64_t bitmap_size = (pmm_max_blocks + 7) / 8;
    for (uint64_t i = 0; i < bitmap_size; i++) {
        pmm_bitmap[i] = 0xFF;
    }
}

void pmm_mark_region_used(uint64_t base, size_t size) {
    uint64_t align_start = base / PAGE_SIZE;
    uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < num_pages; i++) {
        if (align_start + i < pmm_max_blocks) {
            bitmap_set(align_start + i);
        }
    }
}

void pmm_mark_region_free(uint64_t base, size_t size) {
    uint64_t align_start = base / PAGE_SIZE;
    uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < num_pages; i++) {
        if (align_start + i < pmm_max_blocks) {
            if (bitmap_test(align_start + i)) {
                bitmap_clear(align_start + i);
                pmm_used_blocks--;
            }
        }
    }
}

void* pmm_alloc_page(void) {
    // Начинаем строго с 1: страница 0 зарезервирована, а (void*)0 == NULL!
    for (uint64_t i = 1; i < pmm_max_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            pmm_used_blocks++;
            return (void*)(i * PAGE_SIZE);
        }
    }
    return NULL; // Реальный Out of memory
}

void pmm_free_page(void* p) {
    if (!p) return; // Защита от free(NULL)
    uint64_t addr = (uint64_t)p;
    uint64_t bit = addr / PAGE_SIZE;
    
    // Никогда не освобождаем страницу 0 и защищаем границы массива
    if (bit == 0 || bit >= pmm_max_blocks) return;

    if (bitmap_test(bit)) {
        bitmap_clear(bit);
        pmm_used_blocks--;
    }
}

void* pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;
    uint64_t found = 0;
    uint64_t start_bit = 0;

    for (uint64_t i = 1; i < pmm_max_blocks; i++) {
        if (!bitmap_test(i)) {
            if (found == 0) start_bit = i;
            found++;
            if (found == count) {
                for (uint64_t j = start_bit; j < start_bit + count; j++) {
                    bitmap_set(j);
                }
                pmm_used_blocks += count;
                return (void*)(start_bit * PAGE_SIZE);
            }
        } else {
            found = 0;
        }
    }
    return NULL;
}

uint64_t pmm_free_pages(void) { return pmm_max_blocks - pmm_used_blocks; }