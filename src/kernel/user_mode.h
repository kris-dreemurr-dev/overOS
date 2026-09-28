#ifndef USER_MODE_H
#define USER_MODE_H

#include <stdint.h>

// Флаг активности User Mode для фильтрации сбоев в bsod.c
extern volatile int g_user_mode_active;

// Состояние мыши (передается в сискол 7)
typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons; // Бит 0: ЛКМ, Бит 1: ПКМ, Бит 2: СКМ
} mouse_state_t;

// Аппаратная структура сегмента состояния задачи x86
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;       // Вершина стека ядра (куда CPU переключается при вызове INT 0x80)
    uint32_t ss0;        // Селектор данных ядра (0x10)
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

// Инициализация дескрипторов Ring 3 в GDT, TSS и вектора 0x80 в IDT
void init_user_mode(void);

// Запуск кода в Ring 3 с передачей стека
int run_in_user_mode(void (*entry_point)(void), void* user_stack_top);

// Безопасный возврат в ядро
void return_from_user_mode(int exit_code);

// Точка тестового запуска Ring 3 из ядра
void test_user_mode(void);

// Установка вершины стека ядра в TSS для прерываний из Ring 3
void set_tss_rsp0(uint64_t rsp0);
void reset_tss_to_default(void);

#endif // USER_MODE_H