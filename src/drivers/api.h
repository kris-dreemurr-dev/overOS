#ifndef DRIVER_API_H
#define DRIVER_API_H

#include <stdint.h>

#define SYS_MAGIC_0 'D'
#define SYS_MAGIC_1 'S'
#define SYS_MAGIC_2 'Y'
#define SYS_MAGIC_3 'S'

#define SYS_LOAD_BASE 0x02000000   /* устарело: модули больше не привязаны к адресу */

/* sys_header_t.flags */
#define SYS_FLAG_RESIDENT 0x00000001  /* драйвер: после driver_entry остаётся в памяти */

typedef struct devos_api devos_api_t;
typedef int (*driver_entry_t)(const devos_api_t* api);

typedef struct {
    char name83[11];
    uint8_t attr;
    uint32_t size;
    uint16_t cluster;
    char clean_name[16];
} __attribute__((packed)) fat16_file_info_api_t;

typedef struct {
    const char* name;
    uint32_t addr;
} ksym_api_t;

struct devos_api {
    // Вывод текста и графика
    void (*kputs)(const char* str, uint32_t color);
    void (*kputc)(char c, uint32_t color);
    void (*kputs_at)(int x, int y, const char* str, uint32_t color);
    void (*draw_char)(int start_x, int start_y, char c, uint32_t color);
    void (*itoa)(int n, char* str);
    void (*clear_screen)(uint32_t color);
    void (*put_pixel)(int x, int y, uint32_t color);
    uint32_t (*get_back_pixel)(int x, int y);
    void (*flush_buffer)(void);
    void (*sleep_ms)(uint32_t ms);

    // Управление состоянием экрана и консоли
    void (*console_save_state)(void);
    void (*console_restore_state)(void);

    // Порты ввода-вывода
    uint8_t (*inb)(uint16_t port);
    void (*outb)(uint16_t port, uint8_t val);

    // Ввод строк
    int (*readline)(char* buf, int max);

    // Мышь PS/2
    void (*init_ps2_mouse)(void);
    void (*update_mouse_state)(void);
    int (*get_mouse_x)(void);
    int (*get_mouse_y)(void);
    int (*get_mouse_btn)(void);

    // Интеграция с шеллом ядра
    void (*set_term_hook)(void (*hook)(const char* text));
    void (*exec_cmd)(const char* cmd);

    // Параметры экрана и фреймбуфер
    uint16_t screen_width;
    uint16_t screen_height;
    uint16_t screen_pitch;
    volatile uint32_t* lfb;

    // Интерфейс файловой системы FAT16 для редактора
    int (*fat16_is_mounted)(void);
    int (*fat16_mount)(int partition_lba);
    void (*fat16_go_root)(void);
    int (*fat16_change_dir)(const char* name83);
    int (*fat16_make_folder)(const char* name83);
    int (*fat16_read_file)(const char* name83, void* buffer, uint32_t max_bytes);
    int (*fat16_write_file)(const char* name83, const void* data, uint32_t size);
    int (*fat16_get_dir_files)(void* out_files, int max_files);

    // Таблица экспортируемых символов ядра для JIT
    const ksym_api_t* kernel_symbols;
    char* current_path;

    // Единый вход клавиатуры для модулей: 1 = сканкод получен, 0 = нет
    // (порт 0x60/0x64 модули напрямую больше не читают)
    int (*kbd_poll)(uint8_t* scancode);
};

typedef struct {
    char           magic[4];
    driver_entry_t entry_point;
    uint32_t       required_api_ver;
    char           driver_name[16];
    uint32_t       flags;
} __attribute__((packed)) sys_header_t;

#endif