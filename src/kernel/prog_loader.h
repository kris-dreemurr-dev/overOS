#ifndef PROG_LOADER_H
#define PROG_LOADER_H

#include <stdint.h>

#define PROG_LOAD_BASE 0x0000004000000000ULL

#define PRG_FLAG_CLI      0x00ULL
#define PRG_FLAG_DESKTOP  0x01ULL

typedef struct {
    char     magic[4];       // "DPRG"
    uint32_t version;        // 1
    uint64_t load_vaddr;     // 0x4000000000
    uint64_t entry_point;    // Точка входа
    uint64_t code_size;      // Байт кода
    uint64_t bss_size;       // Байт .bss
    uint64_t stack_size;     // Стек
    uint64_t flags;          // Битовые флаги подсистемы (0 = TTY, 1 = Desktop)
    uint64_t res_offset;     // Каталог ресурсов "DRES" (значок, имя, класс), 0 -- нет. См. tools/elf2prg.py
} __attribute__((packed)) devos_prg_header_t;

// Разобранный и проверенный образ программы (DPRG или плоский бинарник)
typedef struct {
    uint64_t load_base;
    uint64_t entry;
    uint64_t payload_offset;   // где в файле начинается код
    uint64_t payload_bytes;
    uint32_t image_pages;      // код + .bss
    uint32_t stack_pages;
    uint64_t stack_top;
    uint64_t flags;            // PRG_FLAG_*
} prg_image_t;

// Пользовательские адреса образа: выше identity-области ядра (0..4 ГБ в каждом процессе)
// и ниже стека. Заголовок файла — недоверенные данные: без этих проверок .prg мог
// загрузить себя поверх физической памяти ядра.
#define PRG_USER_MIN      0x0000000100000000ULL
#define PRG_STACK_TOP     0x00007FFFFFFF0000ULL
#define PRG_MAX_IMAGE     (1024ULL * 1024 * 1024)   // код + .bss
#define PRG_MAX_STACK     (64ULL * 1024 * 1024)

// 0 — образ годится; иначе код ошибки (<0)
int  prg_parse_image(const uint8_t* buf, int bytes, prg_image_t* out);
// Выделяет и мапит страницы образа и стека. 0 — успех; при ошибке вызывающий уничтожает pml4.
int  prg_map_image(uint64_t* pml4, const prg_image_t* img);

// Код возврата prog_load_module: нет права на исполнение (как 126 в bash)
#define PROG_EPERM (-126)
// Нет такого файла (как 127 в bash) / файл не годится как программа
#define PROG_ENOENT (-127)
#define PROG_ENOEXEC (-125)
#define PROG_ENOMEM  (-124)

struct task;
// Создать процесс из файла от имени uid/gid (без ожидания). NULL — ошибка, код в *err.
struct task* prog_spawn(const char* filename, const char* arg, int uid, int gid, int allow_desktop_app, int* err);

int prog_load_module(const char* filename, const char* args);
int prog_read_file_by_path(const char* path, uint8_t* buffer, uint32_t max_size);

extern int g_desktop_pid;   // PID запущенного desktop.prg (-1 — не запущен)

#endif
