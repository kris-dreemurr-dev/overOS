// libc.ll — библиотека языка C для overOS, разделяемая (.ll).
//
// Это та самая libc, которой в системе принципиально нет на этапе компоновки:
// ядро, модули и программы собираются freestanding. Здесь она лежит отдельным
// файлом и подключается в runtime — как libc.so в Linux, только связывание не
// на старте, а по именам через ov_lib_sym (см. src/lib/libc_ll.h).
//
// Куча: malloc берёт память у ядра сисколлом brk (14) кусками по мегабайту и
// раздаёт из списка свободных блоков. Программа, загрузившая libc, не должна
// двигать brk сама — граница общая на процесс.

#include <stdint.h>

// --- сисколл напрямую: заголовка ovos.h библиотека не тянет, чтобы не зависеть
//     от размера его inline-обвязки ---
static inline uint64_t sc1(uint64_t num, uint64_t a) {
    uint64_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(num), "b"(a) : "memory");
    return r;
}

// ---------------------------------------------------------------- память
void* memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}

void* memset(void* dst, int c, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"((uint8_t)c) : "memory");
    return dst;
}

void* memmove(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);
    for (uint64_t i = n; i > 0; i--) d[i - 1] = s[i - 1];   // перекрытие справа
    return dst;
}

int memcmp(const void* a, const void* b, uint64_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (uint64_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

void* memchr(const void* s, int c, uint64_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (uint64_t i = 0; i < n; i++) if (p[i] == (uint8_t)c) return (void*)(p + i);
    return 0;
}

// ---------------------------------------------------------------- строки
uint64_t strlen(const char* s) { uint64_t n = 0; while (s[n]) n++; return n; }

char* strcpy(char* d, const char* s) {
    char* r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char* strncpy(char* d, const char* s, uint64_t n) {
    uint64_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

// Как strlcpy в BSD: всегда закрывает строку нулём, возвращает длину источника
uint64_t strlcpy(char* d, const char* s, uint64_t cap) {
    uint64_t len = strlen(s);
    if (cap) {
        uint64_t n = (len < cap - 1) ? len : cap - 1;
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return len;
}

char* strcat(char* d, const char* s) {
    char* p = d + strlen(d);
    while ((*p++ = *s++)) {}
    return d;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char* a, const char* b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char*)s;
    return (c == 0) ? (char*)s : 0;
}

char* strrchr(const char* s, int c) {
    const char* last = 0;
    for (; *s; s++) if (*s == (char)c) last = s;
    return (char*)last;
}

char* strstr(const char* hay, const char* needle) {
    if (!*needle) return (char*)hay;
    uint64_t n = strlen(needle);
    for (; *hay; hay++) if (!strncmp(hay, needle, n)) return (char*)hay;
    return 0;
}

// ---------------------------------------------------------------- символы
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

// ---------------------------------------------------------------- числа
int abs(int v) { return v < 0 ? -v : v; }

long strtol(const char* s, char** end, int base) {
    const char* p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    if (base == 0) base = (p[0] == '0') ? 8 : 10;
    long v = 0;
    while (*p) {
        int d;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (isalpha((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        p++;
    }
    if (end) *end = (char*)p;
    return neg ? -v : v;
}

int atoi(const char* s) { return (int)strtol(s, 0, 10); }

// Перевод числа в строку с основанием 2..36 (как itoa в DOS-овских libc)
char* itoa(long v, char* buf, int base) {
    if (base < 2 || base > 36) { buf[0] = '\0'; return buf; }
    char tmp[72];
    int i = 0, neg = 0;
    unsigned long u = (unsigned long)v;
    if (v < 0 && base == 10) { neg = 1; u = (unsigned long)(-v); }
    if (u == 0) tmp[i++] = '0';
    while (u) { int d = (int)(u % (unsigned)base); tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)base; }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

// ---------------------------------------------------------------- случайные числа
static uint64_t s_rand_state = 123456789;

void srand(unsigned seed) { s_rand_state = seed ? seed : 1; }

int rand(void) {                                   // xorshift64*, период 2^64-1
    uint64_t x = s_rand_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    s_rand_state = x;
    return (int)((x * 0x2545F4914F6CDD1DULL) >> 33) & 0x7FFFFFFF;
}

#ifndef LIBC_NO_HEAP
// (Определение кучи можно отключить -DLIBC_NO_HEAP: ffmpeg.ll несёт свой
//  распределитель на статическом пуле, чтобы не делить brk с программой.)
// ---------------------------------------------------------------- куча
// Список свободных блоков, память берётся у ядра через brk кусками по мегабайту.
#define HEAP_CHUNK (1024u * 1024u)
#define BLK_MAGIC  0xC0DEC0DEu

typedef struct block {
    uint32_t      magic;
    uint32_t      free;
    uint64_t      size;         // полезных байт
    struct block* next;
} block_t;

static block_t* s_head = 0;
static uint64_t s_brk = 0;

static void* heap_grow(uint64_t bytes) {
    if (!s_brk) s_brk = sc1(14, 0);            // brk(0) -> текущая граница
    if (!s_brk || s_brk == (uint64_t)-1) return 0;
    uint64_t want = bytes;
    if (want < HEAP_CHUNK) want = HEAP_CHUNK;
    uint64_t base = s_brk;
    uint64_t res = sc1(14, s_brk + want);
    if (res == (uint64_t)-1 || res < base + want) return 0;
    s_brk = res;
    return (void*)base;
}

void* malloc(uint64_t size) {
    if (!size) return 0;
    size = (size + 15) & ~15ULL;

    for (block_t* b = s_head; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        if (b->size >= size + sizeof(block_t) + 32) {          // делим большой блок
            block_t* n = (block_t*)((uint8_t*)b + sizeof(block_t) + size);
            n->magic = BLK_MAGIC;
            n->free = 1;
            n->size = b->size - size - sizeof(block_t);
            n->next = b->next;
            b->next = n;
            b->size = size;
        }
        b->free = 0;
        return (uint8_t*)b + sizeof(block_t);
    }

    block_t* b = (block_t*)heap_grow(size + sizeof(block_t));
    if (!b) return 0;
    uint64_t got = size + sizeof(block_t);
    if (got < HEAP_CHUNK) got = HEAP_CHUNK;
    b->magic = BLK_MAGIC;
    b->free = 0;
    b->size = got - sizeof(block_t);
    b->next = s_head;
    s_head = b;
    if (b->size >= size + sizeof(block_t) + 32) {              // остаток -- в свободные
        block_t* n = (block_t*)((uint8_t*)b + sizeof(block_t) + size);
        n->magic = BLK_MAGIC;
        n->free = 1;
        n->size = b->size - size - sizeof(block_t);
        n->next = b->next;
        b->next = n;
        b->size = size;
    }
    return (uint8_t*)b + sizeof(block_t);
}

void free(void* p) {
    if (!p) return;
    block_t* b = (block_t*)((uint8_t*)p - sizeof(block_t));
    if (b->magic != BLK_MAGIC) return;                          // не наш указатель
    b->free = 1;
    for (block_t* c = s_head; c; c = c->next) {                 // склейка соседей
        if (!c->free || !c->next || !c->next->free) continue;
        if ((uint8_t*)c + sizeof(block_t) + c->size != (uint8_t*)c->next) continue;
        c->size += sizeof(block_t) + c->next->size;
        c->next = c->next->next;
    }
}

void* calloc(uint64_t n, uint64_t size) {
    uint64_t total = n * size;
    if (n && total / n != size) return 0;                       // переполнение
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* realloc(void* p, uint64_t size) {
    if (!p) return malloc(size);
    if (!size) { free(p); return 0; }
    block_t* b = (block_t*)((uint8_t*)p - sizeof(block_t));
    if (b->magic == BLK_MAGIC && b->size >= size) return p;
    void* n = malloc(size);
    if (!n) return 0;
    memcpy(n, p, (b->magic == BLK_MAGIC && b->size < size) ? b->size : size);
    free(p);
    return n;
}

#endif // LIBC_NO_HEAP

// ---------------------------------------------------------------- сортировка и поиск
static void swap_bytes(uint8_t* a, uint8_t* b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { uint8_t t = a[i]; a[i] = b[i]; b[i] = t; }
}

void qsort(void* base, uint64_t count, uint64_t size, int (*cmp)(const void*, const void*)) {
    if (count < 2) return;
    uint8_t* a = (uint8_t*)base;
    // Сортировка Шелла: без рекурсии и без дополнительной памяти
    for (uint64_t gap = count / 2; gap > 0; gap /= 2) {
        for (uint64_t i = gap; i < count; i++) {
            for (uint64_t j = i; j >= gap && cmp(a + (j - gap) * size, a + j * size) > 0; j -= gap)
                swap_bytes(a + (j - gap) * size, a + j * size, size);
        }
    }
}

void* bsearch(const void* key, const void* base, uint64_t count, uint64_t size,
              int (*cmp)(const void*, const void*)) {
    const uint8_t* a = (const uint8_t*)base;
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        int c = cmp(key, a + mid * size);
        if (c == 0) return (void*)(a + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

// ---------------------------------------------------------------- форматирование
static void fmt_putc(char* buf, uint64_t cap, uint64_t* pos, char c) {
    if (*pos + 1 < cap) buf[*pos] = c;
    (*pos)++;
}

static void fmt_str(char* buf, uint64_t cap, uint64_t* pos, const char* s, int width, int left) {
    uint64_t len = strlen(s);
    if (!left) for (uint64_t i = len; i < (uint64_t)(width > 0 ? width : 0); i++) fmt_putc(buf, cap, pos, ' ');
    for (uint64_t i = 0; i < len; i++) fmt_putc(buf, cap, pos, s[i]);
    if (left) for (uint64_t i = len; i < (uint64_t)(width > 0 ? width : 0); i++) fmt_putc(buf, cap, pos, ' ');
}

// Поддержано: %d %i %u %x %X %o %c %s %p %% с шириной, '-' и нулём слева,
// а также %f/%g с фиксированной точностью (6 знаков по умолчанию).
int vsnprintf(char* buf, uint64_t cap, const char* fmt, __builtin_va_list ap) {
    uint64_t pos = 0;
    char num[80];
    for (const char* f = fmt; *f; f++) {
        if (*f != '%') { fmt_putc(buf, cap, &pos, *f); continue; }
        f++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        while (*f == '-' || *f == '0') { if (*f == '-') left = 1; else zero = 1; f++; }
        while (isdigit((unsigned char)*f)) { width = width * 10 + (*f - '0'); f++; }
        if (*f == '.') { f++; prec = 0; while (isdigit((unsigned char)*f)) { prec = prec * 10 + (*f - '0'); f++; } }
        while (*f == 'l' || *f == 'z') { lng = 1; f++; }

        switch (*f) {
            case 'd': case 'i': {
                long v = lng ? __builtin_va_arg(ap, long) : (long)__builtin_va_arg(ap, int);
                itoa(v, num, 10);
                break;
            }
            case 'u': {
                unsigned long v = lng ? __builtin_va_arg(ap, unsigned long)
                                      : (unsigned long)__builtin_va_arg(ap, unsigned);
                uint64_t u = v; char t[32]; int i = 0;
                if (!u) t[i++] = '0';
                while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
                int j = 0; while (i) num[j++] = t[--i]; num[j] = '\0';
                break;
            }
            case 'x': case 'X': case 'p': case 'o': {
                unsigned long v;
                if (*f == 'p') v = (unsigned long)__builtin_va_arg(ap, void*);
                else v = lng ? __builtin_va_arg(ap, unsigned long)
                             : (unsigned long)__builtin_va_arg(ap, unsigned);
                itoa((long)v, num, (*f == 'o') ? 8 : 16);
                if (*f == 'X') for (char* q = num; *q; q++) *q = (char)toupper((unsigned char)*q);
                if (*f == 'p') { fmt_str(buf, cap, &pos, "0x", 0, 0); }
                break;
            }
            case 'c': {
                num[0] = (char)__builtin_va_arg(ap, int); num[1] = '\0';
                break;
            }
            case 's': {
                const char* s = __builtin_va_arg(ap, const char*);
                fmt_str(buf, cap, &pos, s ? s : "(null)", width, left);
                continue;
            }
            case 'f': case 'F': case 'g': {
                double d = __builtin_va_arg(ap, double);
                int p = (prec < 0) ? 6 : prec;
                int neg = d < 0; if (neg) d = -d;
                uint64_t ip = (uint64_t)d;
                double fr = d - (double)ip;
                char t[32]; int i = 0;
                if (!ip) t[i++] = '0';
                while (ip) { t[i++] = (char)('0' + ip % 10); ip /= 10; }
                int j = 0;
                if (neg) num[j++] = '-';
                while (i) num[j++] = t[--i];
                if (p > 0) {
                    num[j++] = '.';
                    for (int k = 0; k < p; k++) { fr *= 10.0; int dd = (int)fr; num[j++] = (char)('0' + dd); fr -= dd; }
                }
                num[j] = '\0';
                break;
            }
            case '%': num[0] = '%'; num[1] = '\0'; break;
            default:  num[0] = '%'; num[1] = *f; num[2] = '\0'; break;
        }
        // Выравнивание числовых случаев
        if (zero && !left && width > (int)strlen(num)) {
            int pad = width - (int)strlen(num);
            int start = (num[0] == '-') ? 1 : 0;
            if (start) fmt_putc(buf, cap, &pos, '-');
            for (int i = 0; i < pad; i++) fmt_putc(buf, cap, &pos, '0');
            fmt_str(buf, cap, &pos, num + start, 0, 0);
        } else {
            fmt_str(buf, cap, &pos, num, width, left);
        }
    }
    if (cap) buf[(pos < cap) ? pos : cap - 1] = '\0';
    return (int)pos;
}

int snprintf(char* buf, uint64_t cap, const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

// Печать в консоль через сисколл 1: аналог printf без потока stdout
int printf(const char* fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    sc1(1, (uint64_t)buf);
    return n;
}
