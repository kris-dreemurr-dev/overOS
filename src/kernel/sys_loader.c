#include "sys_loader.h"
#include "../drivers/api.h"
#include "../fs/fat16.h"
#include "sched.h"
#include "tty.h" // Подключаем заголовки TTY
#include "../memory/pmm.h"
#include "../memory/vmm.h"   // PHYS_TO_VIRT

extern void kputs(const char* str, uint32_t color);
extern void kputc(char c, uint32_t color);
extern void kputs_at(int x, int y, const char* str, uint32_t color);
extern void draw_char(int start_x, int start_y, char c, uint32_t color);
extern void itoa(int n, char* str);
extern void clear_screen(uint32_t color);
extern void put_pixel(int x, int y, uint32_t color);
extern uint32_t get_back_pixel(int x, int y);
extern void flush_buffer(void);
extern void sleep_ms(uint32_t ms);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern uint16_t screen_width;
extern uint16_t screen_height;
extern uint16_t screen_pitch;
extern volatile uint32_t* lfb;

extern void console_save_state(void);
extern void console_restore_state(void);

extern void init_ps2_mouse(void);
extern void update_mouse_state(void);
extern void mouse_feed_byte(uint8_t byte);
extern int get_mouse_x(void);
extern int get_mouse_y(void);
extern int get_mouse_btn(void);

extern void set_term_hook(void (*hook)(const char* text));
extern void sys_exec_cmd(const char* cmd);

extern const ksym_api_t auto_ksyms[];
extern char current_path[];

static const char scancode_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  0,   ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static int g_module_tty = -1;

extern int tty_check_hotkey(uint8_t scancode);

static int sys_readline(char* buf, int max_len) {
    int idx = 0;
    while (idx < max_len - 1) {
        // Ожидаем байт в контроллере клавиатуры без вызова чужих обработчиков команд
        while (!(inb(0x64) & 1) ||
               (g_module_tty >= 0 && tty_get_active_id() != g_module_tty)) {
            sched_yield();
            __asm__ volatile("pause");
        }

        uint8_t status = inb(0x64);
        if (status & 0x20) {
            inb(0x60); // Пропускаем пакеты мыши
            continue;
        }

        uint8_t sc = inb(0x60);

        // Alt+F1 / Alt+F2 работают всегда
        if (tty_check_hotkey(sc)) {
            continue;
        }

        // Модуль принадлежит своему TTY: с чужого ввод не принимаем
        if (g_module_tty >= 0 && tty_get_active_id() != g_module_tty) {
            continue;
        }

        if (sc & 0x80) continue;

        // Безопасный перехват горячих клавиш TTY (Alt+F1 / Alt+F2)
        if (tty_check_hotkey(sc)) {
            continue; 
        }

        if (sc & 0x80) continue; // Игнорируем отпускание клавиш

        char c = (sc < 128) ? scancode_ascii[sc] : 0;
        if (!c) continue;

        if (c == '\n') {
            kputc('\n', 0xFFFFFF);
            flush_buffer();
            break;
        }

        if (c == '\b') {
            if (idx > 0) {
                idx--;
                kputc('\b', 0xFFFFFF);
                flush_buffer();
            }
            continue;
        }

        buf[idx++] = c;
        kputc(c, 0xFFFFFF);
        flush_buffer();
    }
    buf[idx] = '\0';
    return idx;
}

static void sys_format_83(const char* src, char* dst) {
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

// ============================================================================
// Загрузка перемещаемых модулей (.SYS формата SREL, см. tools/elf2sys.py)
// ============================================================================
#define SYS_MAX_MEM     (4u * 1024 * 1024)   // потолок: образ + .bss одного модуля
#define SYS_PHYS_LIMIT  0x20000000ULL        // 512 МБ: столько vmm_init мапит в higher-half

typedef struct __attribute__((packed)) {
    char     magic[4];      // "SREL"
    uint32_t image_size;    // байт образа в файле (без .bss)
    uint32_t mem_size;      // образ + .bss
    uint32_t reloc_off;     // смещение таблицы релокаций в файле
    uint32_t reloc_count;   // число uint32-смещений 64-битных ячеек
} sys_trailer_t;

typedef uint64_t __attribute__((aligned(1))) u64_unaligned;

// Единственный читатель клавиатуры для модулей.
// Байты мыши уходят обработчику мыши, Alt+F1/F2 обрабатываются ядром,
// чужому TTY порт не отдаём.
static int sys_kbd_poll(uint8_t* out) {
    if (g_module_tty >= 0 && tty_get_active_id() != g_module_tty)
        return 0;                                  // не наш TTY: порт не трогаем

    while (1) {
        // статус + данные атомарно (таймер вытесняет задачи между двумя inb)
        uint64_t fl;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
        uint8_t status = inb(0x64);
        int have = status & 0x01;
        uint8_t sc = have ? inb(0x60) : 0;
        __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");

        if (!have) return 0;

        if (status & 0x20) {                       // байт мыши — не теряем, отдаём мыши
            mouse_feed_byte(sc);
            continue;
        }

        if (tty_check_hotkey(sc)) {                // переключение TTY
            if (g_module_tty >= 0 && tty_get_active_id() != g_module_tty)
                return 0;                          // ушли с нашего TTY
            continue;
        }

        *out = sc;
        return 1;
    }
}

// Общий api: живёт вечно (резидентные драйверы хранят на него указатель)
static devos_api_t g_kernel_api;

static void sys_api_fill(void) {
    devos_api_t* a = &g_kernel_api;
    a->kputs = kputs;               a->kputc = kputc;
    a->kputs_at = kputs_at;         a->draw_char = draw_char;
    a->itoa = itoa;                 a->clear_screen = clear_screen;
    a->put_pixel = put_pixel;       a->get_back_pixel = get_back_pixel;
    a->flush_buffer = flush_buffer; a->sleep_ms = sleep_ms;
    a->console_save_state = console_save_state;
    a->console_restore_state = console_restore_state;
    a->inb = inb;                   a->outb = outb;
    a->readline = sys_readline;
    a->init_ps2_mouse = init_ps2_mouse;
    a->update_mouse_state = update_mouse_state;
    a->get_mouse_x = get_mouse_x;   a->get_mouse_y = get_mouse_y;
    a->get_mouse_btn = get_mouse_btn;
    a->set_term_hook = set_term_hook;
    a->exec_cmd = sys_exec_cmd;
    a->screen_width = screen_width; a->screen_height = screen_height;
    a->screen_pitch = screen_pitch; a->lfb = lfb;
    a->fat16_is_mounted = fat16_is_mounted;
    a->fat16_mount = fat16_mount;
    a->fat16_go_root = fat16_go_root;
    a->fat16_change_dir = fat16_change_dir;
    a->fat16_make_folder = fat16_make_folder;
    a->fat16_read_file = fat16_read_file;
    a->fat16_write_file = fat16_write_file;
    a->fat16_get_dir_files = (int (*)(void*, int))fat16_get_dir_files;
    a->kernel_symbols = auto_ksyms;
    a->current_path = current_path;
    a->kbd_poll = sys_kbd_poll;
}

// FAT16 и current_path общие: читаем файл под коротким замком
static volatile int g_sys_load_lock = 0;
static void sys_load_lock(void)   { while (__sync_lock_test_and_set(&g_sys_load_lock, 1)) sched_yield(); }
static void sys_load_unlock(void) { __sync_lock_release(&g_sys_load_lock); }

static void sys_free_pages(uint64_t phys, uint32_t first_page, uint32_t end_page) {
    for (uint32_t i = first_page; i < end_page; i++)
        pmm_free_page((void*)(phys + (uint64_t)i * PAGE_SIZE));
}

static int sys_load_fail(uint64_t phys, const char* msg) {
    sys_free_pages(phys, 0, SYS_MAX_MEM / PAGE_SIZE);
    kputs(msg, 0xFF5555);
    return 0;
}

int sys_load_module(const char* filename, const char* args) {
    char name83[11];
    sys_format_83(filename, name83);

    // 1. Временно берём максимум непрерывной физической памяти и читаем файл прямо в неё
    const uint32_t max_pages = SYS_MAX_MEM / PAGE_SIZE;

    sys_load_lock();
    uint64_t phys = (uint64_t)pmm_alloc_pages(max_pages);
    if (!phys) {
        sys_load_unlock();
        kputs("[-] Module error: out of physical memory\n", 0xFF5555);
        return 0;
    }
    if (phys + SYS_MAX_MEM > SYS_PHYS_LIMIT) {   // higher-half мапит только первые 512 МБ
        sys_load_unlock();
        return sys_load_fail(phys, "[-] Module error: memory above 512 MB is not mapped\n");
    }

    uint8_t* base = (uint8_t*)PHYS_TO_VIRT(phys);
    fat16_go_root();
    int bytes = fat16_read_file(name83, base, SYS_MAX_MEM);
    sys_load_unlock();

    if (bytes <= 0) {
        kputs("[-] FAT16 read error: file not found [", 0xFF5555);
        for (int i = 0; i < 11; i++) kputc(name83[i], 0xFFFF55);
        kputs("]\n", 0xFF5555);
        sys_free_pages(phys, 0, max_pages);
        return 0;
    }

    // 2. Трейлер и проверка формата
    if (bytes < (int)(sizeof(sys_header_t) + sizeof(sys_trailer_t)))
        return sys_load_fail(phys, "[-] Module error: file too small\n");

    const sys_trailer_t* t = (const sys_trailer_t*)(base + bytes - sizeof(sys_trailer_t));
    if (t->magic[0] != 'S' || t->magic[1] != 'R' || t->magic[2] != 'E' || t->magic[3] != 'L')
        return sys_load_fail(phys, "[-] Module error: no SREL trailer (rebuild the module with elf2sys.py)\n");

    // Копируем поля: при обнулении .bss трейлер и таблица могут быть затёрты
    uint32_t image_size = t->image_size, mem_size = t->mem_size;
    uint32_t reloc_off  = t->reloc_off,  reloc_cnt = t->reloc_count;
    uint32_t data_end   = (uint32_t)bytes - (uint32_t)sizeof(sys_trailer_t);

    if (image_size < sizeof(sys_header_t) || image_size > data_end ||
        mem_size < image_size || mem_size > SYS_MAX_MEM ||
        reloc_off < image_size || (uint64_t)reloc_off + (uint64_t)reloc_cnt * 4 > data_end)
        return sys_load_fail(phys, "[-] Module error: corrupt SREL trailer\n");

    // 3. Релокации: к каждой 64-битной ячейке прибавляем реальный адрес загрузки
    const uint32_t* rel = (const uint32_t*)(base + reloc_off);
    for (uint32_t i = 0; i < reloc_cnt; i++) {
        uint32_t off = rel[i];
        if ((uint64_t)off + 8 > image_size)
            return sys_load_fail(phys, "[-] Module error: bad relocation\n");
        *(u64_unaligned*)(base + off) += (uint64_t)base;
    }

    // 4. Зануляем .bss (после релокаций: таблица лежит в этой же области)
    for (uint32_t i = image_size; i < mem_size; i++) base[i] = 0;

    // 5. Оставляем только нужные страницы
    uint32_t need_pages = (mem_size + PAGE_SIZE - 1) / PAGE_SIZE;
    sys_free_pages(phys, need_pages, max_pages);

    sys_header_t* hdr = (sys_header_t*)base;
    if (hdr->magic[0] != SYS_MAGIC_0 || hdr->magic[1] != SYS_MAGIC_1 ||
        hdr->magic[2] != SYS_MAGIC_2 || hdr->magic[3] != SYS_MAGIC_3) {
        kputs("[-] Module magic mismatch! Read: ", 0xFF5555);
        for (int i = 0; i < 4; i++) kputc(hdr->magic[i], 0xFFFF55);
        kputs("\n", 0xFFFFFF);
        sys_free_pages(phys, 0, need_pages);
        return 0;
    }

    uint64_t entry = (uint64_t)hdr->entry_point;   // после релокации — абсолютный
    if (entry < (uint64_t)base || entry >= (uint64_t)base + image_size) {
        kputs("[-] Module error: entry point out of bounds!\n", 0xFF5555);
        sys_free_pages(phys, 0, need_pages);
        return 0;
    }

    // 6. Запуск
    sys_api_fill();

    int prev_tty = g_module_tty;
    g_module_tty = tty_get_active_id();

    int status = ((int (*)(const devos_api_t*, const char*))entry)(&g_kernel_api, args ? args : "");
    (void)status;

    g_module_tty = prev_tty;

    // 7. Приложение уходит — память возвращаем. Резидентный драйвер остаётся навсегда.
    if (!(hdr->flags & SYS_FLAG_RESIDENT))
        sys_free_pages(phys, 0, need_pages);

    return 1;
}