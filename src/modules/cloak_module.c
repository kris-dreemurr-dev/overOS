// clock_module.c — Часы реального времени (RTC) с часовыми поясами для devOS (One-shot версия)

#include "../drivers/api.h"
#include <stdint.h>

int driver_entry(const devos_api_t* api);

__attribute__((section(".header")))
const sys_header_t clock_sys_header = {
    .magic = { SYS_MAGIC_0, SYS_MAGIC_1, SYS_MAGIC_2, SYS_MAGIC_3 },
    .entry_point = driver_entry,
    .required_api_ver = 1,
    .driver_name = "CLOCK_SYS",
    .flags = 0
};

static const devos_api_t* g_api = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

static uint8_t read_rtc(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static void print_time_val(uint8_t val, uint32_t col) {
    if (val < 10) {
        g_api->kputc('0', col);
    }
    char buf[16];
    g_api->itoa(val, buf);
    
    char* p = buf;
    while (*p) {
        g_api->kputc(*p++, col);
    }
}

static void print_clocks(void) {
    // Ждем окончания обновления регистров RTC
    outb(0x70, 0x0A);
    while (inb(0x71) & 0x80) {
        __asm__ volatile ("pause");
    }

    uint8_t s = bcd_to_bin(read_rtc(0x00));
    uint8_t m = bcd_to_bin(read_rtc(0x02));
    uint8_t h = bcd_to_bin(read_rtc(0x04));
    
    uint8_t is_pm = h & 0x80;
    h = h & 0x7F;
    if (is_pm) h = (h + 12) % 24;

    uint8_t msk_h  = (h + 3) % 24;
    uint8_t yakt_h = (msk_h + 6) % 24;

    // Вывод времени с секундами для каждого пояса
    g_api->kputs("  [ UTC  ]: ", 0xAAAAAA);
    print_time_val(h, 0xFFFF55); g_api->kputs(":", 0xFFFF55);
    print_time_val(m, 0xFFFF55); g_api->kputs(":", 0xFFFF55);
    print_time_val(s, 0xFFFF55); g_api->kputs("\n", 0xFFFFFF);

    g_api->kputs("  [ MSK  ]: ", 0xAAAAAA);
    print_time_val(msk_h, 0x55FFFF); g_api->kputs(":", 0x55FFFF);
    print_time_val(m, 0x55FFFF); g_api->kputs(":", 0x55FFFF);
    print_time_val(s, 0x55FFFF); g_api->kputs("\n", 0xFFFFFF);

    g_api->kputs("  [ YAKT ]: ", 0xAAAAAA);
    print_time_val(yakt_h, 0x55FF55); g_api->kputs(":", 0x55FF55);
    print_time_val(m, 0x55FF55); g_api->kputs(":", 0x55FF55);
    print_time_val(s, 0x55FF55); g_api->kputs("\n", 0xFFFFFF);
}

int driver_entry(const devos_api_t* api) {
    g_api = api;

    g_api->kputs("\n======================================================\n", 0x55FFFF);
    g_api->kputs("              OVER_OS WORLD CLOCK (RTC)               \n", 0x55FF55);
    g_api->kputs("======================================================\n\n", 0x55FFFF);

    // Вызываем ровно один раз и сразу выходим
    print_clocks();

    g_api->kputs("\n", 0xFFFFFF);
    g_api->flush_buffer();
    
    return 0; // Возврат управления в TTY
}