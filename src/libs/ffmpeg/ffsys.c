// =============================================================================
// ffsys.c — «сисрут» overOS для настоящего FFmpeg.
//
// FFmpeg написан под обычную систему с libc: он зовёт malloc, snprintf, open,
// lseek, clock и десяток функций математики. У overOS libc нет, поэтому этот
// файл и заголовки в src/libs/ffmpeg/include и есть вся платформа, на которую
// FFmpeg опирается. Ничего лишнего: только то, что действительно вызывается
// (см. tools/build_ffmpeg.sh — сборка настроена так, чтобы FFmpeg не просил
// сети, потоков, iconv и каталогов).
//
// Строки и форматирование берутся из src/libs/libc.c, математика — из
// src/libs/libm.c: они линкуются в ту же библиотеку, здесь только то, чего в
// них нет.
//
// Память: своя куча на статическом пуле внутри образа библиотеки, а не brk.
// Это важно — brk у процесса один, и libc.ll программы уже им распоряжается;
// две кучи на одной границе затёрли бы друг друга.
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include "ovos.h"

// ------------------------------------------------------------------ errno
int errno = 0;

// ------------------------------------------------------------------ куча
// Пул лежит в .bss библиотеки: он весь попадает в lib_open как mem_size.
// 40 МБ хватает на разбор контейнера, буфер опорных кадров H.264 для
// 1920x1088 и звуковые очереди; потолок загрузчика — LIB_MAX_MEM (240 МБ).
#define POOL_SIZE   (64u * 1024u * 1024u)
#define BLK_ALIGN   64u
#define BLK_MAGIC   0x4F56414Cu

typedef struct blk {
    uint32_t     magic;
    uint32_t     free;
    uint64_t     size;          // полезных байт
    struct blk*  next;          // следующий по адресу
    uint64_t     pad;           // заголовок кратен 32, полезная часть — 64
} blk_t;

static uint8_t  s_pool[POOL_SIZE] __attribute__((aligned(64)));
static blk_t*   s_head = 0;

static void pool_init(void) {
    uint64_t base = (uint64_t)(uintptr_t)s_pool;
    uint64_t first = (base + sizeof(blk_t) + BLK_ALIGN - 1) & ~(uint64_t)(BLK_ALIGN - 1);
    blk_t* b = (blk_t*)(uintptr_t)(first - sizeof(blk_t));
    b->magic = BLK_MAGIC;
    b->free = 1;
    b->size = POOL_SIZE - (first - base);
    b->next = 0;
    b->pad = 0;
    s_head = b;
}

static void* blk_data(blk_t* b) { return (uint8_t*)b + sizeof(blk_t); }

// Разрезать блок, если остаток стоит отдельного блока
static void blk_split(blk_t* b, uint64_t need) {
    uint64_t data = (uint64_t)(uintptr_t)blk_data(b);
    uint64_t rest_hdr = (data + need + sizeof(blk_t) + BLK_ALIGN - 1) & ~(uint64_t)(BLK_ALIGN - 1);
    rest_hdr -= sizeof(blk_t);
    uint64_t end = data + b->size;
    if (rest_hdr + sizeof(blk_t) + BLK_ALIGN > end) return;
    blk_t* n = (blk_t*)(uintptr_t)rest_hdr;
    n->magic = BLK_MAGIC;
    n->free = 1;
    n->size = end - (rest_hdr + sizeof(blk_t));
    n->next = b->next;
    n->pad = 0;
    b->next = n;
    b->size = rest_hdr - data;
}

static void blk_merge(void) {
    for (blk_t* b = s_head; b && b->next; ) {
        if (b->free && b->next->free) {
            uint64_t end = (uint64_t)(uintptr_t)blk_data(b) + b->size;
            if (end + sizeof(blk_t) >= (uint64_t)(uintptr_t)b->next) {
                b->size = (uint64_t)(uintptr_t)blk_data(b->next) + b->next->size
                          - (uint64_t)(uintptr_t)blk_data(b);
                b->next = b->next->next;
                continue;
            }
        }
        b = b->next;
    }
}

void* malloc(size_t size) {
    if (!size) size = 1;
    if (!s_head) pool_init();
    size = (size + 15) & ~(size_t)15;
    for (blk_t* b = s_head; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        blk_split(b, size);
        b->free = 0;
        return blk_data(b);
    }
    return 0;
}

void free(void* p) {
    if (!p) return;
    blk_t* b = (blk_t*)((uint8_t*)p - sizeof(blk_t));
    if (b->magic != BLK_MAGIC) return;
    b->free = 1;
    blk_merge();
}

void* calloc(size_t n, size_t size) {
    uint64_t total = (uint64_t)n * size;
    if (n && total / n != size) return 0;
    void* p = malloc(total);
    if (p) { uint8_t* d = (uint8_t*)p; for (uint64_t i = 0; i < total; i++) d[i] = 0; }
    return p;
}

void* realloc(void* p, size_t size) {
    if (!p) return malloc(size);
    if (!size) { free(p); return 0; }
    blk_t* b = (blk_t*)((uint8_t*)p - sizeof(blk_t));
    if (b->magic != BLK_MAGIC) return 0;
    if (b->size >= size) return p;
    // Попробовать дорасти за счёт свободного соседа — иначе av_fast_realloc
    // при каждом кадре копировал бы буфер заново
    if (b->next && b->next->free) {
        uint64_t grown = (uint64_t)(uintptr_t)blk_data(b->next) + b->next->size
                         - (uint64_t)(uintptr_t)blk_data(b);
        if (grown >= size) {
            b->size = grown;
            b->next = b->next->next;
            blk_split(b, size);
            return p;
        }
    }
    void* n = malloc(size);
    if (!n) return 0;
    uint8_t* d = (uint8_t*)n; const uint8_t* s = (const uint8_t*)p;
    for (uint64_t i = 0; i < b->size; i++) d[i] = s[i];
    free(p);
    return n;
}

// FFmpeg выравнивает буферы сам, но если posix_memalign есть — пользуется им.
// Полезная часть блока всегда кратна 64 байтам, этого хватает любому кодеку.
int posix_memalign(void** out, size_t align, size_t size) {
    if (align > BLK_ALIGN) {
        void* p = malloc(size + align);
        if (!p) return 12; // ENOMEM
        uint64_t a = ((uint64_t)(uintptr_t)p + align - 1) & ~(uint64_t)(align - 1);
        if (a != (uint64_t)(uintptr_t)p) { free(p); return 22; } // EINVAL: не умеем сдвигать
        *out = p;
        return 0;
    }
    void* p = malloc(size);
    if (!p) return 12;
    *out = p;
    return 0;
}

void* aligned_alloc(size_t align, size_t size) {
    void* p = 0;
    return posix_memalign(&p, align, size) == 0 ? p : 0;
}

// ------------------------------------------------------------------ прочее из stdlib
void abort(void) { ov_exit(134); }
void exit(int code) { ov_exit(code); }
char* getenv(const char* name) { (void)name; return 0; }
long  labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }
long  atol(const char* s) { extern long strtol(const char*, char**, int); return strtol(s, 0, 10); }

int isxdigit(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c)  { return c >= 0x20 && c < 0x7F; }
int iscntrl(int c)  { return c < 0x20 || c == 0x7F; }
int isgraph(int c)  { return c > 0x20 && c < 0x7F; }
int ispunct(int c)  { return isgraph(c) && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')); }

extern uint64_t strlen(const char*);
extern void* memcpy(void*, const void*, uint64_t);

char* strdup(const char* s) {
    uint64_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int in_set(char c, const char* set) {
    for (const char* p = set; *p; p++) if (*p == c) return 1;
    return 0;
}
size_t strspn(const char* s, const char* set) {
    size_t n = 0; while (s[n] && in_set(s[n], set)) n++; return n;
}
size_t strcspn(const char* s, const char* set) {
    size_t n = 0; while (s[n] && !in_set(s[n], set)) n++; return n;
}
char* strncat(char* d, const char* s, size_t n) {
    char* e = d + strlen(d);
    size_t i = 0;
    while (i < n && s[i]) { e[i] = s[i]; i++; }
    e[i] = '\0';
    return d;
}

char* strerror(int e) {
    switch (e) {
        case 2:  return "нет такого файла";
        case 5:  return "ошибка ввода-вывода";
        case 9:  return "неверный дескриптор";
        case 12: return "нет памяти";
        case 13: return "нет доступа";
        case 22: return "неверный аргумент";
        case 38: return "не поддерживается";
        default: return "ошибка";
    }
}
int strerror_r(int e, char* buf, size_t n) {
    const char* s = strerror(e);
    size_t i = 0;
    while (s[i] && i + 1 < n) { buf[i] = s[i]; i++; }
    if (n) buf[i] = '\0';
    return 0;
}

void ov_assert_fail(const char* expr, const char* file, int line) {
    (void)line;
    ov_print("ffmpeg: нарушено условие ");
    ov_print(expr);
    ov_print(" в ");
    ov_print(file);
    ov_print("\n");
    ov_exit(134);
}

// ------------------------------------------------------------------ строки -> числа
static int digit_of(char c, int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

static unsigned long long parse_u(const char* s, char** end, int base, int* neg) {
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    *neg = 0;
    if (*p == '+' || *p == '-') { *neg = (*p == '-'); p++; }
    if ((base == 16 || base == 0) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0) { base = (p[0] == '0') ? 8 : 10; }
    unsigned long long v = 0;
    int any = 0, d;
    while ((d = digit_of(*p, base)) >= 0) { v = v * (unsigned)base + (unsigned)d; p++; any = 1; }
    if (end) *end = (char*)(any ? p : s);
    return v;
}

unsigned long strtoul(const char* s, char** end, int base) {
    int neg; unsigned long long v = parse_u(s, end, base, &neg);
    return neg ? (unsigned long)(0 - v) : (unsigned long)v;
}
unsigned long long strtoull(const char* s, char** end, int base) {
    int neg; unsigned long long v = parse_u(s, end, base, &neg);
    return neg ? (0 - v) : v;
}
long long strtoll(const char* s, char** end, int base) {
    int neg; unsigned long long v = parse_u(s, end, base, &neg);
    return neg ? -(long long)v : (long long)v;
}

extern double pow(double, double);
double strtod(const char* s, char** end) {
    const char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    double v = 0.0;
    int any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10.0 + (*p - '0'); p++; any = 1; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (*p >= '0' && *p <= '9') { v += (*p - '0') * f; f *= 0.1; p++; any = 1; }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char* save = p;
        p++;
        int eneg = 0;
        if (*p == '+' || *p == '-') { eneg = (*p == '-'); p++; }
        if (*p >= '0' && *p <= '9') {
            int e = 0;
            while (*p >= '0' && *p <= '9') { e = e * 10 + (*p - '0'); p++; }
            v *= pow(10.0, eneg ? -e : e);
        } else p = save;
    }
    if (end) *end = (char*)(any ? p : s);
    return neg ? -v : v;
}
double atof(const char* s) { return strtod(s, 0); }

// ------------------------------------------------------------------ математика
// Чего нет в src/libs/libm.c.
extern double exp(double), log(double), atan(double), log2(double), ldexp(double, int);
extern double floor(double), fabs(double);

double cbrt(double x) {
    if (x == 0.0) return 0.0;
    int neg = x < 0.0;
    if (neg) x = -x;
    double r = exp(log(x) / 3.0);
    for (int i = 0; i < 4; i++) r = r - (r * r * r - x) / (3.0 * r * r);   // Ньютон
    return neg ? -r : r;
}
float  cbrtf(float x) { return (float)cbrt((double)x); }
float  atanf(float x) { return (float)atan((double)x); }
float  log2f(float x) { return (float)log2((double)x); }
double scalbn(double x, int n) { return ldexp(x, n); }
double rint(double x) {
    double f = floor(x);
    double d = x - f;
    if (d < 0.5) return f;
    if (d > 0.5) return f + 1.0;
    // ровно половина — к чётному, как требует IEEE
    double h = f * 0.5;
    return (h == floor(h)) ? f : f + 1.0;
}
double nearbyint(double x) { return rint(x); }
float  rintf(float x) { return (float)rint((double)x); }
long      lrint(double x)  { return (long)rint(x); }
long long llrint(double x) { return (long long)rint(x); }
long      lrintf(float x)  { return (long)rint((double)x); }
extern double trunc(double), round(double), hypot(double, double);
float truncf(float x) { return (float)trunc((double)x); }
float roundf(float x) { return (float)round((double)x); }
float hypotf(float a, float b) { return (float)hypot((double)a, (double)b); }
extern double sin(double), cos(double);
void sincos(double x, double* s, double* c) { *s = sin(x); *c = cos(x); }
void sincosf(float x, float* s, float* c) { *s = (float)sin((double)x); *c = (float)cos((double)x); }

// ------------------------------------------------------------------ файлы
int open(const char* path, int flags, ...) {
    int fl = 0;
    if (flags & 1) fl |= OV_O_WRONLY;
    if (flags & 2) fl |= OV_O_RDWR;
    if (flags & 0x40) fl |= OV_O_CREAT;
    int fd = ov_open(path, fl);
    if (fd < 0) { errno = 2; return -1; }
    return fd;
}
int close(int fd) { return ov_close(fd); }
int64_t read(int fd, void* buf, size_t n) {
    int64_t r = ov_read(fd, buf, n);
    if (r < 0) { errno = 5; return -1; }
    return r;
}
int64_t write(int fd, const void* buf, size_t n) {
    int64_t r = ov_write(fd, buf, n);
    if (r < 0) { errno = 5; return -1; }
    return r;
}
long lseek(int fd, long off, int whence) {
    int64_t r = ov_lseek(fd, off, whence);
    if (r < 0) { errno = 22; return -1; }
    return (long)r;
}
int mkdir(const char* path, unsigned mode) { (void)mode; return ov_mkdir(path) > 0 ? 0 : -1; }

// stat/fstat для демультиплексора image2: ему нужен только размер файла.
// Прав доступа у FAT32 нет, поэтому отдаём обычный файл с правами 0644.
struct ov_stat { unsigned mode; long size; long mtime; };

int fstat(int fd, struct ov_stat* out) {
    if (!out || fd < 0) { errno = 9; return -1; }
    int64_t cur = ov_lseek(fd, 0, OV_SEEK_CUR);
    int64_t end = ov_lseek(fd, 0, OV_SEEK_END);
    if (cur >= 0) ov_lseek(fd, cur, OV_SEEK_SET);
    if (end < 0) { errno = 5; return -1; }
    out->mode = 0100644;        // S_IFREG | 0644
    out->size = (long)end;
    out->mtime = 0;
    return 0;
}

int stat(const char* path, struct ov_stat* out) {
    int fd = ov_open(path, 0);
    if (fd < 0) { errno = 2; return -1; }
    int r = fstat(fd, out);
    ov_close(fd);
    return r;
}
int unlink(const char* path) { (void)path; errno = 38; return -1; }
int isatty(int fd) { return fd <= 2; }
unsigned sleep(unsigned sec) { ov_sleep(sec * 1000); return 0; }
int usleep(unsigned usec) { ov_sleep(usec / 1000); return 0; }

// ------------------------------------------------------------------ потоки stdio
// Настоящих потоков нет: FILE — это дескриптор ядра, а журнал FFmpeg уходит
// в консоль сисколлом печати.
struct ov_file { int fd; };
static struct ov_file s_in = { 0 }, s_out = { 1 }, s_err = { 2 };
struct ov_file* stdin  = &s_in;
struct ov_file* stdout = &s_out;
struct ov_file* stderr = &s_err;

extern int vsnprintf(char*, uint64_t, const char*, va_list);

static void stream_put(struct ov_file* f, const char* s, uint64_t n) {
    if (!f) return;
    if (f->fd == 1 || f->fd == 2) {                 // журнал — в консоль
        char buf[512];
        uint64_t i = 0;
        while (i < n) {
            uint64_t k = 0;
            while (k + 1 < sizeof(buf) && i < n) buf[k++] = s[i++];
            buf[k] = '\0';
            ov_print(buf);
        }
        return;
    }
    ov_write(f->fd, s, n);
}

int vfprintf(struct ov_file* f, const char* fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    uint64_t len = (uint64_t)n < sizeof(buf) ? (uint64_t)n : sizeof(buf) - 1;
    stream_put(f, buf, len);
    return n;
}
int fprintf(struct ov_file* f, const char* fmt, ...) {
    va_list ap; __builtin_va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    __builtin_va_end(ap);
    return n;
}
int sprintf(char* buf, const char* fmt, ...) {
    va_list ap; __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    __builtin_va_end(ap);
    return n;
}
int fputc(int c, struct ov_file* f) { char ch = (char)c; stream_put(f, &ch, 1); return c; }
int fputs(const char* s, struct ov_file* f) { stream_put(f, s, strlen(s)); return 0; }
int putchar(int c) { return fputc(c, stdout); }
int fflush(struct ov_file* f) { (void)f; return 0; }
int setvbuf(struct ov_file* f, char* buf, int mode, size_t size) { (void)f; (void)buf; (void)mode; (void)size; return 0; }
size_t fwrite(const void* p, size_t sz, size_t n, struct ov_file* f) {
    stream_put(f, (const char*)p, (uint64_t)sz * n);
    return n;
}
size_t fread(void* p, size_t sz, size_t n, struct ov_file* f) {
    int64_t r = ov_read(f->fd, p, (uint64_t)sz * n);
    return r > 0 && sz ? (size_t)r / sz : 0;
}
struct ov_file* fopen(const char* path, const char* mode) {
    int fl = (mode[0] == 'w') ? (OV_O_WRONLY | OV_O_CREAT) : OV_O_RDONLY;
    int fd = ov_open(path, fl);
    if (fd < 0) return 0;
    struct ov_file* f = (struct ov_file*)malloc(sizeof(struct ov_file));
    if (!f) { ov_close(fd); return 0; }
    f->fd = fd;
    return f;
}
struct ov_file* fdopen(int fd, const char* mode) {
    (void)mode;
    if (fd < 0) return 0;
    struct ov_file* f = (struct ov_file*)malloc(sizeof(struct ov_file));
    if (!f) return 0;
    f->fd = fd;
    return f;
}

int fclose(struct ov_file* f) {
    if (!f) return -1;
    ov_close(f->fd);
    free(f);
    return 0;
}
int fseek(struct ov_file* f, long off, int whence) { return ov_lseek(f->fd, off, whence) < 0 ? -1 : 0; }
long ftell(struct ov_file* f) { return (long)ov_lseek(f->fd, 0, OV_SEEK_CUR); }

// Минимальный sscanf: %d %u %x %s %c %f и пробелы — больше FFmpeg от него и
// не просит (разбор строк метаданных).
int sscanf(const char* s, const char* fmt, ...) {
    va_list ap; __builtin_va_start(ap, fmt);
    int got = 0;
    const char* p = s;
    for (const char* f = fmt; *f; f++) {
        if (*f == ' ') { while (*p == ' ' || *p == '\t') p++; continue; }
        if (*f != '%') {
            if (*p != *f) break;
            p++;
            continue;
        }
        f++;
        int skip = 0, width = 0;
        if (*f == '*') { skip = 1; f++; }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        while (*f == 'l' || *f == 'h' || *f == 'z') f++;
        char conv = *f;
        while (conv != 'c' && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
        char* end = 0;
        if (conv == 'd' || conv == 'i') {
            int neg; unsigned long long v = parse_u(p, &end, 10, &neg);
            if (end == p) break;
            if (!skip) *__builtin_va_arg(ap, int*) = neg ? -(int)v : (int)v;
            p = end;
        } else if (conv == 'u' || conv == 'x') {
            int neg; unsigned long long v = parse_u(p, &end, conv == 'x' ? 16 : 10, &neg);
            if (end == p) break;
            if (!skip) *__builtin_va_arg(ap, unsigned*) = (unsigned)v;
            p = end;
        } else if (conv == 'f' || conv == 'g' || conv == 'e') {
            double v = strtod(p, &end);
            if (end == p) break;
            if (!skip) *__builtin_va_arg(ap, float*) = (float)v;
            p = end;
        } else if (conv == 'c') {
            if (!*p) break;
            if (!skip) *__builtin_va_arg(ap, char*) = *p;
            p++;
        } else if (conv == 's') {
            char* out = skip ? 0 : __builtin_va_arg(ap, char*);
            int n = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && (!width || n < width)) {
                if (out) out[n] = *p;
                n++; p++;
            }
            if (!n) break;
            if (out) out[n] = '\0';
        } else if (conv == '%') {
            if (*p != '%') break;
            p++;
            continue;
        } else break;
        if (!skip) got++;
    }
    __builtin_va_end(ap);
    return got;
}

// ------------------------------------------------------------------ время
// Часы системы — сисколл 8 (RTC), монотонные миллисекунды — счётчик такта.
long time(long* out) {
    typedef struct { uint8_t sec, min, hour, day, month, year; } rtc_t;
    rtc_t t;
    ov_sc(8, (uint64_t)&t, 0, 0);
    // дни с эпохи по алгоритму «дни от гражданской даты»
    int y = 2000 + t.year, m = t.month, d = t.day;
    int era_y = y - (m <= 2 ? 1 : 0);
    int era = (era_y >= 0 ? era_y : era_y - 399) / 400;
    unsigned yoe = (unsigned)(era_y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    long secs = days * 86400 + t.hour * 3600 + t.min * 60 + t.sec;
    if (out) *out = secs;
    return secs;
}

// clock(): микросекунды с запуска — FFmpeg берёт этим зерно для случайных чисел
long clock(void) { return (long)(ov_ms() * 1000); }

int gettimeofday(void* tvp, void* tzp) {
    struct { long sec; long usec; } *tv = tvp;
    (void)tzp;
    if (!tv) return -1;
    tv->sec = time(0);
    tv->usec = (long)(ov_ms() % 1000) * 1000;
    return 0;
}

struct ov_tm { int sec, min, hour, mday, mon, year, wday, yday, isdst; };

struct ov_tm* gmtime_r(const long* tp, struct ov_tm* out) {
    long t = *tp;
    long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    out->hour = (int)(rem / 3600);
    out->min = (int)((rem % 3600) / 60);
    out->sec = (int)(rem % 60);
    out->wday = (int)((days + 4) % 7);
    if (out->wday < 0) out->wday += 7;
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    if (m <= 2) y++;
    out->year = (int)(y - 1900);
    out->mon = (int)m - 1;
    out->mday = (int)d;
    out->yday = 0;
    out->isdst = 0;
    return out;
}
struct ov_tm* localtime_r(const long* tp, struct ov_tm* out) { return gmtime_r(tp, out); }

static long tm_to_secs(const struct ov_tm* tm) {
    int y = tm->year + 1900, m = tm->mon + 1, d = tm->mday;
    int era_y = y - (m <= 2 ? 1 : 0);
    int era = (era_y >= 0 ? era_y : era_y - 399) / 400;
    unsigned yoe = (unsigned)(era_y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return days * 86400 + tm->hour * 3600 + tm->min * 60 + tm->sec;
}
long mktime(struct ov_tm* tm) { return tm_to_secs(tm); }
long timegm(struct ov_tm* tm) { return tm_to_secs(tm); }

size_t strftime(char* buf, size_t n, const char* fmt, const struct ov_tm* tm) {
    extern int snprintf(char*, uint64_t, const char*, ...);
    // FFmpeg форматирует этим только дату создания в метаданных
    (void)fmt;
    int k = snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tm->year + 1900, tm->mon + 1, tm->mday, tm->hour, tm->min, tm->sec);
    return (k > 0 && (size_t)k < n) ? (size_t)k : 0;
}

// ---------------------------------------------------------------------------
// Заглушки таблиц FFT, вырезанных заплатой 01-tx-tables.
//
// Ассемблерные ядра libavutil (tx_float.asm) ссылаются на таблицы длин до
// 2 097 152 отсчётов безусловно — адрес берётся инструкцией lea в начале
// соответствующей ветки. Самих веток для таких длин у нас никто не вызовет:
// заплата 02-tx-x86-maxlen объявляет максимумом 131 072, да и ни один кодек
// выше 2048 не поднимается (AAC — 2048, MP3 — 1152, SBR — 64). Но линкеру
// символы всё равно нужны, а держать ради них 20 МБ .bss в каждой копии
// библиотеки — расточительство, ради которого заплата и писалась.
// ---------------------------------------------------------------------------
float ff_tx_tab_262144_float[1];
float ff_tx_tab_524288_float[1];
float ff_tx_tab_1048576_float[1];
float ff_tx_tab_2097152_float[1];
