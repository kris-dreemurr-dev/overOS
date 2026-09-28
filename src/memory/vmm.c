#include "vmm.h"
#include "pmm.h"

#define PML4_INDEX(va) (((uint64_t)(va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((uint64_t)(va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((uint64_t)(va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((uint64_t)(va) >> 12) & 0x1FF)

// Макрос для перевода физического адреса в Higher-Half виртуальный адрес ядра
#define PHYS_TO_VIRT(p) ((void*)((uint64_t)(p) + 0xFFFFFFFF80000000ULL))

uint64_t* kernel_pml4 = NULL; 
uint64_t kernel_pml4_phys = 0;

void vmm_init(void) {
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    kernel_pml4_phys = cr3_val & ~0xFFF; 
    kernel_pml4 = (uint64_t*)PHYS_TO_VIRT(kernel_pml4_phys); 

    // Получаем PDPT ядра (индекс 511 в PML4 = Higher-Half)
    uint64_t phys_pdpt = kernel_pml4[511] & ~0xFFFULL;
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(phys_pdpt);

    // Получаем PD ядра (индекс 510 в PDPT = виртуальный адрес 0xFFFFFFFF80000000)
    uint64_t phys_pd = pdpt[510] & ~0xFFFULL;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(phys_pd);

    // Мапим первые 512 МБ физической памяти страницами по 2 МБ (256 записей по 2 МБ)
    for (int i = 0; i < 256; i++) {
        uint64_t paddr = (uint64_t)i * 0x200000;
        pd[i] = paddr | 0x83; // 0x80 = 2MB Page, 0x02 = Writable, 0x01 = Present
    }

    // Сбрасываем кэш TLB
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
}

// Добавьте простую функцию-геттер для получения физического адреса PML4
uint64_t* vmm_get_kernel_pml4_phys(void) {
    return (uint64_t*)kernel_pml4_phys;
}

uint64_t* vmm_create_address_space(void) {
    if (!kernel_pml4) {
        uint64_t cr3_val;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
        kernel_pml4 = (uint64_t*)PHYS_TO_VIRT(cr3_val & ~0xFFF);
    }

    void* phys_new_pml4 = pmm_alloc_page();
    if (!phys_new_pml4) return NULL;

    uint64_t* new_pml4 = (uint64_t*)PHYS_TO_VIRT(phys_new_pml4);

    for (int i = 0; i < 512; i++) new_pml4[i] = 0;

    // Клонируем Higher-Half ядро
    new_pml4[511] = kernel_pml4[511];

    // ОБЯЗАТЕЛЬНО: клонируем и низкую identity-область (GDT/TSS/IDT-инфраструктура
    // из entry_kernel.o живёт тут, и CPU обращается к ней при ЛЮБОМ прерывании/
    // syscall/исключении независимо от того, чей CR3 сейчас загружен).
    // Низкая половина: identity 0-4 ГБ (PD общие с ядром), но PDPT СВОЙ у каждого процесса —
    // иначе образы программ (PROG_LOAD_BASE = 0x4000000000) окажутся общими для всех процессов.
    void* phys_pdpt = pmm_alloc_page();
    if (!phys_pdpt) { pmm_free_page(phys_new_pml4); return NULL; }
    uint64_t* pdpt_new = (uint64_t*)PHYS_TO_VIRT(phys_pdpt);
    uint64_t* k_pdpt   = (uint64_t*)PHYS_TO_VIRT(kernel_pml4[0] & ~0xFFFULL);
    for (int i = 0; i < 512; i++) pdpt_new[i] = 0;
    for (int i = 0; i < 4; i++)   pdpt_new[i] = k_pdpt[i];
    new_pml4[0] = (uint64_t)phys_pdpt | 0x07;

    return (uint64_t*)phys_new_pml4;
}

void vmm_destroy_address_space(uint64_t* phys_pml4) {
    if (!phys_pml4) return;
    
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(phys_pml4);
    if (pml4 == kernel_pml4) return;

    // Индекс 0 теперь личный PDPT процесса; общие PD identity-области (j = 0..3) не освобождаем
    for (int i = 0; i < 511; i++) {
        if (!(pml4[i] & VMM_FLAG_PRESENT)) continue;
        uint64_t phys_pdpt = pml4[i] & ~0xFFF;
        uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(phys_pdpt);

        for (int j = (i == 0 ? 4 : 0); j < 512; j++) {
            if (!(pdpt[j] & VMM_FLAG_PRESENT)) continue;
            uint64_t phys_pd = pdpt[j] & ~0xFFF;
            uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(phys_pd);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_FLAG_PRESENT)) continue;
                uint64_t phys_pt = pd[k] & ~0xFFF;
                uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(phys_pt);

                for (int m = 0; m < 512; m++) {
                    if (pt[m] & VMM_FLAG_PRESENT) {
                        pmm_free_page((void*)(pt[m] & ~0xFFF));
                    }
                }
                pmm_free_page((void*)phys_pt);
            }
            pmm_free_page((void*)phys_pd);
        }
        pmm_free_page((void*)phys_pdpt);
    }
    pmm_free_page(phys_pml4);
}

int vmm_map_page(uint64_t* phys_pml4, uint64_t virt_addr, uint64_t phys_addr, uint32_t flags) {
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(phys_pml4);

    int pml4_i = PML4_INDEX(virt_addr);
    int pdpt_i = PDPT_INDEX(virt_addr);
    int pd_i   = PD_INDEX(virt_addr);
    int pt_i   = PT_INDEX(virt_addr);

    // 1. Уровень PML4 -> PDPT
    uint64_t* pdpt;
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) {
        void* phys_pdpt = pmm_alloc_page();
        if (!phys_pdpt) return 0;
        pdpt = (uint64_t*)PHYS_TO_VIRT(phys_pdpt);
        for (int i = 0; i < 512; i++) pdpt[i] = 0;
        // В PML4 пишем физический адрес PDPT
        pml4[pml4_i] = (uint64_t)phys_pdpt | flags | VMM_FLAG_PRESENT;
    } else {
        pdpt = (uint64_t*)PHYS_TO_VIRT(pml4[pml4_i] & ~0xFFF);
    }

    // 2. Уровень PDPT -> PD
    uint64_t* pd;
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) {
        void* phys_pd = pmm_alloc_page();
        if (!phys_pd) return 0;
        pd = (uint64_t*)PHYS_TO_VIRT(phys_pd);
        for (int i = 0; i < 512; i++) pd[i] = 0;
        // В PDPT пишем физический адрес PD
        pdpt[pdpt_i] = (uint64_t)phys_pd | flags | VMM_FLAG_PRESENT;
    } else {
        pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpt_i] & ~0xFFF);
    }

    // 3. Уровень PD -> PT
    // Запись PD может быть 2 МБ-страницей (identity-область ядра, бит PS = 0x80):
    // принимать её за таблицу PT нельзя — PTE запишутся прямо в физическую память,
    // а выделенные страницы потеряются.
    if ((pd[pd_i] & VMM_FLAG_PRESENT) && (pd[pd_i] & 0x80)) return 0;

    uint64_t* pt;
    if (!(pd[pd_i] & VMM_FLAG_PRESENT)) {
        void* phys_pt = pmm_alloc_page();
        if (!phys_pt) return 0;
        pt = (uint64_t*)PHYS_TO_VIRT(phys_pt);
        for (int i = 0; i < 512; i++) pt[i] = 0;
        // В PD пишем физический адрес PT
        pd[pd_i] = (uint64_t)phys_pt | flags | VMM_FLAG_PRESENT;
    } else {
        pt = (uint64_t*)PHYS_TO_VIRT(pd[pd_i] & ~0xFFF);
    }

    // 4. Уровень PT -> Физическая страница данных
    pt[pt_i] = (phys_addr & ~0xFFF) | flags | VMM_FLAG_PRESENT;

    // Сбрасываем кэш TLB
    __asm__ __volatile__("invlpg (%0)" : : "r" (virt_addr) : "memory");

    return 1;
}

void vmm_switch_directory(uint64_t* phys_pml4) {
    // Переключаем CR3 на физический адрес каталога страниц процесса
    __asm__ __volatile__("mov %0, %%cr3" : : "r" ((uint64_t)phys_pml4) : "memory");
}

uint64_t* vmm_clone_address_space(uint64_t* parent_pml4_phys) {
    uint64_t* parent_pml4 = (uint64_t*)PHYS_TO_VIRT(parent_pml4_phys);
    
    // 1. Создаем новое базовое пространство (с ядром и identity mapping)
    uint64_t* child_pml4_phys = (uint64_t*)vmm_create_address_space();
    if (!child_pml4_phys) return NULL;
    
    uint64_t* child_pml4 = (uint64_t*)PHYS_TO_VIRT(child_pml4_phys);

    // 2. Обходим пользовательские пространства (индексы от 0 до 510)
    for (int i = 0; i < 511; i++) {
        if (!(parent_pml4[i] & VMM_FLAG_PRESENT)) continue;
        
        uint64_t parent_pdpt_phys = parent_pml4[i] & ~0xFFF;
        uint64_t* parent_pdpt = (uint64_t*)PHYS_TO_VIRT(parent_pdpt_phys);
        
        for (int j = 0; j < 512; j++) {
            if (!(parent_pdpt[j] & VMM_FLAG_PRESENT)) continue;
            if (i == 0 && j < 4) continue;   // общие с ядром PD identity-области: они уже есть у ребёнка
            
            uint64_t parent_pd_phys = parent_pdpt[j] & ~0xFFF;
            uint64_t* parent_pd = (uint64_t*)PHYS_TO_VIRT(parent_pd_phys);
            
            for (int k = 0; k < 512; k++) {
                if (!(parent_pd[k] & VMM_FLAG_PRESENT)) continue;
                if (parent_pd[k] & 0x80) continue;   // 2 МБ-страница, это не таблица PT
                
                uint64_t parent_pt_phys = parent_pd[k] & ~0xFFF;
                uint64_t* parent_pt = (uint64_t*)PHYS_TO_VIRT(parent_pt_phys);
                
                for (int m = 0; m < 512; m++) {
                    if (!(parent_pt[m] & VMM_FLAG_PRESENT)) continue;
                    
                    uint64_t flags = parent_pt[m] & 0xFFF;
                    uint64_t* src_page = (uint64_t*)PHYS_TO_VIRT(parent_pt[m] & ~0xFFF);
                    
                    // Выделяем новую физическую страницу для ребенка
                    void* new_phys_page = pmm_alloc_page();
                    if (!new_phys_page) {
                        // Очистка при ошибке (можно дописать при необходимости)
                        vmm_destroy_address_space(child_pml4_phys);
                        return NULL;
                    }
                    
                    uint64_t* dst_page = (uint64_t*)PHYS_TO_VIRT(new_phys_page);
                    
                    // Копируем содержимое памяти 4 КБ
                    for (int p = 0; p < 512; p++) {
                        dst_page[p] = src_page[p];
                    }
                    
                    // Собираем виртуальный адрес из индексов
                    uint64_t virt_addr = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21) | ((uint64_t)m << 12);
                    
                    // Мапим скопированную страницу в пространство ребенка
                    vmm_map_page(child_pml4_phys, virt_addr, (uint64_t)new_phys_page, flags);
                }
            }
        }
    }
    
    return child_pml4_phys;
}