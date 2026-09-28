#include "prog_loader.h"
#include "user_mode.h"
#include "sched.h"
#include "tty.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include <stdint.h>

extern void kputs(const char* str, uint32_t color);
extern void kputc(char c, uint32_t color);
extern void clear_screen(uint32_t color);
extern void flush_buffer(void);
extern void itoa(int n, char* str);
extern int fat16_read_file(const char* filename, uint8_t* buffer, uint32_t max_size);

uint8_t kernel_temp_buf[1024 * 1024 * 4] __attribute__((aligned(4096)));

// Общий kernel_temp_buf: одновременно грузить программу может только одна задача
static volatile int g_load_lock __attribute__((section(".data"))) = 0;

static void format_to_83(const char* src, char* dst) {
    for (int i = 0; i < 11; i++) dst[i] = ' ';
    int i = 0, d = 0;
    while (src[i] && src[i] != '.' && d < 8) {
        char c = src[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[d++] = c;
    }
    if (src[i] == '.') {
        i++;
        d = 8;
        while (src[i] && d < 11) {
            char c = src[i++];
            if (c >= 'a' && c <= 'z') c -= 32;
            dst[d++] = c;
        }
    }
}

int prog_load_module(const char* filename, const char* args) {
    (void)args;

    {
    char b[16];
    itoa((int)g_load_lock, b);
    kputs("[dbg] lock=", 0xFFFF55);
    kputs(b, 0xFFFFFF);
    kputs("\n", 0xFFFFFF);
    flush_buffer();
    }

    while (__sync_lock_test_and_set(&g_load_lock, 1)) sched_yield();

    char name83[11];
    format_to_83(filename, name83);

    kputs("[devOS] Loading program ", 0x00AAAAAA); 
    kputs(filename, 0x00FFFFFF); 
    kputs(" into kernel buffer...\n", 0x00AAAAAA); 
    flush_buffer(); 

    // 1. Читаем файл с накопителя
    // Диск трогаем с выключенными прерываниями: чтобы сисколл другой программы не вошёл в FAT посреди чтения
    uint64_t rd_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rd_flags) :: "memory");
    int bytes = fat16_read_file(name83, kernel_temp_buf, sizeof(kernel_temp_buf));
    __asm__ volatile("pushq %0; popfq" :: "r"(rd_flags) : "memory", "cc"); 
    if (bytes <= 0) {
        kputs("[!] Executable not found or read error.\n", 0x00FF5555); 
        flush_buffer(); 
        __sync_lock_release(&g_load_lock);
        return -1; 
    }

    kputs("[devOS] Read bytes: ", 0x00AAAAAA);
    char bbuf[16];
    itoa(bytes, bbuf); 
    kputs(bbuf, 0x0055FF55); 
    kputs("\n", 0x00AAAAAA); 
    flush_buffer(); 

    // 2. Анализ формата исполняемого файла
    devos_prg_header_t* hdr = (devos_prg_header_t*)kernel_temp_buf;
    uint64_t load_base = PROG_LOAD_BASE;
    uint64_t entry_vaddr = PROG_LOAD_BASE;
    uint64_t payload_offset = 0;
    uint64_t payload_bytes = (uint64_t)bytes;
    uint64_t total_image_bytes = payload_bytes;
    uint32_t stack_size = 512 * 1024;

    if (hdr->magic[0] == 'D' && hdr->magic[1] == 'P' && hdr->magic[2] == 'R' && hdr->magic[3] == 'G') {
        kputs("[devOS] Native DPRG format detected!\n", 0x0055FF55);
        load_base = hdr->load_vaddr ? hdr->load_vaddr : PROG_LOAD_BASE;
        entry_vaddr = hdr->entry_point ? hdr->entry_point : load_base;
        payload_offset = sizeof(devos_prg_header_t);
        payload_bytes = hdr->code_size;
        total_image_bytes = hdr->code_size + hdr->bss_size;
        if (hdr->stack_size) stack_size = (uint32_t)hdr->stack_size;
    } else {
        kputs("[devOS] Legacy flat binary detected.\n", 0x00FFFF55);
    }
    flush_buffer();

    // 3. Сохраняем CR3 ядра
    uint64_t original_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(original_cr3)); 

    // 4. Создаем адресное пространство процесса
    uint64_t* proc_pml4 = vmm_create_address_space(); 
    if (!proc_pml4) {
        kputs("[!] Failed to allocate PML4\n", 0x00FF5555); 
        flush_buffer(); 
        __sync_lock_release(&g_load_lock);
        return 0; 
    }

    // 5. Выделяем страницы под образ (Код + Данные + BSS)
    uint32_t num_image_pages = (total_image_bytes + 4095) / 4096;
    if (num_image_pages == 0) num_image_pages = 1;

    kputs("[devOS] Mapped image pages (Code + Data + BSS): ", 0x00AAAAAA);
    itoa(num_image_pages, bbuf);
    kputs(bbuf, 0x0055FF55);
    kputs("\n", 0x00AAAAAA);
    flush_buffer();

    for (uint32_t i = 0; i < num_image_pages; i++) {
        uint64_t v_addr = load_base + (i * 4096); 
        void* p_addr = pmm_alloc_page(); 
        if (!p_addr) {
            kputs("[!] PMM out of memory for program image!\n", 0x00FF5555);
            flush_buffer(); 
            __asm__ volatile("mov %0, %%cr3" :: "r"(original_cr3) : "memory"); 
            vmm_destroy_address_space(proc_pml4); 
            __sync_lock_release(&g_load_lock);
        return 0; 
        }
        vmm_map_page(proc_pml4, v_addr, (uint64_t)p_addr, VMM_FLAG_USER | VMM_FLAG_WRITABLE); 
    }

    // 6. Выделяем стек процесса
    uint32_t num_stack_pages = (stack_size + 4095) / 4096;
    uint64_t stack_top = 0x00007FFFFFFF0000ULL; 
    uint64_t stack_base = stack_top - (num_stack_pages * 4096); 

    for (uint32_t i = 0; i < num_stack_pages; i++) {
        void* p_addr = pmm_alloc_page(); 
        if (p_addr) {
            vmm_map_page(proc_pml4, stack_base + (i * 4096), (uint64_t)p_addr, VMM_FLAG_USER | VMM_FLAG_WRITABLE); 
        }
    }

    // 7. Копируем тело программы и зануляем секцию .bss
    __asm__ volatile("cli");
    vmm_switch_directory(proc_pml4); 

    uint8_t* target = (uint8_t*)load_base;
    uint8_t* src = kernel_temp_buf + payload_offset;

    for (uint64_t i = 0; i < payload_bytes; i++) {
        target[i] = src[i];
    }
    // Зануление хвоста гарантирует чистоту секций .bss и .lbss
    for (uint64_t i = payload_bytes; i < (uint64_t)(num_image_pages * 4096); i++) {
        target[i] = 0;
    }

    __asm__ volatile("mov %0, %%cr3" :: "r"(original_cr3) : "memory"); 
    __sync_lock_release(&g_load_lock);   // kernel_temp_buf больше не нужен
    __asm__ volatile("sti");

    // 8. Регистрация в планировщике
    uint64_t total_mem = (num_image_pages + num_stack_pages) * 4096;
    task_t* proc_task = sched_register_user_task(filename, (uint64_t)proc_pml4, total_mem);

    if (proc_task) {
        proc_task->heap_start = load_base + (num_image_pages * 4096);
        proc_task->heap_end   = proc_task->heap_start;
        
        kputs("\n[devOS] Process registered in Scheduler! PID: ", 0x0055FF55);
        itoa((int)proc_task->pid, bbuf);
        kputs(bbuf, 0x00FFFFFF);
        kputs("\n", 0x00FFFFFF);
    }
    flush_buffer(); 

    sched_dump_tasks();

    // 9. Запуск процесса в Ring 3 (в задаче оболочки текущего TTY)
    task_t* cur_task = sched_get_current_task();
    uint64_t old_task_cr3 = cur_task ? cur_task->cr3 : original_cr3;
    tty_t* my_tty = (cur_task && cur_task->tty_id >= 0) ? tty_get(cur_task->tty_id) : NULL;

    __asm__ volatile("cli");        // от смены cr3 до iretq вытеснять нельзя
    if (cur_task) {
        cur_task->cr3        = (uint64_t)proc_pml4;
        cur_task->heap_start = load_base + (uint64_t)num_image_pages * 4096;
        cur_task->heap_end   = cur_task->heap_start;
        cur_task->open83[0]  = '\0';
    }
    if (my_tty) my_tty->gfx_mode = 1;   // курсор консоли не рисуем поверх программы
    vmm_switch_directory(proc_pml4);
    sched_set_foreground_task(proc_task);

    int exit_code = run_in_user_mode((void (*)(void))entry_vaddr, (void*)stack_top);

    // Сюда возвращаемся из сисколла exit (IF = 0)
    __asm__ volatile("cli");
    if (cur_task) cur_task->cr3 = old_task_cr3;
    sched_set_foreground_task(NULL);
    if (my_tty) my_tty->gfx_mode = 0;

    // 10. Очистка ресурсов
    __asm__ volatile("mov %0, %%cr3" :: "r"(original_cr3) : "memory");
    if (proc_task) sched_remove_user_task(proc_task);
    vmm_destroy_address_space(proc_pml4);
    kputs("\n[devOS] Process finished with exit code: ", 0x00AAAAAA);
    itoa(exit_code, bbuf);
    kputs(bbuf, 0x0055FF55);
    kputs("\n", 0x00AAAAAA);
    flush_buffer();

    __asm__ volatile("sti");   // после сисколла IF остаётся 0 — возвращаем
    return exit_code;
}