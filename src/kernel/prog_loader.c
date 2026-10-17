#include "prog_loader.h"
#include "user_mode.h"
#include "sched.h"
#include "tty.h"
#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "users.h"
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
int g_desktop_pid = -1;      // PID композитора: его не убивают Ctrl+Alt+C

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

int prg_parse_image(const uint8_t* buf, int bytes, prg_image_t* out) {
    if (bytes <= 0) return -1;
    const devos_prg_header_t* hdr = (const devos_prg_header_t*)buf;
    uint64_t stack_size = 512 * 1024;
    uint64_t image_bytes;

    out->flags = 0;
    if (bytes >= (int)sizeof(devos_prg_header_t) &&
        hdr->magic[0] == 'D' && hdr->magic[1] == 'P' && hdr->magic[2] == 'R' && hdr->magic[3] == 'G') {
        out->load_base      = hdr->load_vaddr ? hdr->load_vaddr : PROG_LOAD_BASE;
        out->payload_offset = sizeof(devos_prg_header_t);
        out->payload_bytes  = hdr->code_size;
        out->entry          = hdr->entry_point ? hdr->entry_point : out->load_base;
        out->flags          = hdr->flags;
        if (hdr->stack_size) stack_size = hdr->stack_size;
        if (out->payload_bytes > (uint64_t)bytes - sizeof(devos_prg_header_t)) return -2;  // код длиннее файла
        if (hdr->bss_size > PRG_MAX_IMAGE) return -3;
        image_bytes = out->payload_bytes + hdr->bss_size;
    } else {
        // Плоский бинарник: весь файл — код с PROG_LOAD_BASE
        out->load_base      = PROG_LOAD_BASE;
        out->payload_offset = 0;
        out->payload_bytes  = (uint64_t)bytes;
        out->entry          = PROG_LOAD_BASE;
        image_bytes         = (uint64_t)bytes;
    }

    if (image_bytes > PRG_MAX_IMAGE) return -3;
    if (stack_size < 4096 || stack_size > PRG_MAX_STACK) return -4;
    if (out->load_base & 0xFFF) return -5;

    out->image_pages = (uint32_t)((image_bytes + 4095) / 4096);
    if (out->image_pages == 0) out->image_pages = 1;
    out->stack_pages = (uint32_t)((stack_size + 4095) / 4096);
    out->stack_top   = PRG_STACK_TOP;

    uint64_t image_end   = out->load_base + (uint64_t)out->image_pages * 4096;
    uint64_t stack_floor = out->stack_top - (uint64_t)out->stack_pages * 4096 - 4096;   // + защитная страница
    if (out->load_base < PRG_USER_MIN || image_end > stack_floor || image_end < out->load_base) return -6;
    if (out->entry < out->load_base || out->entry >= out->load_base + out->payload_bytes) return -7;
    return 0;
}

int prg_map_image(uint64_t* pml4, const prg_image_t* img) {
    for (uint32_t i = 0; i < img->image_pages; i++) {
        void* p = pmm_alloc_page();
        if (!p) return -1;
        if (!vmm_map_page(pml4, img->load_base + (uint64_t)i * 4096, (uint64_t)p, VMM_FLAG_USER | VMM_FLAG_WRITABLE)) {
            pmm_free_page(p);
            return -1;
        }
    }
    uint64_t stack_base = img->stack_top - (uint64_t)img->stack_pages * 4096;
    for (uint32_t i = 0; i < img->stack_pages; i++) {
        void* p = pmm_alloc_page();
        if (!p) return -1;
        if (!vmm_map_page(pml4, stack_base + (uint64_t)i * 4096, (uint64_t)p, VMM_FLAG_USER | VMM_FLAG_WRITABLE)) {
            pmm_free_page(p);
            return -1;
        }
    }
    return 0;
}

// Создаёт процесс из файла (без ожидания): общий путь для оболочки и sys_spawn.
// uid/gid — от чьего имени запускать (право X проверяется для них). *err — PROG_E* при неудаче.
task_t* prog_spawn(const char* filename, const char* arg, int uid, int gid, int allow_desktop_app, int* err) {
    *err = 0;
    // Относительные пути — от каталога того, кто запускает (оболочка Ring 3 или TTY)
    extern const char* fs_caller_cwd(void);
    const char* base = fs_caller_cwd();
    if (!perm_check(uid, gid, filename, base, PERM_X)) { *err = PROG_EPERM; return NULL; }

    fs_lock();

    // 1. Читаем бинарник в буфер (kernel_temp_buf общий — fs_lock держим до копирования)
    int bytes = fs_read_file(filename, kernel_temp_buf, sizeof(kernel_temp_buf));
    if (bytes <= 0) { fs_unlock(); *err = PROG_ENOENT; return NULL; }

    // 2. Анализ и проверка заголовка (недоверенные данные из файла)
    prg_image_t img;
    if (prg_parse_image(kernel_temp_buf, bytes, &img) != 0) { fs_unlock(); *err = PROG_ENOEXEC; return NULL; }
    if ((img.flags & PRG_FLAG_DESKTOP) && !allow_desktop_app) { fs_unlock(); *err = PROG_ENOEXEC; return NULL; }

    // 3. Адресное пространство и страницы
    uint64_t* proc_pml4 = vmm_create_address_space();
    if (!proc_pml4) { fs_unlock(); *err = PROG_ENOMEM; return NULL; }
    if (prg_map_image(proc_pml4, &img) != 0) {
        vmm_destroy_address_space(proc_pml4);
        fs_unlock();
        *err = PROG_ENOMEM;
        return NULL;
    }

    // 4. Копирование образа
    uint64_t original_cr3;
    uint64_t cp_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(cp_flags) :: "memory");
    __asm__ volatile("mov %%cr3, %0" : "=r"(original_cr3));
    vmm_switch_directory(proc_pml4);

    uint8_t* target = (uint8_t*)img.load_base;
    uint8_t* src = kernel_temp_buf + img.payload_offset;
    for (uint64_t i = 0; i < img.payload_bytes; i++) target[i] = src[i];
    for (uint64_t i = img.payload_bytes; i < (uint64_t)img.image_pages * 4096; i++) target[i] = 0;

    __asm__ volatile("mov %0, %%cr3" :: "r"(original_cr3) : "memory");
    fs_unlock();
    __asm__ volatile("pushq %0; popfq" :: "r"(cp_flags) : "memory", "cc");

    // 5. Процесс
    uint64_t total_mem  = (uint64_t)(img.image_pages + img.stack_pages) * 4096;
    uint64_t heap_start = img.load_base + (uint64_t)img.image_pages * 4096;
    // Имя процесса — только имя файла, без пути: в ps/диспетчере задач поле
    // короткое (16 байт), и «/desprogs/ovfiles.prg» обрезалось до «/desprogs/ovfil»
    const char* base_name = filename;
    for (const char* p = filename; *p; p++) if (*p == '/' || *p == '\\') base_name = p + 1;

    task_t* proc = sched_spawn_process(base_name, (uint64_t)proc_pml4, total_mem,
                                       img.entry, img.stack_top, heap_start);
    if (!proc) {
        vmm_destroy_address_space(proc_pml4);
        *err = PROG_ENOMEM;
        return NULL;
    }
    proc->uid = uid;
    proc->gid = gid;
    {
        task_t* me = sched_get_current_task();
        sched_set_exe(proc, filename, me ? me->cwd : "/");
    }
    int i = 0;
    if (arg) while (arg[i] && i < (int)sizeof(proc->arg) - 1) { proc->arg[i] = arg[i]; i++; }
    proc->arg[i] = '\0';

    // Текущий каталог процесса = каталог того, кто его запустил
    const char* cwd = base;
    int c = 0;
    while (cwd[c] && c < (int)sizeof(proc->cwd) - 1) { proc->cwd[c] = cwd[c]; c++; }
    proc->cwd[c] = '\0';
    return proc;
}

int prog_load_module(const char* filename, const char* args) {
    // Графические программы (флаг DESKTOP) из консоли не запускаются — кроме самого desktop.prg
    int err;
    task_t* proc = prog_spawn(filename, args, cred_uid(), cred_gid(), is_desktop_binary(filename), &err);
    if (!proc) {
        if (err == PROG_EPERM) {
            kputs("[!] Permission denied: ", 0x00FF5555);
            kputs(filename, 0x00FFFFFF);
            kputs("\n", 0x00FF5555);
        } else if (err == PROG_ENOEXEC) {
            kputs("[!] Not a console program, or invalid/unsafe executable image.\n", 0x00FF5555);
        } else if (err == PROG_ENOMEM) {
            kputs("[!] Out of memory while loading program.\n", 0x00FF5555);
        }
        flush_buffer();
        return err;
    }

    // Запуск на переднем плане TTY: ему клавиатура, оболочка ждёт завершения
    task_t* shell = sched_get_current_task();
    tty_t*  my_tty = (shell && shell->tty_id >= 0) ? tty_get(shell->tty_id) : NULL;
    if (my_tty) {
        if (is_desktop_binary(filename)) { my_tty->gfx_mode = 1; g_desktop_pid = (int)proc->pid; }
        my_tty->fg_pid = (int)proc->pid;
    }
    sched_set_foreground_task(proc);

    // Теперь, когда передний план и графический режим TTY проставлены, процесс
    // можно пускать: до этого момента он ещё не бежал (см. sched_spawn_process).
    sched_start_process(proc);

    int exit_code = sched_wait_child(proc);

    sched_set_foreground_task(NULL);
    if (g_desktop_pid == (int)proc->pid) g_desktop_pid = -1;
    if (my_tty) { my_tty->gfx_mode = 0; my_tty->fg_pid = -1; }

    return exit_code;
}
