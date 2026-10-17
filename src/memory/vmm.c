#include "vmm.h"
#include "../kernel/smp.h"
#include "pmm.h"

#define PML4_INDEX(va) (((uint64_t)(va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((uint64_t)(va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((uint64_t)(va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((uint64_t)(va) >> 12) & 0x1FF)

uint64_t* kernel_pml4 = NULL; 
uint64_t kernel_pml4_phys = 0;

// --- Прямое отображение физической памяти (PML4[256] = 0xFFFF8000'00000000) ---
// Таблицы держим в .bss ядра, а не в PMM: прямое окно нужно раньше, чем
// аллокатор сможет отдать хоть страницу, да и адресовать выданную страницу
// без этого окна было бы нечем.
#define DMAP_MAX_GB 64
static uint64_t dmap_pdpt[512]            __attribute__((aligned(4096)));
static uint64_t dmap_pd[DMAP_MAX_GB][512] __attribute__((aligned(4096)));
static uint64_t dmap_limit_bytes = 0;

uint64_t vmm_dmap_limit(void) { return dmap_limit_bytes; }

// Прямое окно строим страницами по 2 МБ: гигабайтные есть не на каждом
// процессоре, а выигрыш здесь не стоит проверки CPUID.
static void dmap_build(uint64_t ram_bytes) {
    uint64_t gb = (ram_bytes + 0x3FFFFFFFULL) / 0x40000000ULL;
    if (gb < 4) gb = 4;                         // минимум 4 ГБ: туда попадают MMIO-окна
    if (gb > DMAP_MAX_GB) gb = DMAP_MAX_GB;

    for (int i = 0; i < 512; i++) dmap_pdpt[i] = 0;

    for (uint64_t g = 0; g < gb; g++) {
        for (int i = 0; i < 512; i++) {
            uint64_t paddr = g * 0x40000000ULL + (uint64_t)i * 0x200000ULL;
            dmap_pd[g][i] = paddr | 0x83;        // 2 МБ | Writable | Present, без USER
        }
        dmap_pdpt[g] = ((uint64_t)&dmap_pd[g][0] - KERNEL_VIRT_BASE) | 0x03;
    }

    dmap_limit_bytes = gb * 0x40000000ULL;
    kernel_pml4[256] = ((uint64_t)&dmap_pdpt[0] - KERNEL_VIRT_BASE) | 0x03;
}

void vmm_init(uint64_t ram_bytes) {
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    kernel_pml4_phys = cr3_val & ~0xFFF; 
    // Пока прямого окна нет, PML4 достаём через образ ядра (первый гигабайт).
    kernel_pml4 = (uint64_t*)(kernel_pml4_phys + KERNEL_VIRT_BASE);

    // Первым делом -- прямое окно: дальше всё в ядре адресует физику через него
    dmap_build(ram_bytes);
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
    kernel_pml4 = (uint64_t*)PHYS_TO_VIRT(kernel_pml4_phys);

    // Получаем PDPT ядра (индекс 511 в PML4 = Higher-Half)
    uint64_t phys_pdpt = kernel_pml4[511] & ~0xFFFULL;
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(phys_pdpt);

    // Получаем PD ядра (индекс 510 в PDPT = виртуальный адрес 0xFFFFFFFF80000000)
    uint64_t phys_pd = pdpt[510] & ~0xFFFULL;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(phys_pd);

    // Окно образа ядра: первый гигабайт страницами по 2 МБ, без бита USER
    for (int i = 0; i < 512; i++) {
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

    // Клонируем Higher-Half ядро и прямое отображение физической памяти:
    // ядро работает с ними и внутри сисколла, то есть на CR3 процесса.
    new_pml4[511] = kernel_pml4[511];
    new_pml4[256] = kernel_pml4[256];

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
    // Рабочие ядра, считающие в этом пространстве, снимаем раньше, чем
    // отдадим его страницы (иначе они продолжат писать в чужую память)
    smp_stop_space((uint64_t)phys_pml4);
    
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(phys_pml4);
    if (pml4 == kernel_pml4) return;

    // Индекс 0 теперь личный PDPT процесса; общие PD identity-области (j = 0..3) не освобождаем
    for (int i = 0; i < 511; i++) {
        if (i == 256) continue;                 // прямое окно ядра -- общее, оно не наше
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
                    if ((pt[m] & VMM_FLAG_PRESENT) && !(pt[m] & VMM_FLAG_SHARED)) {
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
        if (i == 256) continue;                 // прямое окно ядра, у ребёнка уже есть
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
                    if (parent_pt[m] & VMM_FLAG_SHARED) continue;   // shm ребёнку не наследуется
                    
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

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

// Включение режима Write-Combining в таблице PAT процессора
void enable_write_combining(void) {
    // Читаем текущий IA32_PAT MSR (0x277)
    uint64_t pat = rdmsr(0x277);

    // Запись PAT1 (биты 15:8) по умолчанию равна 0x04 (Write-Through).
    // Перенастраиваем её на 0x01 (Write-Combining):
    pat &= ~(0xFFULL << 8);
    pat |=  (0x01ULL << 8);

    wrmsr(0x277, pat);

    // Сбрасываем кэш и конвейер процессора
    __asm__ volatile ("wbinvd" ::: "memory");
}
// Перевести уже отображённый диапазон в тип памяти write-combining (PAT1).
//
// Нужно для LFB: identity-область 0..4 ГБ построена страницами по 2 МБ, и
// vmm_map_page на такой записи ничего не делает (возвращает 0) — флаг WC
// видеопамяти раньше не ставился вовсе. В QEMU это незаметно, а на железе
// прошивка отдаёт видеопамять как UC (некэшируемую): каждая запись идёт на
// шину отдельно, и кадр 1600x826 выводился десятки миллисекунд при
// выключенных прерываниях — вставала вся система вместе с курсором.
// Правим записи любого размера на месте: PWT=1, PCD=0, бит PAT=0 -> индекс 1.
// Возвращает, сколько записей изменено.
#define VMM_ADDR_MASK 0x000FFFFFFFFFF000ULL
static int vmm_set_type(uint64_t virt, uint64_t size, int pcd);
int vmm_set_wc(uint64_t virt, uint64_t size) { return vmm_set_type(virt, size, 0); }
// UC (PAT3: PCD=1, PWT=1) — для регистров устройств, например LAPIC
int vmm_set_uc(uint64_t virt, uint64_t size) { return vmm_set_type(virt, size, 1); }

static int vmm_set_type(uint64_t virt, uint64_t size, int pcd) {
    const uint64_t setb = (1ULL << 3) | (pcd ? (1ULL << 4) : 0);
    uint64_t va = virt & ~0xFFFULL, end = virt + size;
    int changed = 0;
    while (va < end) {
        uint64_t* e = &kernel_pml4[PML4_INDEX(va)];
        if (!(*e & VMM_FLAG_PRESENT)) { va = (va | ((1ULL << 39) - 1)) + 1; continue; }
        uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(*e & VMM_ADDR_MASK);
        e = &pdpt[PDPT_INDEX(va)];
        if (!(*e & VMM_FLAG_PRESENT)) { va = (va | ((1ULL << 30) - 1)) + 1; continue; }
        if (*e & 0x80) {                                   // 1 ГБ: бит PAT — 12
            *e = (*e & ~((1ULL << 4) | (1ULL << 12))) | setb;
            changed++;
            va = (va | ((1ULL << 30) - 1)) + 1;
            continue;
        }
        uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(*e & VMM_ADDR_MASK);
        e = &pd[PD_INDEX(va)];
        if (!(*e & VMM_FLAG_PRESENT)) { va = (va | ((1ULL << 21) - 1)) + 1; continue; }
        if (*e & 0x80) {                                   // 2 МБ: бит PAT — 12
            *e = (*e & ~((1ULL << 4) | (1ULL << 12))) | setb;
            changed++;
            va = (va | ((1ULL << 21) - 1)) + 1;
            continue;
        }
        uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(*e & VMM_ADDR_MASK);
        e = &pt[PT_INDEX(va)];
        if (*e & VMM_FLAG_PRESENT) {                       // 4 КБ: бит PAT — 7
            *e = (*e & ~((1ULL << 4) | (1ULL << 7))) | setb;
            changed++;
        }
        va += 0x1000;
    }
    // Старый тип мог остаться в TLB и в кэше: сбрасываем оба
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("wbinvd; mov %0, %%cr3" : : "r"(cr3) : "memory");
    return changed;
}

// ---------------------------------------------------------------------------
// Проверка пользовательских указателей для сисколлов: страница отображена, и на
// КАЖДОМ уровне таблиц стоит бит USER (identity-область ядра 0..4 ГБ есть в каждом
// процессе, но без USER — Ring 3 её не видит, и ядро не должно писать туда по его просьбе).
// ---------------------------------------------------------------------------
#define USER_SPACE_END 0x0000800000000000ULL

static int vmm_user_page_ok(uint64_t* pml4, uint64_t va) {
    const uint64_t need = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    uint64_t e = pml4[PML4_INDEX(va)];
    if ((e & need) != need) return 0;
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(e & ~0xFFFULL);
    e = pdpt[PDPT_INDEX(va)];
    if ((e & need) != need) return 0;
    if (e & 0x80) return 1;                        // 1 ГБ страница
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(e & 0x000FFFFFFFFFF000ULL);
    e = pd[PD_INDEX(va)];
    if ((e & need) != need) return 0;
    if (e & 0x80) return 1;                        // 2 МБ страница
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(e & 0x000FFFFFFFFFF000ULL);
    e = pt[PT_INDEX(va)];
    return (e & need) == need;
}

static uint64_t* vmm_current_pml4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t*)PHYS_TO_VIRT(cr3 & 0x000FFFFFFFFFF000ULL);
}

int vmm_user_range_ok(uint64_t addr, uint64_t len) {
    if (len == 0) return 1;
    if (addr >= USER_SPACE_END || len > USER_SPACE_END - addr) return 0;
    uint64_t* pml4 = vmm_current_pml4();
    uint64_t page = addr & ~0xFFFULL;
    uint64_t end  = addr + len - 1;
    for (; page <= end; page += 4096) {
        if (!vmm_user_page_ok(pml4, page)) return 0;
    }
    return 1;
}

int vmm_user_str_ok(uint64_t addr, uint64_t max) {
    uint64_t* pml4 = vmm_current_pml4();
    uint64_t checked_page = 1;   // невыровненное значение = "ещё не проверяли"
    for (uint64_t i = 0; i < max; i++) {
        uint64_t va = addr + i;
        if (va >= USER_SPACE_END) return 0;
        uint64_t page = va & ~0xFFFULL;
        if (page != checked_page) {
            if (!vmm_user_page_ok(pml4, page)) return 0;
            checked_page = page;
        }
        if (*(const volatile char*)va == '\0') return 1;
    }
    return 1;   // длиннее max — ядро всё равно читает не больше max байт
}

// Снять отображение и вернуть физический адрес страницы (0 — её не было).
// Нужно для sys_brk: при сжатии кучи страницы надо отдать обратно в PMM.
uint64_t vmm_unmap_page_take(uint64_t* phys_pml4, uint64_t va) {
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(phys_pml4);
    uint64_t e = pml4[PML4_INDEX(va)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(e & 0x000FFFFFFFFFF000ULL);
    e = pdpt[PDPT_INDEX(va)];
    if (!(e & VMM_FLAG_PRESENT) || (e & 0x80)) return 0;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(e & 0x000FFFFFFFFFF000ULL);
    e = pd[PD_INDEX(va)];
    if (!(e & VMM_FLAG_PRESENT) || (e & 0x80)) return 0;
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(e & 0x000FFFFFFFFFF000ULL);
    uint64_t pte = pt[PT_INDEX(va)];
    if (!(pte & VMM_FLAG_PRESENT)) return 0;
    if (pte & VMM_FLAG_SHARED) return 0;        // чужая страница: отдавать её в PMM нельзя
    pt[PT_INDEX(va)] = 0;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & 0x000FFFFFFFFFF000ULL) == (uint64_t)phys_pml4) __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    smp_flush_space((uint64_t)phys_pml4);       // у рабочих ядер эта страница ещё в TLB
    return pte & 0x000FFFFFFFFFF000ULL;
}

void vmm_unmap_page(uint64_t* phys_pml4, uint64_t va) {
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(phys_pml4);
    uint64_t e = pml4[PML4_INDEX(va)];
    if (!(e & VMM_FLAG_PRESENT)) return;
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(e & 0x000FFFFFFFFFF000ULL);
    e = pdpt[PDPT_INDEX(va)];
    if (!(e & VMM_FLAG_PRESENT) || (e & 0x80)) return;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(e & 0x000FFFFFFFFFF000ULL);
    e = pd[PD_INDEX(va)];
    if (!(e & VMM_FLAG_PRESENT) || (e & 0x80)) return;
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(e & 0x000FFFFFFFFFF000ULL);
    pt[PT_INDEX(va)] = 0;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & 0x000FFFFFFFFFF000ULL) == (uint64_t)phys_pml4) __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    smp_flush_space((uint64_t)phys_pml4);       // у рабочих ядер эта страница ещё в TLB
}
