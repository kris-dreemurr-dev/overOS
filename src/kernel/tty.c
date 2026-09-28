#include "tty.h"
#include "sched.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"

extern uint16_t  screen_width;
extern uint16_t  screen_height;
extern uint16_t  screen_pitch;
extern volatile  uint32_t* lfb;
extern uint32_t  back_buffer[];
extern uint32_t* current_draw_buffer;
extern int       cursor_x, cursor_y, prompt_min_x;
extern int       total_scrolled_px, scroll_offset_px;
extern uint32_t  current_bg_color;
extern char      input_buffer[];
extern int       input_len;
extern void      keyboard_poll_handler(void);

extern void clear_screen(uint32_t color);
extern void print_prompt(void);
extern void flush_buffer(void);
extern void kputs(const char* str, uint32_t color);
extern void itoa(int n, char* str);

static tty_t g_ttys[MAX_TTYS];
static int   g_active_tty = 0;

static void tty2_task_entry(void) {
    while (1) {
        sched_yield();
    }
}

void tty_init_core(void) {
    // TTY 1 привязан к статическому back_buffer ядра
    g_ttys[0].id = 0;
    g_ttys[0].active = 1;
    g_ttys[0].buffer = back_buffer;
    g_ttys[0].cursor_x = 0;
    g_ttys[0].cursor_y = 0;
    g_ttys[0].prompt_min_x = 0;
    g_ttys[0].total_scrolled_px = 0;
    g_ttys[0].scroll_offset_px = 0;
    g_ttys[0].input_len = 0;
    g_ttys[0].bg_color = 0x000000;
    g_ttys[0].gfx_mode = 0;

    // TTY 2 не выделен до нажатия Alt+F2
    g_ttys[1].id = 1;
    g_ttys[1].active = 0;
    g_ttys[1].buffer = NULL;

    g_active_tty = 0;
    current_draw_buffer = back_buffer;
}

int tty_get_active_id(void) {
    return g_active_tty;
}

tty_t* tty_get(int id) {
    if (id < 0 || id >= MAX_TTYS) return NULL;
    return &g_ttys[id];
}

static void tty_shell_loop(void);   // объявление, если функция определена ниже

// Создаёт TTY 2: видеобуфер, состояние терминала и задачу с оболочкой.
// Возвращает 0 при успехе, -1 при нехватке памяти.
static int tty_spawn_second(void) {
    size_t px_count = (size_t)screen_width * screen_height;
    size_t fb_size  = px_count * sizeof(uint32_t);
    size_t pages    = (fb_size + 4095) / 4096;

    // Выделяем непрерывный видеобуфер под TTY 2
    void* phys = pmm_alloc_pages(pages);
    if (!phys) return -1;
    g_ttys[1].buffer = (uint32_t*)PHYS_TO_VIRT(phys);

    // Зачищаем буфер терминала
    for (size_t i = 0; i < px_count; i++) {
        g_ttys[1].buffer[i] = 0x000000;
    }

    g_ttys[1].cursor_x = 0;
    g_ttys[1].cursor_y = 0;
    g_ttys[1].prompt_min_x = 0;
    g_ttys[1].total_scrolled_px = 0;
    g_ttys[1].scroll_offset_px = 0;
    g_ttys[1].input_len = 0;
    g_ttys[1].bg_color = 0x000000;
    g_ttys[1].gfx_mode = 0;
    g_ttys[1].active = 1;

    // Оболочка TTY 2: общий цикл, привязка к TTY через tty_id
    task_t* t = task_create_kernel(tty_shell_loop, "tty2_shell");
    t->tty_id = 1;

    return 0;
}

void tty_switch(int target_id) {
    if (target_id == g_active_tty || target_id < 0 || target_id >= MAX_TTYS) return;

    // 1. Создаём TTY 2 при первом обращении (Alt+F2).
    //    Делаем это до критической секции: выделение и очистка буфера занимают время.
    int is_first_open = 0;
    if (target_id == 1 && !g_ttys[1].active) {
        if (tty_spawn_second() != 0) return;   // нет памяти — остаёмся на текущем TTY
        is_first_open = 1;
    }

    // Дальше меняем глобальное состояние терминала: не даём планировщику
    // вклиниться посередине, иначе другая задача нарисует в полусохранённый TTY
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    tty_t* cur = &g_ttys[g_active_tty];
    tty_t* dst = &g_ttys[target_id];

    // 2. Сохраняем состояние текущей консоли
    cur->cursor_x = cursor_x;
    cur->cursor_y = cursor_y;
    cur->prompt_min_x = prompt_min_x;
    cur->total_scrolled_px = total_scrolled_px;
    cur->scroll_offset_px = scroll_offset_px;
    cur->bg_color = current_bg_color;

    int save_len = input_len;
    if (save_len < 0) save_len = 0;
    if (save_len > (int)sizeof(cur->input_buf) - 1) save_len = (int)sizeof(cur->input_buf) - 1;
    cur->input_len = save_len;
    for (int i = 0; i < save_len; i++) {
        cur->input_buf[i] = input_buffer[i];
    }

    // 3. Переключаем рабочий буфер отрисовки
    g_active_tty = target_id;
    current_draw_buffer = dst->buffer;

    // 4. Восстанавливаем состояние целевой консоли
    cursor_x = dst->cursor_x;
    cursor_y = dst->cursor_y;
    prompt_min_x = dst->prompt_min_x;
    total_scrolled_px = dst->total_scrolled_px;
    scroll_offset_px = dst->scroll_offset_px;
    current_bg_color = dst->bg_color;

    int load_len = dst->input_len;
    if (load_len < 0) load_len = 0;
    if (load_len > (int)sizeof(dst->input_buf) - 1) load_len = (int)sizeof(dst->input_buf) - 1;
    input_len = load_len;
    for (int i = 0; i < load_len; i++) {
        input_buffer[i] = dst->input_buf[i];
    }
    input_buffer[load_len] = '\0';

    // 5. TTY 2 открыт впервые — баннер и приглашение (рисуются уже в его буфер)
    if (is_first_open) {
        print_tty_banner(1);   // devOS x86_64 0.0.1 tty patch / tty2
        print_prompt();
    }

    // 6. Показываем экран целевого TTY
    flush_buffer();

    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static void tty_shell_loop(void) {
    while (1) {
        keyboard_poll_handler();   // сама проверяет, что наш TTY активен
        sched_yield();
    }
}

// TTY 1 стартует в boot-цепочке: рисует свой экран и переходит в общий цикл
void tty1_task_entry(void) {
    clear_screen(0x000000);
    print_tty_banner(0);
    print_prompt();
    flush_buffer();
    tty_shell_loop();
}

static int g_alt_state = 0;

// Возвращает 1, если сканкод был хоткеем TTY (и его не надо передавать приложению)
int tty_check_hotkey(uint8_t scancode) {
    if (scancode == 0x38) { // Нажат Alt
        g_alt_state = 1;
        return 0; // Не поглощаем, чтобы клавиатура знала про Alt
    }
    if (scancode == 0xB8) { // Отпущен Alt
        int was_combo = (g_alt_state == 2);
        g_alt_state = 0;
        return was_combo ? 1 : 0;
    }

    if (g_alt_state) {
        if (scancode == 0x3B) { // F1 -> TTY 1
            g_alt_state = 2;
            tty_switch(0);
            return 1; // Поглощаем клавишу!
        }
        if (scancode == 0x3C) { // F2 -> TTY 2
            g_alt_state = 2;
            tty_switch(1);
            return 1; // Поглощаем клавишу!
        }
    }
    return 0;
}