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
extern int fs_read_file(const char* filename, uint8_t* buffer, uint32_t max_size);
extern void fs_lock(void);
extern void fs_unlock(void);

uint8_t kernel_temp_buf[1024 * 1024 * 4] __attribute__((aligned(4096)));

int prog_load_module(const char* filename, const char* args) {
    (void)args;

    // kernel_temp_buf общий с sys_execve/sys_loader; fs_lock также защищает
    // от конкурентного доступа к общему состоянию драйвера fs_* (fat32.c)
    fs_lock();

    char name83[11];

    kputs("[overOS] Loading program ", 0x00AAAAAA); 
    kputs(filename, 0x00FFFFFF); 
    kputs(" into kernel buffer...\n", 0x00AAAAAA); 
    flush_buffer(); 

    // 1. Читаем файл с накопителя
    // Диск трогаем с выключенными прерываниями: чтобы сисколл другой программы не вошёл в FAT посреди чтения
    uint64_t rd_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rd_flags) :: "memory");
    int bytes = fs_read_file(filename, kernel_temp_buf, sizeof(kernel_temp_buf));
    __asm__ volatile("pushq %0; popfq" :: "r"(rd_flags) : "memory", "cc"); 
    if (bytes <= 0) {
        kputs("[!] Executable not found or read error.\n", 0x00FF5555); 
        flush_buffer(); 
        fs_unlock();
        return -1; 
    }

    kputs("[overOS] Read bytes: ", 0x00AAAAAA);
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
        kputs("[overOS] Native DPRG format detected!\n", 0x0055FF55);
        load_base = hdr->load_vaddr ? hdr->load_vaddr : PROG_LOAD_BASE;
        entry_vaddr = hdr->entry_point ? hdr->entry_point : load_base;
        payload_offset = sizeof(devos_prg_header_t);
        payload_bytes = hdr->code_size;
        total_image_bytes = hdr->code_size + hdr->bss_size;
        if (hdr->stack_size) stack_size = (uint32_t)hdr->stack_size;
    } else {
        kputs("[overOS] Legacy flat binary detected.\n", 0x00FFFF55);
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
        fs_unlock();
        return 0; 
    }

    // 5. Выделяем страницы под образ (Код + Данные + BSS)
    uint32_t num_image_pages = (total_image_bytes + 4095) / 4096;
    if (num_image_pages == 0) num_image_pages = 1;

    kputs("[overOS] Mapped image pages (Code + Data + BSS): ", 0x00AAAAAA);
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
            fs_unlock();
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
    uint64_t cp_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(cp_flags) :: "memory");
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
    fs_unlock();   // kernel_temp_buf и состояние fs_* больше не нужны
    __asm__ volatile("pushq %0; popfq" :: "r"(cp_flags) : "memory", "cc");

    // 8. Запускаем ОТДЕЛЬНЫЙ процесс: свой ядерный стек, своё адресное пространство.
    //    Оболочка (текущая задача) остаётся сама собой и просто ждёт его завершения.
    task_t* shell = sched_get_current_task();
    tty_t*  my_tty = (shell && shell->tty_id >= 0) ? tty_get(shell->tty_id) : NULL;

    uint64_t total_mem  = (uint64_t)(num_image_pages + num_stack_pages) * 4096;
    uint64_t heap_start = load_base + (uint64_t)num_image_pages * 4096;

    task_t* proc = sched_spawn_process(filename, (uint64_t)proc_pml4, total_mem,
                                       entry_vaddr, stack_top, heap_start);
    if (!proc) {
        kputs("[!] Failed to spawn process\n", 0x00FF5555);
        flush_buffer();
        vmm_destroy_address_space(proc_pml4);
        return -1;
    }

    kputs("[overOS] Process started, PID: ", 0x00AAAAAA);
    itoa((int)proc->pid, bbuf);
    kputs(bbuf, 0x00FFFFFF);
    kputs("\n", 0x00FFFFFF);
    flush_buffer();

    if (my_tty) { my_tty->gfx_mode = 1; my_tty->fg_pid = (int)proc->pid; }   // курсор не рисуем, Ctrl+C метит в этот PID
    sched_set_foreground_task(proc);

    // 9. Ждём завершения. Внутри: адресное пространство, ядерный стек и task_t
    //    процесса освобождаются, из планировщика он убирается.
    int exit_code = sched_wait_child(proc);

    sched_set_foreground_task(NULL);
    if (my_tty) { my_tty->gfx_mode = 0; my_tty->fg_pid = -1; }

    kputs("\n[overOS] Process finished with exit code: ", 0x00AAAAAA);
    itoa(exit_code, bbuf);
    kputs(bbuf, 0x0055FF55);
    kputs("\n", 0x00AAAAAA);
    flush_buffer();
    return exit_code;
}