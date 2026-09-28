#include <stdint.h>

extern void kputs(const char* str, uint32_t color);

// Безопасная запись 32-битного значения в порт ввода-вывода (поддерживает порты > 255)
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

// Безопасное чтение 32-битного значения из порта ввода-вывода
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Функция управления аппаратной подсветкой матрицы Intel Graphics
void intel_set_backlight(int percent, int silent) {
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;

    // 1. Формируем адрес конфигурационного пространства PCI (Bus 0, Device 2, Func 0)
    uint32_t address = (1U << 31) | (0 << 16) | (2 << 11) | (0 << 8) | 0x10;

    // 2. Читаем базовый адрес MMIO (BAR0) через корректные функции портов
    outl(0xCF8, address);
    uint32_t bar0 = inl(0xCFC);

    if (bar0 == 0 || bar0 == 0xFFFFFFFF) {
        kputs("[-] Intel GPU MMIO BAR0 not found for backlight!\n", 0xFF5555);
        return;
    }

    // Маскируем служебные флаги типа памяти в BAR0
    uint32_t mmio_base = bar0 & 0xFFFFFFF0;

    // 3. Указатель на аппаратный регистр управления подсветкой BLC_PWM_CTL (смещение 0x48254)
    volatile uint32_t* backlight_ctl = (volatile uint32_t*)(mmio_base + 0x48254);

    // Старшие 16 бит — максимальная граница ШИМ, младшие 16 бит — текущий уровень
    uint32_t max_val = (*backlight_ctl >> 16) & 0xFFFF;
    if (max_val == 0) max_val = 300; // Страховка дефолтным значением

    uint32_t target_val = (max_val * percent) / 100;

    // 4. Записываем рассчитанный уровень яркости в регистр контроллера
    *backlight_ctl = (max_val << 16) | target_val;
    if (silent == 0) {
        kputs("[+] Display backlight adjusted successfully.\n", 0x55FF55);
    }
}