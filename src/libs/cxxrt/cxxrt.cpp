// =============================================================================
// cxxrt.cpp — куча, operator new/delete, глобальные конструкторы, mem*.
//
// В программе куча растёт через brk (ov_sbrk) кусками не меньше 256 КБ.
// Свободные блоки лежат в списке по возрастанию адреса и при освобождении
// сливаются с соседями, поэтому кадр за кадром выделять и освобождать можно
// без утечки адресного пространства. Программа на C++ не должна загружать
// libc.ll: у процесса одна граница brk, и две кучи на ней испортят друг друга.
//
// В библиотеке .ll граница brk чужая (она принадлежит программе), поэтому
// там рантайм собирается с -DCXXRT_ARENA_KB=<размер>: куча — статический
// массив внутри самой библиотеки, как пул у ffmpeg.ll.
// =============================================================================

#include "cxxrt.h"
#include "ovos.h"

// ------------------------------------------------------------------ mem*
// Компилятор сам вставляет вызовы memcpy/memset (копирование структур,
// обнуление массивов), а libc у программы нет. rep movsb/stosb — потому что
// обычный цикл GCC на -O2 опознаёт и превращает обратно в вызов memcpy.
extern "C" void* memcpy(void* d, const void* s, size_t n) {
    void* r = d;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

extern "C" void* memset(void* d, int c, size_t n) {
    void* r = d;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

extern "C" void* memmove(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    if (dd <= ss || dd >= ss + n) return memcpy(d, s, n);
    // Перекрытие с копированием назад: rep movsb с флагом направления
    dd += n - 1;
    ss += n - 1;
    __asm__ volatile ("std\n\trep movsb\n\tcld" : "+D"(dd), "+S"(ss), "+c"(n) : : "memory");
    return d;
}

extern "C" int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

// ------------------------------------------------------------------ куча
// Заголовок блока 16 байт, полезная область выровнена на 16 (SSE любит)
struct blk_t {
    size_t size;      // размер полезной области
    blk_t* next;      // только у свободных: следующий по адресу
};

static blk_t* s_free = 0;
static size_t s_used = 0;
static const size_t HDR = sizeof(blk_t);
#ifndef CXXRT_ARENA_KB
static const size_t GROW = 256 * 1024;
#endif

static size_t align16(size_t n) { return (n + 15) & ~(size_t)15; }

// Вставить свободный блок в список по адресу и слить с соседями
static void free_insert(blk_t* b) {
    blk_t* prev = 0;
    blk_t* cur = s_free;
    while (cur && cur < b) { prev = cur; cur = cur->next; }
    b->next = cur;
    if (prev) prev->next = b; else s_free = b;
    // со следующим
    if (cur && (uint8_t*)b + HDR + b->size == (uint8_t*)cur) {
        b->size += HDR + cur->size;
        b->next = cur->next;
    }
    // с предыдущим
    if (prev && (uint8_t*)prev + HDR + prev->size == (uint8_t*)b) {
        prev->size += HDR + b->size;
        prev->next = b->next;
    }
}

#ifdef CXXRT_ARENA_KB
// Арена библиотеки: отдаётся целиком при первом же запросе
alignas(16) static uint8_t s_arena[(size_t)CXXRT_ARENA_KB * 1024];
static int s_arena_given = 0;

static int heap_grow(size_t need) {
    (void)need;
    if (s_arena_given) return 0;
    s_arena_given = 1;
    blk_t* b = (blk_t*)s_arena;
    b->size = sizeof(s_arena) - HDR;
    free_insert(b);
    return 1;
}
#else
static int heap_grow(size_t need) {
    size_t sz = align16(need + HDR);
    if (sz < GROW) sz = GROW;
    void* p = ov_sbrk((int64_t)sz);
    if (p == (void*)(uintptr_t)-1) return 0;
    // brk даёт адрес как есть; выровняем начало блока на 16
    uintptr_t a = ((uintptr_t)p + 15) & ~(uintptr_t)15;
    size_t lost = a - (uintptr_t)p;
    blk_t* b = (blk_t*)a;
    b->size = sz - lost - HDR;
    free_insert(b);
    return 1;
}
#endif

extern "C" void* ov_malloc(size_t n) {
    if (n == 0) n = 1;
    n = align16(n);
    for (int attempt = 0; attempt < 2; attempt++) {
        blk_t* prev = 0;
        for (blk_t* b = s_free; b; prev = b, b = b->next) {
            if (b->size < n) continue;
            if (b->size >= n + HDR + 32) {
                // Отрезаем хвост, остаток остаётся в списке на том же месте
                blk_t* rest = (blk_t*)((uint8_t*)b + HDR + n);
                rest->size = b->size - n - HDR;
                rest->next = b->next;
                if (prev) prev->next = rest; else s_free = rest;
                b->size = n;
            } else {
                if (prev) prev->next = b->next; else s_free = b->next;
            }
            b->next = 0;
            s_used += b->size;
            return (uint8_t*)b + HDR;
        }
        if (!heap_grow(n)) return 0;
    }
    return 0;
}

extern "C" void ov_free(void* p) {
    if (!p) return;
    blk_t* b = (blk_t*)((uint8_t*)p - HDR);
    s_used -= b->size;
    free_insert(b);
}

extern "C" void* ov_calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    if (sz && total / sz != n) return 0;
    void* p = ov_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

extern "C" size_t ov_heap_used(void) { return s_used; }

// Самый большой свободный кусок: куча может быть занята наполовину и всё
// равно не дать буфер в несколько мегабайт, если её раздробили дыры
extern "C" size_t ov_heap_largest_free(void) {
    size_t best = 0;
    for (blk_t* b = s_free; b; b = b->next) if (b->size > best) best = b->size;
    return best;
}

// ------------------------------------------------------------------ new/delete
// Исключений нет: при нехватке памяти программа завершается с кодом 12
// (ENOMEM) — вернуть nullptr из обычного new язык не разрешает.
static void* must_alloc(size_t n) {
    void* p = ov_malloc(n);
    if (!p) ov_exit(12);
    return p;
}

void* operator new(size_t n)   { return must_alloc(n); }
void* operator new[](size_t n) { return must_alloc(n); }
void operator delete(void* p) noexcept           { ov_free(p); }
void operator delete[](void* p) noexcept         { ov_free(p); }
void operator delete(void* p, size_t) noexcept   { ov_free(p); }
void operator delete[](void* p, size_t) noexcept { ov_free(p); }

// ------------------------------------------------------------------ прочее
extern "C" void __cxa_pure_virtual() { ov_exit(134); }

// Деструкторы глобальных объектов: программа выходит через exit ядра и
// разрушать их некому, поэтому регистрацию просто принимаем
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
extern "C" int atexit(void (*)(void)) { return 0; }
extern "C" { void* __dso_handle = 0; }

// Конструкторы глобальных объектов: компоновщик собирает их адреса в
// .init_array, а скрипт компоновки программы отмечает его границы. Символы
// слабые: у программы без глобальных объектов их может не быть вовсе.
// В библиотеке .ll (CXXRT_ARENA_KB) глобальных конструкторов не бывает — app.mk
// отказывается собирать такую библиотеку, — а слабая неопределённая метка там
// превратилась бы в абсолютный 32-битный ноль, который загрузчик не чинит.
#ifndef CXXRT_ARENA_KB
extern "C" {
typedef void (*ctor_t)(void);
extern ctor_t __init_array_start[] __attribute__((weak));
extern ctor_t __init_array_end[] __attribute__((weak));
}
#endif

void ovcxx_init() {
#ifndef CXXRT_ARENA_KB
    static int done = 0;
    if (done) return;
    done = 1;
    if (!__init_array_start || !__init_array_end) return;
    for (ctor_t* c = __init_array_start; c < __init_array_end; c++) (*c)();
#endif
}
