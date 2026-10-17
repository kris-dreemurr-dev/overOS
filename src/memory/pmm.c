#include "pmm.h"

#include "vmm.h"   // PHYS_TO_VIRT: битмап адресуем через прямое отображение

static uint8_t* pmm_bitmap = (uint8_t*)PHYS_TO_VIRT(0x1F00000);   // 31 МБ: за загрузочным стеком, до 1 МБ (32 ГБ памяти)
static uint64_t pmm_max_blocks = 0;
static uint64_t pmm_used_blocks = 0;

// Подсказка аллокатора: с какого бита искать свободную страницу. Без неё
// pmm_alloc_page() каждый раз шёл от первого бита, и выделение 50 МБ под
// процесс обходилось в 12800 проходов по всему битмапу -- именно отсюда
// росли паузы при открытии картинки или видео. С 16 ГБ ОЗУ битмап -- 4 млн
// бит, и линейный поиск был бы просто неприемлем.
static uint64_t pmm_hint = 1;

uint64_t g_ram_bytes = 0;

// Сколько страниц реально пригодны (их назвал пригодными BIOS). Битмап
// покрывает весь диапазон адресов, включая дыру PCI под 4 ГБ, и без этого
// счётчика "занято" выглядело бы как лишний гигабайт.
static uint64_t pmm_usable_blocks = 0;

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

    pmm_hint = 1;
    pmm_usable_blocks = 0;
    uint64_t bitmap_size = (pmm_max_blocks + 7) / 8;
    for (uint64_t i = 0; i < bitmap_size; i++) {
        pmm_bitmap[i] = 0xFF;
    }
}

void pmm_mark_region_used(uint64_t base, size_t size) {
    uint64_t align_start = base / PAGE_SIZE;
    uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    // Считаем только переход "свободна -> занята": иначе счётчик занятого
    // расходится с битмапом. Этого и не хватало -- 36 МБ самого ядра (и всё,
    // что резервируется так же) не попадали в учёт, и free, fastfetch и
    // диспетчер задач занижали занятую память ровно на эту величину.
    for (uint64_t i = 0; i < num_pages; i++) {
        if (align_start + i < pmm_max_blocks) {
            if (!bitmap_test(align_start + i)) {
                bitmap_set(align_start + i);
                pmm_used_blocks++;
            }
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
                pmm_usable_blocks++;
            }
        }
    }
    if (align_start < pmm_hint) pmm_hint = align_start < 1 ? 1 : align_start;
}

void* pmm_alloc_page(void) {
    // Ищем от подсказки до конца, затем от начала до подсказки: страница 0
    // зарезервирована всегда, потому что (void*)0 == NULL.
    uint64_t start = pmm_hint;
    if (start < 1 || start >= pmm_max_blocks) start = 1;

    for (int pass = 0; pass < 2; pass++) {
        uint64_t from = pass ? 1 : start;
        uint64_t to   = pass ? start : pmm_max_blocks;

        for (uint64_t i = from; i < to; i++) {
            // Байт из одних единиц -- пропускаем сразу восемь занятых страниц
            if ((i & 7) == 0) {
                while (i + 8 <= to && pmm_bitmap[i / 8] == 0xFF) i += 8;
                if (i >= to) break;
            }
            if (!bitmap_test(i)) {
                bitmap_set(i);
                pmm_used_blocks++;
                pmm_hint = i + 1;
                return (void*)(i * PAGE_SIZE);
            }
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
        if (bit < pmm_hint) pmm_hint = bit;   // освободившееся отдадим первым
    }
}

// Непрерывный кусок физической памяти. Такие нужны прежде всего под DMA
// (кольца USB, сетевой карты, звука) и под теневой буфер кадра, а контроллеры
// у нас 32-битные -- поэтому ищем строго ниже 4 ГБ.
void* pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;
    uint64_t found = 0;
    uint64_t start_bit = 0;

    uint64_t limit = pmm_max_blocks;
    const uint64_t dma_limit = 0x100000000ULL / PAGE_SIZE;
    if (limit > dma_limit) limit = dma_limit;

    for (uint64_t i = 1; i < limit; i++) {
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

uint64_t pmm_get_used_blocks(void) {
    uint64_t free_blocks = pmm_max_blocks - pmm_used_blocks;
    return pmm_usable_blocks > free_blocks ? pmm_usable_blocks - free_blocks : 0;
}

uint64_t pmm_get_total_blocks(void) {
    return pmm_usable_blocks ? pmm_usable_blocks : pmm_max_blocks;
}