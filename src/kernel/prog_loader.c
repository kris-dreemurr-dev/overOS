#include "prog_loader.h"
#include "user_mode.h"
#include "sched.h"
#include "tty.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include <stdint.h>

extern void kputs(const char* str, uint32_t color);
extern void flush_buffer(void);
extern int  fs_read_file(const char* filename, uint8_t* buffer, uint32_t max_size);
extern int  fs_change_dir(const char* dir);
extern int  fs_go_root(void);
extern void fs_lock(void);
extern void fs_unlock(void);

uint8_t kernel_temp_buf[1024 * 1024 * 4] __attribute__((aligned(4096)));

// Проверка: является ли бинарник самой графической оболочкой
static int is_desktop_binary(const char* fn) {
    if (!fn) return 0;
    const char* base = fn;
    for (int i = 0; fn[i]; i++) {
        if (fn[i] == '/') base = &fn[i + 1];
    }
    const char* target = "desktop.prg";
    int i = 0;
    while (base[i] && target[i]) {
        char c1 = base[i];
        char c2 = target[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        i++;
    }
    return (base[i] == '\0' && target[i] == '\0');
}

int prog_load_module(const char* filename, const char* args) {
    (void)args;
    fs_lock();

    // 1. Читаем бинарник в буфер
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

    // 2. Анализ заголовка DPRG
    devos_prg_header_t* hdr = (devos_prg_header_t*)kernel_temp_buf;
    uint64_t load_base = PROG_LOAD_BASE;
    uint64_t entry_vaddr = PROG_LOAD_BASE;
    uint64_t payload_offset = 0;
    uint64_t payload_bytes = (uint64_t)bytes;
    uint64_t total_image_bytes = payload_bytes;
    uint32_t stack_size = 512 * 1024;
    int is_desktop_app = 0;

    if (hdr->magic[0] == 'D' && hdr->magic[1] == 'P' && hdr->magic[2] == 'R' && hdr->magic[3] == 'G') {
        load_base = hdr->load_vaddr ? hdr->load_vaddr : PROG_LOAD_BASE;
        entry_vaddr = hdr->entry_point ? hdr->entry_point : load_base;
        payload_offset = sizeof(devos_prg_header_t);
        payload_bytes = hdr->code_size;
        total_image_bytes = hdr->code_size + hdr->bss_size;
        if (hdr->stack_size) stack_size = (uint32_t)hdr->stack_size;
        if (hdr->flags & PRG_FLAG_DESKTOP) is_desktop_app = 1;
    }

    // БЛОКИРОВКА ГРАФИЧЕСКИХ ПРОГРАММ В TTY
    if (is_desktop_app && !is_desktop_binary(filename)) {
        kputs("\n[!] .PRG file dont support tty\n\n", 0x00FF5555);
        flush_buffer();
        fs_unlock();
        return -1;
    }

    // Для TTY консольных программ выводим инфо, для оболочки Desktop — молча запускаем
    if (!is_desktop_binary(filename)) {
        kputs("[overOS] Loading program ", 0x00AAAAAA);
        kputs(filename, 0x00FFFFFF);
        kputs("...\n", 0x00AAAAAA);
        flush_buffer();
    }

    // 3. Выделение страниц процесса
    uint64_t original_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(original_cr3)); 

    uint64_t* proc_pml4 = vmm_create_address_space(); 
    if (!proc_pml4) {
        fs_unlock();
        return 0; 
    }

    uint32_t num_image_pages = (total_image_bytes + 4095) / 4096;
    if (num_image_pages == 0) num_image_pages = 1;

    for (uint32_t i = 0; i < num_image_pages; i++) {
        uint64_t v_addr = load_base + (i * 4096); 
        void* p_addr = pmm_alloc_page(); 
        if (!p_addr) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(original_cr3) : "memory"); 
            vmm_destroy_address_space(proc_pml4); 
            fs_unlock();
            return 0; 
        }
        vmm_map_page(proc_pml4, v_addr, (uint64_t)p_addr, VMM_FLAG_USER | VMM_FLAG_WRITABLE); 
    }

    uint32_t num_stack_pages = (stack_size + 4095) / 4096;
    uint64_t stack_top = 0x00007FFFFFFF0000ULL; 
    uint64_t stack_base = stack_top - (num_stack_pages * 4096); 

    for (uint32_t i = 0; i < num_stack_pages; i++) {
        void* p_addr = pmm_alloc_page(); 
        if (p_addr) {
            vmm_map_page(proc_pml4, stack_base + (i * 4096), (uint64_t)p_addr, VMM_FLAG_USER | VMM_FLAG_WRITABLE); 
        }
    }

    // 4. Копирование образа
    uint64_t cp_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(cp_flags) :: "memory");
    vmm_switch_directory(proc_pml4); 

    uint8_t* target = (uint8_t*)load_base;
    uint8_t* src = kernel_temp_buf + payload_offset;

    for (uint64_t i = 0; i < payload_bytes; i++) target[i] = src[i];
    for (uint64_t i = payload_bytes; i < (uint64_t)(num_image_pages * 4096); i++) target[i] = 0;

    __asm__ volatile("mov %0, %%cr3" :: "r"(original_cr3) : "memory"); 
    fs_unlock();
    __asm__ volatile("pushq %0; popfq" :: "r"(cp_flags) : "memory", "cc");

    // 5. Запуск
    task_t* shell = sched_get_current_task();
    tty_t*  my_tty = (shell && shell->tty_id >= 0) ? tty_get(shell->tty_id) : NULL;

    uint64_t total_mem  = (uint64_t)(num_image_pages + num_stack_pages) * 4096;
    uint64_t heap_start = load_base + (uint64_t)num_image_pages * 4096;

    task_t* proc = sched_spawn_process(filename, (uint64_t)proc_pml4, total_mem,
                                       entry_vaddr, stack_top, heap_start);
    if (!proc) {
        vmm_destroy_address_space(proc_pml4);
        return -1;
    }

    if (my_tty) { 
        if (is_desktop_binary(filename)) my_tty->gfx_mode = 1; 
        my_tty->fg_pid = (int)proc->pid; 
    }
    sched_set_foreground_task(proc);

    int exit_code = sched_wait_child(proc);

    sched_set_foreground_task(NULL);
    if (my_tty) { my_tty->gfx_mode = 0; my_tty->fg_pid = -1; }

    return exit_code;
}