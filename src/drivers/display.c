#include <stdint.h>
#include "../memory/vmm.h"

extern uint64_t* vmm_get_kernel_pml4_phys(void);
extern void kputs(const char* str, uint32_t color);

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Кэшированный указатель на регистр подсветки
static volatile uint32_t* s_backlight_ctl = 0;
static uint32_t s_max_pwm = 0;

void intel_set_backlight(int percent, int silent) {
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;

    // Инициализация при первом вызове
    if (!s_backlight_ctl) {
        uint32_t address = (1U << 31) | (0 << 16) | (2 << 11) | (0 << 8) | 0x10;
        outl(0xCF8, address);
        uint32_t bar0 = inl(0xCFC);

        if (bar0 == 0 || bar0 == 0xFFFFFFFF) {
            if (!silent) kputs("[-] Intel GPU MMIO BAR0 not found!\n", 0xFF5555);
            return;
        }

        uint32_t mmio_base = bar0 & 0xFFFFFFF0;
        uintptr_t reg_phys = mmio_base + 0x48254;

        // Маппим 4 КБ страницу регистра
        uintptr_t page = reg_phys & ~0xFFFULL;
        vmm_map_page(vmm_get_kernel_pml4_phys(), page, page, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (1 << 4));

        s_backlight_ctl = (volatile uint32_t*)reg_phys;
        s_max_pwm = (*s_backlight_ctl >> 16) & 0xFFFF;
        if (s_max_pwm == 0) s_max_pwm = 300;
    }

    // Мгновенная запись нового уровня без задержек шины
    uint32_t target_val = (s_max_pwm * percent) / 100;
    *s_backlight_ctl = (s_max_pwm << 16) | target_val;

    if (silent == 0) {
        kputs("[+] Display backlight adjusted successfully.\n", 0x55FF55);
    }
}