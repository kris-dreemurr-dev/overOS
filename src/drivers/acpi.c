#include "acpi.h"
#include <stddef.h>

// Импортируем функции из ядра для вывода текста и работы с портами
extern void kputs(const char* str, uint32_t color);
extern void flush_buffer(void);
extern void outb(uint16_t port, uint8_t val);
extern void outw(uint16_t port, uint16_t val);
extern uint8_t inb(uint16_t port);
extern uint16_t inw(uint16_t port);
extern void sleep_ms(uint32_t ms);

// Вспомогательная функция для сравнения сигнатур (замена strncmp)
static int match_signature(const char* a, const char* b, int length) {
    for (int i = 0; i < length; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

// Поиск корневого указателя ACPI в памяти BIOS (от 0x000E0000 до 0x000FFFFF)
static rsdp_t* find_rsdp(void) {
    for (uint32_t addr = 0x000E0000; addr < 0x00100000; addr += 16) {
        if (match_signature((char*)addr, "RSD PTR ", 8)) {
            // Проверка контрольной суммы
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) {
                sum += ((uint8_t*)addr)[i];
            }
            if (sum == 0) {
                return (rsdp_t*)addr;
            }
        }
    }
    return NULL;
}

void acpi_power_off(void) {
    //kputs("Initializing ACPI Driver...\n", 0xAAAAAA);
    flush_buffer();

    rsdp_t* rsdp = find_rsdp();
    if (!rsdp) {
        kputs("ACPI [FAIL]: RSDP not found. System halted.\n", 0xFF5555);
        flush_buffer();
        __asm__ __volatile__("cli; hlt");
        return;
    }

    acpi_header_t* rsdt = (acpi_header_t*)rsdp->rsdt_address;
    if (!match_signature(rsdt->signature, "RSDT", 4)) {
        kputs("ACPI [FAIL]: Invalid RSDT signature.\n", 0xFF5555);
        flush_buffer();
        return;
    }

    // Ищем таблицу FACP (FADT) в RSDT
    uint32_t entries = (rsdt->length - sizeof(acpi_header_t)) / 4;
    uint32_t* table_pointers = (uint32_t*)((uint8_t*)rsdt + sizeof(acpi_header_t));
    fadt_t* fadt = NULL;

    for (uint32_t i = 0; i < entries; i++) {
        acpi_header_t* header = (acpi_header_t*)table_pointers[i];
        if (match_signature(header->signature, "FACP", 4)) {
            fadt = (fadt_t*)header;
            break;
        }
    }

    if (!fadt) {
        kputs("ACPI [FAIL]: FADT table not found.\n", 0xFF5555);
        flush_buffer();
        return;
    }

    //kputs("ACPI [OK]: Table parsed. Sending shutdown signal...\n", 0x55FF55);
    flush_buffer();

    // Проверяем, включен ли ACPI. Бит 0 (SCI_EN) в PM1a Control Block должен быть равен 1.
    if (!(inw(fadt->pm1a_control_block) & 1)) {
        if (fadt->smi_command_port != 0 && fadt->acpi_enable != 0) {
            outb(fadt->smi_command_port, fadt->acpi_enable);
            // Ждем инициализации ACPI BIOS'ом
            sleep_ms(300); 
        }
    }

    // Отправляем сигнал S5 (Sleep Enable | Sleep Type).
    // Без AML-интерпретатора мы не знаем точный Sleep Type, 
    // поэтому перебираем самые частые значения (QEMU = 0, VBox = 5, Bochs = 7).
    uint16_t pm1a = fadt->pm1a_control_block;
    uint16_t pm1b = fadt->pm1b_control_block;

    // 0x2000 - бит SLP_EN. В биты 10-12 помещается SLP_TYP (0, 5, 7, 1, 6).
    uint16_t s5_values[] = {0, 5, 7, 1, 6}; 

    for (int i = 0; i < 5; i++) {
        uint16_t val = 0x2000 | (s5_values[i] << 10);
        
        outw(pm1a, val); // Отправляем в основной блок
        
        if (pm1b != 0) {
            outw(pm1b, val); // Отправляем во вторичный блок (если есть)
        }
    }

    // Если выполнение дошло сюда, выключение не сработало
    kputs("ACPI Shutdown failed. Halting CPU.\n", 0xFF5555);
    flush_buffer();
    __asm__ __volatile__("cli; hlt");
}