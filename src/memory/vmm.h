#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define VMM_FLAG_PRESENT  (1 << 0)
#define VMM_FLAG_WRITABLE (1 << 1)
#define VMM_FLAG_USER     (1 << 2)
// Бит, свободный для ОС (9): страница разделяемой памяти — принадлежит объекту shm, а не
// процессу; при уничтожении адресного пространства и при fork её не освобождают и не копируют
#define VMM_FLAG_SHARED   (1 << 9)
#define VMM_FLAG_WC (1 << 3)

// Образ ядра слинкован по 0xFFFFFFFF80000000 и живёт там всегда.
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
// Прямое отображение ВСЕЙ физической памяти (PML4[256]). Окно ядра по
// 0xFFFFFFFF80... покрывает лишь первый гигабайт, поэтому любой физический
// адрес ядро видит здесь -- так устроен и Linux (page_offset_base).
#define DMAP_BASE        0xFFFF800000000000ULL
#define PHYS_TO_VIRT(p)  ((void*)((uint64_t)(p) + DMAP_BASE))

// Обратный перевод: указатель ядра может быть и из образа, и из прямого
// отображения, и прямым физическим адресом (identity-область 0-4 ГБ).
static inline uint64_t virt_to_phys(uint64_t va) {
    if (va >= KERNEL_VIRT_BASE) return va - KERNEL_VIRT_BASE;
    if (va >= DMAP_BASE)         return va - DMAP_BASE;
    return va;
}

// Сколько физической памяти нашлось и сколько её отображено прямым окном
uint64_t vmm_dmap_limit(void);

// Инициализация VMM: захват системного PML4 и прямое окно на всю найденную ОЗУ
void vmm_init(uint64_t ram_bytes);

// Создать новый изолированный каталог страниц (PML4) с клонированием ядра
uint64_t* vmm_create_address_space(void);
// Физический адрес PML4 ядра (для CR3): нужен потокам ядра и выходу процесса
uint64_t* vmm_get_kernel_pml4_phys(void);

// Замапить виртуальную страницу на физическую
int vmm_map_page(uint64_t* pml4, uint64_t virt_addr, uint64_t phys_addr, uint32_t flags);

// Переключить адресное пространство (загрузка в CR3)
void vmm_switch_directory(uint64_t* pml4);

void vmm_destroy_address_space(uint64_t* pml4);

uint64_t* vmm_clone_address_space(uint64_t* pml4);

// Функция настройки регистра IA32_PAT
void enable_write_combining(void);
// Тип памяти WC для уже отображённого диапазона (LFB). Возвращает число записей.
int  vmm_set_wc(uint64_t virt, uint64_t size);
int  vmm_set_uc(uint64_t virt, uint64_t size);

// Сисколлы: буфер/строка из Ring 3 целиком отображены в текущем адресном пространстве с правом USER
int vmm_user_range_ok(uint64_t addr, uint64_t len);
int vmm_user_str_ok(uint64_t addr, uint64_t max);

// Снять отображение одной страницы (физическую страницу не освобождает)
void vmm_unmap_page(uint64_t* phys_pml4, uint64_t virt_addr);
// то же, но возвращает физический адрес снятой страницы (0 — нечего снимать)
uint64_t vmm_unmap_page_take(uint64_t* phys_pml4, uint64_t virt_addr);

#endif