#ifndef LIB_LOADER_H
#define LIB_LOADER_H

#include <stdint.h>

// =============================================================================
// .ll — разделяемые библиотеки overOS (linked library), аналог .so и .dll.
//
// Файл собирается tools/elf2ll.py из перемещаемого ELF (PIE, база 0) и несёт
// образ, таблицу релокаций и таблицу экспорта (имя -> смещение). Ядро грузит
// библиотеку в адресное пространство вызывающего процесса по свободному адресу
// в области LIB_BASE, прибавляет базу к ячейкам из таблицы релокаций и отдаёт
// программе дескриптор. Дальше программа берёт адрес символа по имени и зовёт
// его как обычную функцию -- связывания на этапе линковки нет, всё в runtime
// (как dlopen/dlsym, а не как динамический линковщик).
//
// Импорта у библиотеки нет: всё, что ей нужно от системы, она делает сама через
// int 0x80. Это снимает вопрос разрешения символов между модулями.
// =============================================================================

#define LL_MAGIC0 'D'
#define LL_MAGIC1 'L'
#define LL_MAGIC2 'I'
#define LL_MAGIC3 'B'

#define LIB_BASE      0x0000600000000000ULL   // область библиотек процесса
// Слот под одну библиотеку. Виртуальных адресов в 64 битах не жалко (слоты идут
// подряд от LIB_BASE, 16 слотов по 256 МБ — это 4 ГБ адресов на процесс), а
// физическая память выделяется ровно под образ и bss библиотеки. Пределы подняты
// под полноценный FFmpeg: с HEVC, VP9 и фильтрами и образ, и таблицы крупнее.
#define LIB_SLOT_SIZE (256ULL * 1024 * 1024)  // один слот на библиотеку
#define LIB_MAX_SLOTS 16                      // столько .ll можно держать открытыми
#define LIB_MAX_IMAGE (48ULL * 1024 * 1024)   // предел размера образа (кода и данных) в файле
// Предел памяти библиотеки вместе с .bss: образ, кучи и буферы опорных кадров.
#define LIB_MAX_MEM   (240ULL * 1024 * 1024)

typedef struct {
    char     magic[4];       // "DLIB"
    uint32_t version;        // 1
    uint32_t image_off;      // смещение образа в файле
    uint32_t reloc_count;
    uint64_t image_size;     // байт образа в файле (.text/.rodata/.data)
    uint64_t mem_size;       // образ + .bss
    uint64_t reloc_off;      // таблица uint32-смещений
    uint64_t sym_off;        // таблица ll_sym_t
    uint64_t sym_count;
    uint64_t str_off;        // строки имён
    uint64_t str_size;
    uint64_t reserved;
} ll_header_t;

#define LL_SYM_FUNC  1
#define LL_SYM_DATA  2
#define LL_SYM_CONST 3

typedef struct {
    uint32_t name_off;       // смещение имени в таблице строк
    uint32_t flags;          // LL_SYM_*
    uint64_t value;          // смещение символа в образе
} ll_sym_t;

// Сисколлы 119/120/121. Возврат: дескриптор >= 0 либо отрицательный код.
#define LIB_ENOENT   -1     // файл не найден или не читается
#define LIB_ENOEXEC  -2     // это не .ll или заголовок не сходится
#define LIB_ENOMEM   -3     // нет памяти или свободного слота
#define LIB_EPERM    -4     // нет права X на файл

int      lib_open(const char* path, int uid, int gid);
uint64_t lib_sym(int handle, const char* name);
int      lib_close(int handle);
void     lib_fork(uint64_t parent_pid, uint64_t child_pid);   // ребёнок наследует образы
void     lib_release_pid(uint64_t pid);  // процесс завершился: отпустить его слоты

#endif
