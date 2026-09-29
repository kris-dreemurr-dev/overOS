#ifndef REDACTOR_H
#define REDACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../../drivers/api.h"

extern const devos_api_t* g_redactor_api;

#define EDITOR_MAX_LINES 256
#define EDITOR_MAX_COLS  128

// Цели компиляции
#define TARGET_JIT  0  // Запуск в редакторе (F5)
#define TARGET_SYS  1  // Сборка драйвера ядра Ring 0 (F9)
#define TARGET_PRG  2  // Сборка изолированной программы Ring 3 (F8)

#define PRG_LOAD_BASE 0x01000000 // 16 MB

void run_editor(const char* args);
int line_length(int row);
void insert_char(char c);
void set_compiler_target(int target);
void set_compiler_target(int target);
void set_compiler_target_sys(int is_sys);

int compile_source_to_x86(const char* src);
void run_compiled_code(void);
int get_jit_code_size(void);
extern char compile_error_msg[64];
extern uint8_t jit_buffer[16384];
extern char string_pool[4096];
extern int string_pool_idx;

extern char text_buf[EDITOR_MAX_LINES][EDITOR_MAX_COLS];

static inline void kputs(const char* str, uint32_t color) { if(g_redactor_api) g_redactor_api->kputs(str, color); }
static inline void kputc(char c, uint32_t color) { if(g_redactor_api) g_redactor_api->kputc(c, color); }
static inline void kputs_at(int x, int y, const char* str, uint32_t color) { if(g_redactor_api) g_redactor_api->kputs_at(x, y, str, color); }
static inline void draw_char(int start_x, int start_y, char c, uint32_t color) { if(g_redactor_api) g_redactor_api->draw_char(start_x, start_y, c, color); }
static inline void clear_screen(uint32_t color) { if(g_redactor_api) g_redactor_api->clear_screen(color); }
static inline void put_pixel(int x, int y, uint32_t color) { if(g_redactor_api) g_redactor_api->put_pixel(x, y, color); }
static inline uint32_t get_back_pixel(int x, int y) { return g_redactor_api ? g_redactor_api->get_back_pixel(x, y) : 0; }
static inline void flush_buffer(void) { if(g_redactor_api) g_redactor_api->flush_buffer(); }
static inline void sleep_ms(uint32_t ms) { if(g_redactor_api) g_redactor_api->sleep_ms(ms); }
static inline uint8_t inb(uint16_t port) { return g_redactor_api ? g_redactor_api->inb(port) : 0; }
static inline void outb(uint16_t port, uint8_t val) { if(g_redactor_api) g_redactor_api->outb(port, val); }
static inline void console_save_state(void) { if(g_redactor_api && g_redactor_api->console_save_state) g_redactor_api->console_save_state(); }
static inline void console_restore_state(void) { if(g_redactor_api && g_redactor_api->console_restore_state) g_redactor_api->console_restore_state(); }

#define screen_height               (g_redactor_api ? g_redactor_api->screen_height : 768)
#define screen_width                (g_redactor_api ? g_redactor_api->screen_width : 1024)
#define current_path                (g_redactor_api ? g_redactor_api->current_path : "/")

#define fs_is_mounted()          (g_redactor_api ? g_redactor_api->fs_is_mounted() : 0)
#define fs_mount(lba)            (g_redactor_api ? g_redactor_api->fs_mount(lba) : 0)
#define fs_go_root()             if(g_redactor_api) g_redactor_api->fs_go_root()
#define fs_change_dir(n)         (g_redactor_api ? g_redactor_api->fs_change_dir(n) : 0)
#define fs_make_folder(n)        (g_redactor_api ? g_redactor_api->fs_make_folder(n) : 0)
#define fs_read_file(n, b, s)    (g_redactor_api ? g_redactor_api->fs_read_file(n, b, s) : 0)
#define fs_write_file(n, b, s)   (g_redactor_api ? g_redactor_api->fs_write_file(n, b, s) : 0)
#define fs_get_dir_files(o, m)   (g_redactor_api ? g_redactor_api->fs_get_dir_files(o, m) : 0)

typedef struct {
    uint32_t saved_esp;
    uint32_t saved_ebp;
    int is_running_jit;
} jit_guard_t;

extern jit_guard_t g_jit_guard;

#endif // REDACTOR_H