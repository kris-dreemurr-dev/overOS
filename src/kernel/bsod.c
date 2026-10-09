#include "bsod.h"
#include "user_mode.h"
#include <stdint.h>
#include "config.h"
#include "sched.h"

volatile jit_crash_guard_t g_jit_guard = { 0, 0, 0 };

extern void clear_screen(uint32_t color);
extern void kputs(const char* str, uint32_t color);
extern void kputc(char c, uint32_t color);
extern void flush_buffer(void);
extern uint8_t inb(uint16_t port);
extern void sleep_ms(uint32_t ms);

static void bsod_print_num(int val) {
    if (val == 0) { kputc('0', 0xFFFFFF); return; }
    if (val < 0) { kputc('-', 0xFFFFFF); val = -val; }
    char temp[16]; int t = 0;
    while (val > 0) { temp[t++] = '0' + (val % 10); val /= 10; }
    for (int j = t - 1; j >= 0; j--) kputc(temp[j], 0xFFFFFF);
}

static void bsod_print_hex64(uint64_t val) {
    kputs("0x", 0xAAAAAA);
    char hex_chars[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        kputc(hex_chars[(val >> (i * 4)) & 0xF], 0xFFFF55);
    }
}

static void bsod_print_hex16(uint16_t val) {
    kputs("0x", 0xAAAAAA);
    char hex_chars[] = "0123456789ABCDEF";
    for (int i = 3; i >= 0; i--) {
        kputc(hex_chars[(val >> (i * 4)) & 0xF], 0xFFFF55);
    }
}

static void bsod_print_hex8(uint8_t val) {
    char hex_chars[] = "0123456789ABCDEF";
    kputc(hex_chars[(val >> 4) & 0xF], 0xFFFF55);
    kputc(hex_chars[val & 0xF], 0xFFFF55);
}

static const char* get_exception_name(int exc_no) {
    switch (exc_no) {
        case 0:  return "DIVIDE_BY_ZERO";
        case 1:  return "DEBUG_EXCEPTION";
        case 2:  return "NON_MASKABLE_INTERRUPT";
        case 3:  return "BREAKPOINT";
        case 4:  return "OVERFLOW";
        case 5:  return "BOUND_RANGE_EXCEEDED";
        case 6:  return "INVALID_OPCODE";
        case 7:  return "DEVICE_NOT_AVAILABLE";
        case 8:  return "DOUBLE_FAULT";
        case 10: return "INVALID_TSS";
        case 11: return "SEGMENT_NOT_PRESENT";
        case 12: return "STACK_SEGMENT_FAULT";
        case 13: return "GENERAL_PROTECTION_FAULT";
        case 14: return "PAGE_FAULT";
        case 16: return "x87_FLOAT_EXCEPTION";
        case 17: return "ALIGNMENT_CHECK";
        case 18: return "MACHINE_CHECK";
        case 19: return "SIMD_FLOATING_POINT_FAULT";
        default: return "UNKNOWN_FATAL_EXCEPTION";
    }
}

void show_bsod(const char* reason, uint32_t error_code, int exc_no, uint64_t fault_eip, uint64_t fault_esp) {
    uint64_t r_rax = 0, r_rbx = 0, r_rcx = 0, r_rdx = 0, r_rsi = 0, r_rdi = 0, r_rbp = 0;
    uint64_t r_r8 = 0,  r_r9 = 0,  r_r10 = 0, r_r11 = 0, r_r12 = 0, r_r13 = 0, r_r14 = 0, r_r15 = 0;
    uint64_t rflags = 0;
    uint64_t cr0 = 0, cr2 = 0, cr3 = 0, cr4 = 0, efer = 0;
    uint16_t cs = 0, ds = 0, ss = 0;

    // 1. Считываем регистры общего назначения, флаги и селекторы
    __asm__ volatile (
        "movq %%rax, %0\n\t"
        "movq %%rbx, %1\n\t"
        "movq %%rcx, %2\n\t"
        "movq %%rdx, %3\n\t"
        "movq %%rsi, %4\n\t"
        "movq %%rdi, %5\n\t"
        "movq %%rbp, %6\n\t"
        "movq %%r8,  %7\n\t"
        "movq %%r9,  %8\n\t"
        "movq %%r10, %9\n\t"
        "movq %%r11, %10\n\t"
        "movq %%r12, %11\n\t"
        "movq %%r13, %12\n\t"
        "movq %%r14, %13\n\t"
        "movq %%r15, %14\n\t"
        "pushfq\n\t"
        "popq %15\n\t"
        "movw %%cs, %16\n\t"
        "movw %%ds, %17\n\t"
        "movw %%ss, %18\n\t"
        : "=m"(r_rax), "=m"(r_rbx), "=m"(r_rcx), "=m"(r_rdx),
          "=m"(r_rsi), "=m"(r_rdi), "=m"(r_rbp),
          "=m"(r_r8),  "=m"(r_r9),  "=m"(r_r10), "=m"(r_r11),
          "=m"(r_r12), "=m"(r_r13), "=m"(r_r14), "=m"(r_r15),
          "=m"(rflags),
          "=m"(cs), "=m"(ds), "=m"(ss)
        :
        : "memory"
    );

    // 2. Считываем системные регистры управления и MSR EFER (0xC0000080)
    __asm__ volatile (
        "movq %%cr0, %%rax\n\t"
        "movq %%rax, %0\n\t"
        "movq %%cr2, %%rax\n\t"
        "movq %%rax, %1\n\t"
        "movq %%cr3, %%rax\n\t"
        "movq %%rax, %2\n\t"
        "movq %%cr4, %%rax\n\t"
        "movq %%rax, %3\n\t"
        "movl $0xC0000080, %%ecx\n\t"
        "rdmsr\n\t"
        "shlq $32, %%rdx\n\t"
        "orq %%rdx, %%rax\n\t"
        "movq %%rax, %4\n\t"
        : "=m"(cr0), "=m"(cr2), "=m"(cr3), "=m"(cr4), "=m"(efer)
        :
        : "rax", "rcx", "rdx", "memory"
    );

    clear_screen(0x000088); 

    kputs("\n  :(  devOS x86_64 CRASH SCREEN - STOP CODE: ", 0xFFFFFF);
    bsod_print_hex64(error_code);
    kputs("\n\n", 0xFFFFFF);

    kputs("  * Description:     ", 0x55FF55);
    kputs(reason ? reason : "Unknown Error", 0xFFFFFF);
    kputs("\n", 0xFFFFFF);

    if (exc_no >= 0) {
        kputs("  * Exception:       ", 0xFFFF55);
        bsod_print_num(exc_no);
        kputs(" [", 0xAAAAAA);
        kputs(get_exception_name(exc_no), 0xFFFFFF);
        kputs("]\n", 0xAAAAAA);
    }

    // --- REGISTERS (3 колонки с точным выравниванием по 78 символов) ---
    kputs("\n  ------------------ CPU REGISTERS (x86_64) -------------------\n", 0x00FF88);
    kputs("  RIP = ", 0x55FFFF); bsod_print_hex64(fault_eip);
    kputs("  RSP = ", 0x55FFFF); bsod_print_hex64(fault_esp);
    kputs("  RBP = ", 0x55FFFF); bsod_print_hex64(r_rbp); kputs("\n", 0xFFFFFF);

    kputs("  RAX = ", 0x55FFFF); bsod_print_hex64(r_rax);
    kputs("  RBX = ", 0x55FFFF); bsod_print_hex64(r_rbx);
    kputs("  RCX = ", 0x55FFFF); bsod_print_hex64(r_rcx); kputs("\n", 0xFFFFFF);

    kputs("  RDX = ", 0x55FFFF); bsod_print_hex64(r_rdx);
    kputs("  RSI = ", 0x55FFFF); bsod_print_hex64(r_rsi);
    kputs("  RDI = ", 0x55FFFF); bsod_print_hex64(r_rdi); kputs("\n", 0xFFFFFF);

    kputs("  R8  = ", 0x55FFFF); bsod_print_hex64(r_r8);
    kputs("  R9  = ", 0x55FFFF); bsod_print_hex64(r_r9);
    kputs("  R10 = ", 0x55FFFF); bsod_print_hex64(r_r10); kputs("\n", 0xFFFFFF);

    kputs("  R11 = ", 0x55FFFF); bsod_print_hex64(r_r11);
    kputs("  R12 = ", 0x55FFFF); bsod_print_hex64(r_r12);
    kputs("  R13 = ", 0x55FFFF); bsod_print_hex64(r_r13); kputs("\n", 0xFFFFFF);

    kputs("  R14 = ", 0x55FFFF); bsod_print_hex64(r_r14);
    kputs("  R15 = ", 0x55FFFF); bsod_print_hex64(r_r15);
    kputs("  RFL = ", 0x55FFFF); bsod_print_hex64(rflags); kputs("\n", 0xFFFFFF);

    kputs("  CR0 = ", 0xAAAAAA); bsod_print_hex64(cr0);
    kputs("  CR2 = ", 0xFF5555); bsod_print_hex64(cr2);
    kputs("  CR3 = ", 0xAAAAAA); bsod_print_hex64(cr3); kputs("\n", 0xFFFFFF);

    kputs("  CR4 = ", 0xAAAAAA); bsod_print_hex64(cr4);
    kputs("  EFER= ", 0xAAAAAA); bsod_print_hex64(efer);
    kputs("  CS  = ", 0xAAAAAA); bsod_print_hex16(cs);
    if ((cs & 3) == 3) {
        kputs(" [RING 3]\n", 0xFFFF55);
    } else {
        kputs(" [RING 0]\n", 0x55FF55);
    }

    // --- CODE DUMP (16 байт инструкций по адресу RIP) ---
    kputs("\n  ------------------ CODE DUMP (RIP) ---------------------------\n  [RIP+00]: ", 0x00FF88);
    if (fault_eip >= 0x1000) {
        const uint8_t* code = (const uint8_t*)fault_eip;
        for (int i = 0; i < 16; i++) {
            bsod_print_hex8(code[i]);
            kputc(' ', 0xFFFFFF);
        }
    } else {
        kputs("[INVALID RIP ADDRESS]", 0xFF5555);
    }
    kputs("\n", 0xFFFFFF);

    // --- STACK DUMP (первые 4 квадрослова верхушки стека RSP) ---
    kputs("\n  ------------------ STACK DUMP (RSP) --------------------------\n", 0x00FF88);
    if (fault_esp >= 0x1000) {
        const uint64_t* stk = (const uint64_t*)fault_esp;
        kputs("  [RSP+00]: ", 0xAAAAAA); bsod_print_hex64(stk[0]);
        kputs("   [RSP+08]: ", 0xAAAAAA); bsod_print_hex64(stk[1]); kputs("\n", 0xFFFFFF);
        kputs("  [RSP+10]: ", 0xAAAAAA); bsod_print_hex64(stk[2]);
        kputs("   [RSP+18]: ", 0xAAAAAA); bsod_print_hex64(stk[3]); kputs("\n", 0xFFFFFF);
    } else {
        kputs("  [INVALID RSP ADDRESS]\n", 0xFF5555);
    }

    kputs("\n  FATAL KERNEL CRASH. System halted.", 0xFF5555);

    flush_buffer();

    while (inb(0x64) & 1) inb(0x60);
    while (1) {
        if (inb(0x64) & 1) {
            uint8_t sc = inb(0x60);
            if (!(sc & 0x80)) break;
        }
    }
}

void handle_cpu_exception(int exc_no, uint64_t fault_eip, uint64_t fault_esp) {
    if (g_user_mode_active) {
        kputs("\n  [devOS Guard] Crash caught in User Mode (Ring 3)!\n", 0xFF5555);
        kputs("  * Exception: ", 0xFFFF55);
        bsod_print_num(exc_no);
        kputs(" [", 0xAAAAAA);
        kputs(get_exception_name(exc_no), 0xFFFFFF);
        kputs("]\n", 0xAAAAAA);
        
        kputs("  * Fault RIP: ", 0x55FFFF);
        bsod_print_hex64(fault_eip);
        kputs("   Fault RSP: ", 0x55FFFF);
        bsod_print_hex64(fault_esp);
        kputs("\n", 0xFFFFFF);

        kputs("  * Action:    Process terminated. Safely returning to kernel shell.\n\n", 0x55FF55);
        flush_buffer();

        // return_from_user_mode() - это старый путь единого общего стека (до того,
        // как появились настоящие процессы со своим CR3/стеком через fork/spawn);
        // сейчас это пустая заглушка и сюда ничего не возвращало бы вообще.
        // Настоящее восстановление - ровно то же, что sys_exit/kill делают для
        // живого процесса: sched_exit_current не возвращается, родитель заберёт
        // код через wait()/waitpid().
        task_t* t = sched_get_current_task();
        if (t && t->is_process) {
            sched_exit_current(-1);
        }
        return_from_user_mode(-1);   // страховка: недостижимо для настоящих процессов
        return;
    }

#if BSOD_ENABLED
    show_bsod("KERNEL_CRASH", 0x88880001, exc_no, fault_eip, fault_esp);
#else
    // Если BSOD отключен в конфиге, просто выводим короткое сообщение и зависаем на месте
    kputs("\n[KERNEL PANIC] System halted (BSOD disabled in config).\n", 0xFF5555);
    flush_buffer();
#endif

    __asm__ volatile ("cli; hlt");
    while (1);
}

void default_exception_handler(uint64_t* frame) {
    // Извлекаем данные из кадра прерывания, сформированного в interrupts.asm
    int      exc_no    = (int)frame[15];
    uint64_t fault_eip = frame[17];
    uint64_t fault_esp = frame[20];

    handle_cpu_exception(exc_no, fault_eip, fault_esp);
}