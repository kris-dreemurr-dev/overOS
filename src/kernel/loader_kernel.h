#ifndef LOADER_KERNEL_H
#define LOADER_KERNEL_H

#include <stdint.h>

// Что из подсистем поднялось при загрузке. Заполняет kernel_system_bootstrap,
// читать можно откуда угодно (например, чтобы не предлагать звук там, где
// контроллера нет, или показать состояние в "О системе").
typedef struct {
    int memory, cpu, timer, pci, display, usb, fs, input, audio, net, modules;
    int failed;              // была ошибка, без которой система неполна
    uint32_t ram_mb;
    int pci_devices;
} boot_report_t;

void kernel_system_bootstrap(void);
const boot_report_t* boot_get_report(void);

#endif
