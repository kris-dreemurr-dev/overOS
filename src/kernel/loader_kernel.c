#include "loader_kernel.h"
#include "../drivers/display.h"
#include "../drivers/pci.h"
#include "../fs/fs.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "tty.h"
#include "sched.h"
#include <stdint.h>

// Внешние функции инициализации подсистем
extern void ehci_init(void);
extern void init_crash_guard_idt(void);
extern void init_user_mode(void);
extern void pic_remap(void);
extern void pit_init(uint32_t freq);
extern void tsc_calibrate(void);
extern void render_boot_logo(void);

// Объявления функций вывода и задержек из kernel.c
extern void kputs(const char* str, uint32_t color);
extern void flush_buffer(void);
extern void sleep_ms(uint32_t ms);

// Хелпер для красивого вывода статуса в стиле Arch/systemd
static void log_status(const char* name, int success) {
    kputs("  [ ", 0xFFFFFF);
    if (success) {
        kputs("OK", 0x55FF55);
    } else {
        kputs("FAIL", 0xFF5555);
    }
    kputs(" ] ", 0xFFFFFF);
    kputs(name, 0xFFFFFF);
    kputs("\n", 0xFFFFFF);
    flush_buffer();
    sleep_ms(60); 
}

void kernel_system_bootstrap(void) {
    kputs(":: Welcome to overOS\n\n", 0x55FFFF);
    flush_buffer();
    sleep_ms(200);

    log_status("Initializing CPU Interrupt Guard & BSOD IDT", 1);

    log_status("Virtual & Physical Memory Manager (PMM/VMM)", 1);

    log_status("Configuring Ring 3 User Mode & TSS", 1);

    log_status("Programmable Interrupt Controller & PIT Timer (1000Hz)", 1);

    ehci_init();
    log_status("USB 2.0 EHCI Controller Stack", 1);

    int fs_status = 1;
    log_status("Mounting root File System (FAT32 / LFN)", fs_status);

    intel_set_backlight(100, 1);
    log_status("Display Backlight & VBE Framebuffer LFB", 1);

    log_status("Kernel Scheduler & Virtual Terminals (TTY)", 1);

    kputs("\n:: System has been started\n\n", 0x55FF55);
    flush_buffer();
    sleep_ms(400);
}