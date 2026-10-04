#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

extern void devos_set_palette(const uint8_t* raw_rgb);

// -----------------------------------------------------------------------------
// ВЫВОД НА ЭКРАН DEVOS (СИСТЕМНЫЙ ВЫЗОВ 1)
// -----------------------------------------------------------------------------
void sys_print(const char* str) {
    if (!str) return;
    __asm__ volatile (
        "int $0x80"
        :
        : "a"(1), "b"(str), "c"(0x00FFFFFF)
        : "memory"
    );
}

void* stderr = (void*)2;
void* stdout = (void*)1;
void* stdin  = (void*)0;

int vsnprintf(char* str, size_t size, const char* format, va_list ap);

int puts(const char* s) {
    if (s) {
        sys_print(s);
        sys_print("\n");
    }
    return 0;
}

int putchar(int c) {
    char buf[2] = { (char)c, 0 };
    sys_print(buf);
    return c;
}

int putc(int c, void* stream) { (void)stream; return putchar(c); }
int fputc(int c, void* stream) { (void)stream; return putchar(c); }
int fflush(void* stream) { (void)stream; return 0; }

int printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int res = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sys_print(buf);
    return res;
}

int fprintf(void* stream, const char* fmt, ...) {
    (void)stream;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int res = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sys_print(buf);
    return res;
}

int vfprintf(void* stream, const char* fmt, va_list ap) {
    (void)stream;
    char buf[512];
    int res = vsnprintf(buf, sizeof(buf), fmt, ap);
    sys_print(buf);
    return res;
}

// -----------------------------------------------------------------------------
// ПАМЯТЬ И КУЧА (RING 3)
// -----------------------------------------------------------------------------
static uintptr_t g_heap_top = 0x01600000;

void* memset(void* dest, int c, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = (uint8_t)c;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

static void* sbrk(int64_t increment) {
    uint64_t cur_brk = 0;
    __asm__ volatile ("int $0x80" : "=a"(cur_brk) : "0"(14), "b"(0) : "memory");
    if (increment == 0) return (void*)cur_brk;

    uint64_t new_brk = cur_brk + increment;
    uint64_t res = 0;
    __asm__ volatile ("int $0x80" : "=a"(res) : "0"(14), "b"(new_brk) : "memory");
    if (res == (uint64_t)-1) return (void*)-1;
    
    return (void*)cur_brk;
}

void* malloc(size_t size) {
    if (size == 0 || size > 32 * 1024 * 1024) return NULL;

    size = (size + 15) & ~15; // Выравнивание по 16 байт
    void* ptr = sbrk((int64_t)size);
    if (ptr == (void*)-1) {
        sys_print("\n>>> [DOOM LIBC] Out of memory via sys_brk! <<<\n");
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

void* calloc(size_t num, size_t size) {
    return malloc(num * size);
}

void* realloc(void* ptr, size_t size) {
    void* new_ptr = malloc(size);
    if (ptr && new_ptr) memcpy(new_ptr, ptr, size);
    return new_ptr;
}

void free(void* ptr) {
    (void)ptr;
}
// -----------------------------------------------------------------------------
// МАТЕМАТИКА
// -----------------------------------------------------------------------------
double fabs(double x) { return (x < 0.0) ? -x : x; }
float fabsf(float x) { return (x < 0.0f) ? -x : x; }
int abs(int j) { return (j < 0) ? -j : j; }

// -----------------------------------------------------------------------------
// СТРОКИ И ЧИСЛА
// -----------------------------------------------------------------------------
size_t strlen(const char* s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strcasecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;
        if (c1 != c2) return (unsigned char)c1 - (unsigned char)c2;
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncasecmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;

    while (n-- > 0) {
        unsigned char c1 = (unsigned char)*s1++;
        unsigned char c2 = (unsigned char)*s2++;

        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;

        if (c1 != c2) {
            return c1 - c2;
        }
        if (c1 == '\0') {
            return 0;
        }
    }
    return 0;
}
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for ( ; i < n; i++) dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == 0) ? (char*)s : NULL;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack, *n = needle;
            while (*h && *n && *h == *n) { h++; n++; }
            if (!*n) return (char*)haystack;
        }
    }
    return NULL;
}

char* strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* d = (char*)malloc(len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

long strtol(const char* nptr, char** endptr, int base) {
    while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' || *nptr == '\r') nptr++;
    int sign = 1;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') { nptr++; }

    if (base == 0) {
        if (*nptr == '0') {
            if (nptr[1] == 'x' || nptr[1] == 'X') { base = 16; nptr += 2; }
            else base = 8;
        } else base = 10;
    } else if (base == 16 && *nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
        nptr += 2;
    }

    long res = 0;
    while (*nptr) {
        int digit;
        if (*nptr >= '0' && *nptr <= '9') digit = *nptr - '0';
        else if (*nptr >= 'a' && *nptr <= 'f') digit = *nptr - 'a' + 10;
        else if (*nptr >= 'A' && *nptr <= 'F') digit = *nptr - 'A' + 10;
        else break;
        if (digit >= base) break;
        res = res * base + digit;
        nptr++;
    }
    if (endptr) *endptr = (char*)nptr;
    return res * sign;
}

double strtod(const char* nptr, char** endptr) {
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    double sign = 1.0;
    if (*nptr == '-') { sign = -1.0; nptr++; }
    else if (*nptr == '+') { nptr++; }
    double val = 0.0;
    while (*nptr >= '0' && *nptr <= '9') {
        val = val * 10.0 + (*nptr - '0');
        nptr++;
    }
    if (*nptr == '.') {
        nptr++;
        double div = 10.0;
        while (*nptr >= '0' && *nptr <= '9') {
            val += (*nptr - '0') / div;
            div *= 10.0;
            nptr++;
        }
    }
    if (endptr) *endptr = (char*)nptr;
    return val * sign;
}

int atoi(const char* s) { return (int)strtol(s, NULL, 10); }

// -----------------------------------------------------------------------------
// ТАБЛИЦА CTYPE
// -----------------------------------------------------------------------------
static int32_t g_toupper_table[384];
static int32_t* g_toupper_ptr = &g_toupper_table[128];

const int32_t** __ctype_toupper_loc(void) {
    static int initialized = 0;
    if (!initialized) {
        for (int i = 0; i < 256; i++) {
            g_toupper_table[128 + i] = (i >= 'a' && i <= 'z') ? (i - 32) : i;
        }
        initialized = 1;
    }
    return (const int32_t**)&g_toupper_ptr;
}

static uint16_t g_ctype_b_table[384];
static uint16_t* g_ctype_b_ptr = &g_ctype_b_table[128];

const uint16_t** __ctype_b_loc(void) {
    static int initialized = 0;
    if (!initialized) {
        for (int i = 0; i < 256; i++) {
            uint16_t mask = 0;
            if (i >= 'A' && i <= 'Z') mask |= 0x0100;
            if (i >= 'a' && i <= 'z') mask |= 0x0200;
            if (i >= '0' && i <= '9') mask |= 0x0800;
            if (i >= 0x20 && i <= 0x7E) mask |= 0x4000;
            if (i == ' ' || i == '\t' || i == '\n' || i == '\r') mask |= 0x2000;
            g_ctype_b_table[128 + i] = mask;
        }
        initialized = 1;
    }
    return (const uint16_t**)&g_ctype_b_ptr;
}

int toupper(int c) { return (c >= 'a' && c <= 'z') ? (c - 32) : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? (c + 32) : c; }

// -----------------------------------------------------------------------------
// ФОРМАТИРОВАНИЕ
// -----------------------------------------------------------------------------
static int vsscanf_impl(const char* str, const char* format, va_list ap) {
    int count = 0;
    while (*format && *str) {
        while (*str == ' ' || *str == '\t' || *str == '\n') str++;
        while (*format == ' ' || *format == '\t' || *format == '\n') format++;
        if (!*format || !*str) break;

        if (*format == '%') {
            format++;
            if (*format == 'd' || *format == 'i') {
                int* out = va_arg(ap, int*);
                int sign = 1;
                if (*str == '-') { sign = -1; str++; }
                else if (*str == '+') { str++; }
                int val = 0, read = 0;
                while (*str >= '0' && *str <= '9') {
                    val = val * 10 + (*str - '0');
                    str++;
                    read = 1;
                }
                if (read) { *out = val * sign; count++; } else break;
            } else if (*format == 'x' || *format == 'X') {
                int* out = va_arg(ap, int*);
                if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
                int val = 0, read = 0;
                while ((*str >= '0' && *str <= '9') || (*str >= 'a' && *str <= 'f') || (*str >= 'A' && *str <= 'F')) {
                    int d = 0;
                    if (*str >= '0' && *str <= '9') d = *str - '0';
                    else if (*str >= 'a' && *str <= 'f') d = *str - 'a' + 10;
                    else if (*str >= 'A' && *str <= 'F') d = *str - 'A' + 10;
                    val = (val << 4) | d;
                    str++;
                    read = 1;
                }
                if (read) { *out = val; count++; } else break;
            } else if (*format == 's') {
                char* out = va_arg(ap, char*);
                int read = 0;
                while (*str && *str != ' ' && *str != '\t' && *str != '\n') {
                    *out++ = *str++;
                    read = 1;
                }
                *out = '\0';
                if (read) count++; else break;
            }
        } else {
            if (*format != *str) break;
            format++;
            str++;
        }
    }
    return count;
}

int sscanf(const char* s, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsscanf_impl(s, format, ap);
    va_end(ap);
    return ret;
}

int __isoc99_sscanf(const char* s, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsscanf_impl(s, format, ap);
    va_end(ap);
    return ret;
}

int vsnprintf(char* str, size_t size, const char* format, va_list ap) {
    if (!str || size == 0) return 0;
    size_t idx = 0;

    while (*format && idx + 1 < size) {
        if (*format == '%') {
            format++;
            if (!*format) break;

            int zero_pad = 0;
            int width = 0;

            // Обработка флагов '0' или '.' (например, %03d или %.3d)
            if (*format == '0') {
                zero_pad = 1;
                format++;
            } else if (*format == '.') {
                zero_pad = 1;
                format++;
            }

            // Считывание ширины / точности
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }

            if (*format == 's') {
                char* s = va_arg(ap, char*);
                if (!s) s = "(null)";
                while (*s && idx + 1 < size) str[idx++] = *s++;
            } else if (*format == 'd' || *format == 'i') {
                int val = va_arg(ap, int);
                if (val < 0) {
                    if (idx + 1 < size) str[idx++] = '-';
                    val = -val;
                }
                char buf[16];
                int bidx = 0;
                if (val == 0) {
                    buf[bidx++] = '0';
                } else {
                    while (val > 0) {
                        buf[bidx++] = '0' + (val % 10);
                        val /= 10;
                    }
                }
                // Добиваем ведущими нулями, если указана ширина (напр. %.3d для 33 -> 033)
                while (bidx < width && bidx < 15 && zero_pad) {
                    buf[bidx++] = '0';
                }
                while (bidx > 0 && idx + 1 < size) {
                    str[idx++] = buf[--bidx];
                }
            } else if (*format == 'u') {
                unsigned int val = va_arg(ap, unsigned int);
                char buf[16];
                int bidx = 0;
                if (val == 0) {
                    buf[bidx++] = '0';
                } else {
                    while (val > 0) {
                        buf[bidx++] = '0' + (val % 10);
                        val /= 10;
                    }
                }
                while (bidx < width && bidx < 15 && zero_pad) {
                    buf[bidx++] = '0';
                }
                while (bidx > 0 && idx + 1 < size) {
                    str[idx++] = buf[--bidx];
                }
            } else if (*format == 'x' || *format == 'X') {
                unsigned int val = va_arg(ap, unsigned int);
                char buf[16];
                int bidx = 0;
                if (val == 0) {
                    buf[bidx++] = '0';
                } else {
                    while (val > 0) {
                        int rem = val % 16;
                        buf[bidx++] = (rem < 10) ? ('0' + rem) : ('a' + rem - 10);
                        val /= 16;
                    }
                }
                while (bidx < width && bidx < 15 && zero_pad) {
                    buf[bidx++] = '0';
                }
                while (bidx > 0 && idx + 1 < size) {
                    str[idx++] = buf[--bidx];
                }
            } else if (*format == 'c') {
                str[idx++] = (char)va_arg(ap, int);
            } else if (*format == '%') {
                str[idx++] = '%';
            } else {
                str[idx++] = *format;
            }
        } else {
            str[idx++] = *format;
        }
        format++;
    }
    str[idx] = '\0';
    return (int)idx;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, size, format, ap);
    va_end(ap);
    return res;
}

int sprintf(char* str, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, 1024, format, ap);
    va_end(ap);
    return res;
}

// -----------------------------------------------------------------------------
// СИСТЕМНЫЕ ВЫЗОВЫ И ЗАВЕРШЕНИЕ
// -----------------------------------------------------------------------------
static int g_errno = 0;
int* __errno_location(void) { return &g_errno; }

int system(const char* command) { (void)command; return 0; }
int remove(const char* pathname) { (void)pathname; return 0; }
int mkdir(const char* pathname, unsigned int mode) { (void)pathname; (void)mode; return 0; }

void exit(int status) {
    // Прямой вывод без буферизации
    //sys_print("\n\n>>> [MINI_LIBC] exit() CALLED WITH STATUS: ");
    char numbuf[16];
    // Простой вывод hex-кода
    for (int i = 7; i >= 0; i--) {
        int nibble = (status >> (i * 4)) & 0xF;
        numbuf[7 - i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    numbuf[8] = '\n';
    numbuf[9] = 0;
    //sys_print(numbuf);

    // Если это указатель на строку ошибки — выводим её
    if (status >= 0x01000000 && status < 0x02000000) {
        sys_print(">>> [STRING TEXT]: ");
        sys_print((const char*)status);
        sys_print("\n\n");
    }

    __asm__ volatile ("int $0x80" : : "a"(0), "b"(status) : "memory");
    while (1);
}

void abort(void) { exit(1); }

// -----------------------------------------------------------------------------
// ФАЙЛОВАЯ СИСТЕМА, СОХРАНЕНИЯ И КОНФИГИ (RING 3)
// -----------------------------------------------------------------------------
#define WAD_RAM_ADDR 0x05000000

static inline int sys_open(const char* fn) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(10), "b"(fn) : "memory");
    return ret;
}

static inline int sys_read(int fd, void* buf, int sz) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(11), "b"(fd), "c"(buf), "d"(sz) : "memory");
    return ret;
}

static inline void sys_close(int fd) {
    __asm__ volatile ("int $0x80" : : "a"(12), "b"(fd) : "memory");
}

static inline int sys_write(const char* fn, const void* buf, uint32_t sz) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(13), "b"(fn), "c"(buf), "d"(sz) : "memory");
    return ret;
}

#define MAX_FILES 16
typedef struct {
    int is_open;
    int is_write;
    int is_wad;
    char name[16];
    uint8_t* buffer;
    uint32_t pos;
    uint32_t size;
    uint32_t capacity;
} devos_file_t;

static devos_file_t g_files[MAX_FILES];

static const char* get_base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

void* fopen(const char* filename, const char* mode) {
    if (!filename) return NULL;
    const char* base_name = get_base_name(filename);

    devos_file_t* f = NULL;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].is_open) { f = &g_files[i]; break; }
    }
    if (!f) return NULL;
    memset(f, 0, sizeof(devos_file_t));

    // WAD файл читаем в динамически выделенную память, а не по фиксированному адресу!
    
    // Добавьте этот код в fopen() вместо проверки doom1.wad
    if (strcasecmp(base_name, "pak0.pak") == 0 || strcasecmp(base_name, "pak1.pak") == 0 || strstr(base_name, ".wad")) {
        int fd = sys_open(base_name); // Вызываем ваш сисколл
        if (fd < 0) return NULL;

        // Для Quake Shareware (pak0.pak) нужно около 18-20 МБ. 
        // Для полной версии ставьте 55 * 1024 * 1024
        f->capacity = 25 * 1024 * 1024; 
        f->buffer = (uint8_t*)malloc(f->capacity);
        if (!f->buffer) {
            sys_close(fd);
            return NULL;
        }

        int read_bytes = sys_read(fd, f->buffer, f->capacity);
        sys_close(fd);

        f->is_wad = 1; // Используем тот же флаг, чтобы не писать на диск
        f->size = (read_bytes > 0) ? read_bytes : 0;
        f->is_open = 1;
        return f;
    }

    int is_write = (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+'));

    if (is_write) {
        f->capacity = 512 * 1024; // Сейв в Doom может весить до 200-300 КБ
        f->buffer = malloc(f->capacity);
        if (!f->buffer) return NULL;
        f->is_write = 1;
        strcpy(f->name, base_name);
        f->is_open = 1;
        return f;
    } else {
        // Режим чтения: если файла нет на диске — ОБЯЗАТЕЛЬНО возвращаем NULL!
        int fd = sys_open(base_name);
        if (fd < 0) {
            return NULL;
        }

        f->capacity = 512 * 1024;
        f->buffer = malloc(f->capacity);
        if (!f->buffer) { sys_close(fd); return NULL; }

        int bytes = sys_read(fd, f->buffer, f->capacity);
        sys_close(fd);

        f->size = (bytes > 0) ? bytes : 0;
        strcpy(f->name, base_name);
        f->is_open = 1;
        return f;
    }
}

size_t fread(void* ptr, size_t size, size_t nmemb, void* stream) {
    if (!stream || !ptr || size == 0) return 0;
    devos_file_t* f = (devos_file_t*)stream;
    size_t bytes_to_read = size * nmemb;

    if (f->pos >= f->size) return 0;
    if (f->pos + bytes_to_read > f->size) bytes_to_read = f->size - f->pos;

    if (f->is_wad) {
        memcpy(ptr, f->buffer + f->pos, bytes_to_read);
    } else {
        memcpy(ptr, f->buffer + f->pos, bytes_to_read);
    }
    f->pos += bytes_to_read;
    return bytes_to_read / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, void* stream) {
    if (!stream || !ptr || size == 0) return 0;
    devos_file_t* f = (devos_file_t*)stream;
    if (!f->is_write || f->is_wad) return 0;

    size_t bytes_to_write = size * nmemb;
    if (f->pos + bytes_to_write > f->capacity) {
        bytes_to_write = f->capacity - f->pos;
    }

    memcpy(f->buffer + f->pos, ptr, bytes_to_write);
    f->pos += bytes_to_write;
    if (f->pos > f->size) f->size = f->pos;

    return bytes_to_write / size;
}

int fclose(void* stream) {
    if (!stream) return 0;
    devos_file_t* f = (devos_file_t*)stream;
    if (!f->is_open) return 0;

    if (f->is_write && !f->is_wad) {
        sys_write(f->name, f->buffer, f->size);
    }

    if (f->buffer) free(f->buffer);
    memset(f, 0, sizeof(devos_file_t));
    return 0;
}

int fseek(void* stream, long offset, int whence) {
    if (!stream) return -1;
    devos_file_t* f = (devos_file_t*)stream;
    int32_t target = 0;

    if (whence == 0)      target = (int32_t)offset;
    else if (whence == 1) target = (int32_t)f->pos + (int32_t)offset;
    else if (whence == 2) target = (int32_t)f->size + (int32_t)offset;

    if (target < 0) target = 0;
    if ((uint32_t)target > f->size) target = (int32_t)f->size;

    f->pos = (uint32_t)target;
    return 0;
}

long ftell(void* stream) {
    if (!stream) return -1;
    return (long)((devos_file_t*)stream)->pos;
}

int feof(void* stream) {
    if (!stream) return 1;
    return ((devos_file_t*)stream)->pos >= ((devos_file_t*)stream)->size;
}

int rename(const char* oldpath, const char* newpath) {
    const char* base_old = get_base_name(oldpath);
    const char* base_new = get_base_name(newpath);

    int fd = sys_open(base_old);
    if (fd < 0) return -1;

    uint8_t* temp_buf = malloc(512 * 1024);
    if (!temp_buf) { sys_close(fd); return -1; }

    int sz = sys_read(fd, temp_buf, 512 * 1024);
    sys_close(fd);

    if (sz > 0) {
        sys_write(base_new, temp_buf, sz);
    }
    free(temp_buf);
    return 0;
}

// Низкоуровневые POSIX обертки (на случай если Doom использует их для сейвов)
int open(const char* filename, int flags, ...) {
    void* stream = fopen(filename, (flags & 1) ? "wb" : "rb");
    return stream ? (int)((devos_file_t*)stream - g_files) + 100 : -1;
}

int close(int fd) {
    int idx = fd - 100;
    if (idx >= 0 && idx < MAX_FILES) {
        fclose(&g_files[idx]);
    }
    return 0;
}

int read(int fd, void* buf, size_t count) {
    int idx = fd - 100;
    if (idx >= 0 && idx < MAX_FILES) {
        return (int)fread(buf, 1, count, &g_files[idx]);
    }
    return -1;
}

int write(int fd, const void* buf, size_t count) {
    int idx = fd - 100;
    if (idx >= 0 && idx < MAX_FILES) {
        return (int)fwrite(buf, 1, count, &g_files[idx]);
    }
    return -1;
}

int lseek(int fd, int offset, int whence) {
    int idx = fd - 100;
    if (idx >= 0 && idx < MAX_FILES) {
        fseek(&g_files[idx], offset, whence);
        return (int)ftell(&g_files[idx]);
    }
    return -1;
}

void wad_reset(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_files[i].buffer) free(g_files[i].buffer);
        memset(&g_files[i], 0, sizeof(devos_file_t));
    }
}

// -----------------------------------------------------------------------------
// АЛГОРИТМЫ (QSORT)
// -----------------------------------------------------------------------------
static void swap_bytes(char *a, char *b, size_t size) {
    do {
        char tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    } while (--size > 0);
}

void qsort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *)) {
    char *pi, *pj, *pn;
    char *a = (char *)base;

    if (num <= 1) return;

    pi = a + (num / 2) * size;
    swap_bytes(a, pi, size);
    pi = a;
    pj = a + size;
    pn = a + num * size;

    while (pj < pn) {
        if (cmp(pj, a) < 0) {
            pi += size;
            swap_bytes(pi, pj, size);
        }
        pj += size;
    }
    swap_bytes(a, pi, size);

    size_t left_num = (pi - a) / size;
    size_t right_num = num - left_num - 1;

    if (left_num > 1) qsort(a, left_num, size, cmp);
    if (right_num > 1) qsort(pi + size, right_num, size, cmp);
}

// -----------------------------------------------------------------------------
// РАНДОМИЗАЦИЯ
// -----------------------------------------------------------------------------
static unsigned long int next = 1;

int rand(void) {
    next = next * 1103515245 + 12345;
    return (unsigned int)(next / 65536) % 32768;
}

void srand(unsigned int seed) {
    next = seed;
}

// -----------------------------------------------------------------------------
// УПРАВЛЕНИЕ КОНТЕКСТОМ (SETJMP / LONGJMP x86_64)
// -----------------------------------------------------------------------------
__attribute__((naked)) int setjmp(void* env) {
    __asm__ volatile (
        "mov %%rbx, 0x00(%%rdi)\n"
        "mov %%rbp, 0x08(%%rdi)\n"
        "mov %%r12, 0x10(%%rdi)\n"
        "mov %%r13, 0x18(%%rdi)\n"
        "mov %%r14, 0x20(%%rdi)\n"
        "mov %%r15, 0x28(%%rdi)\n"
        "lea 8(%%rsp), %%rdx\n" // Сохраняем RSP до вызова функции
        "mov %%rdx, 0x30(%%rdi)\n"
        "mov (%%rsp), %%rdx\n"  // Сохраняем адрес возврата (RIP)
        "mov %%rdx, 0x38(%%rdi)\n"
        "xor %%eax, %%eax\n"    // Возвращаем 0 при первом вызове
        "ret\n"
        ::: "memory"
    );
}

__attribute__((naked)) void longjmp(void* env, int val) {
    __asm__ volatile (
        "mov %%esi, %%eax\n"
        "test %%eax, %%eax\n"
        "jnz 1f\n"
        "inc %%eax\n"           // longjmp не может вернуть 0
        "1:\n"
        "mov 0x00(%%rdi), %%rbx\n"
        "mov 0x08(%%rdi), %%rbp\n"
        "mov 0x10(%%rdi), %%r12\n"
        "mov 0x18(%%rdi), %%r13\n"
        "mov 0x20(%%rdi), %%r14\n"
        "mov 0x28(%%rdi), %%r15\n"
        "mov 0x30(%%rdi), %%rsp\n"
        "mov 0x38(%%rdi), %%rdx\n"
        "jmp *%%rdx\n"          // Прыжок по сохраненному RIP
        ::: "memory"
    );
}

typedef long time_t;
time_t time(time_t *tloc) {
    // Временно возвращаем тики таймера, чтобы движок не застрял в одном сиде
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    time_t current = lo; 
    if (tloc) *tloc = current;
    return current;
}

// -----------------------------------------------------------------------------
// МАТЕМАТИКА FPU (ДЛЯ QUAKE)
// -----------------------------------------------------------------------------
double sin(double x) {
    double res;
    __asm__ volatile ("fsin" : "=t" (res) : "0" (x));
    return res;
}

double cos(double x) {
    double res;
    __asm__ volatile ("fcos" : "=t" (res) : "0" (x));
    return res;
}

double sqrt(double x) {
    double res;
    __asm__ volatile ("fsqrt" : "=t" (res) : "0" (x));
    return res;
}

double atan2(double y, double x) {
    double res;
    __asm__ volatile ("fpatan" : "=t" (res) : "0" (x), "u" (y) : "st(1)");
    return res;
}

double atan(double x) {
    return atan2(x, 1.0);
}

double floor(double x) {
    int64_t i = (int64_t)x;
    return (x < 0 && x != (double)i) ? (double)(i - 1) : (double)i;
}

double ceil(double x) {
    int64_t i = (int64_t)x;
    return (x > 0 && x != (double)i) ? (double)(i + 1) : (double)i;
}

// -----------------------------------------------------------------------------
// ВВОД/ВЫВОД И ФОРМАТИРОВАНИЕ (ДЛЯ QUAKE)
// -----------------------------------------------------------------------------
int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, 32768, format, ap);
}

int fgetc(void *stream) {
    if (!stream) return -1;
    devos_file_t* f = (devos_file_t*)stream;
    if (f->pos >= f->size) return -1;
    return (int)f->buffer[f->pos++];
}

int getc(void *stream) {
    return fgetc(stream);
}

int __isoc99_fscanf(void *stream, const char *format, ...) {
    if (!stream) return -1;
    devos_file_t* f = (devos_file_t*)stream;
    va_list ap;
    va_start(ap, format);
    int count = 0;

    while (*format && f->pos < f->size) {
        if (*format == '%') {
            format++;
            if (*format == 'i' || *format == 'd') {
                int* out = va_arg(ap, int*);
                while (f->pos < f->size && (f->buffer[f->pos] == ' ' || f->buffer[f->pos] == '\t' || 
                       f->buffer[f->pos] == '\n' || f->buffer[f->pos] == '\r')) {
                    f->pos++;
                }
                int val = 0, sign = 1;
                if (f->pos < f->size && f->buffer[f->pos] == '-') { sign = -1; f->pos++; }
                else if (f->pos < f->size && f->buffer[f->pos] == '+') { f->pos++; }
                int read_any = 0;
                while (f->pos < f->size && f->buffer[f->pos] >= '0' && f->buffer[f->pos] <= '9') {
                    val = val * 10 + (f->buffer[f->pos] - '0');
                    f->pos++;
                    read_any = 1;
                }
                if (read_any) {
                    *out = val * sign;
                    count++;
                }
            } else if (*format == 's') {
                char* out = va_arg(ap, char*);
                while (f->pos < f->size && (f->buffer[f->pos] == ' ' || f->buffer[f->pos] == '\t' || 
                       f->buffer[f->pos] == '\n' || f->buffer[f->pos] == '\r')) {
                    f->pos++;
                }
                int read_any = 0;
                while (f->pos < f->size && f->buffer[f->pos] != ' ' && f->buffer[f->pos] != '\t' && 
                       f->buffer[f->pos] != '\n' && f->buffer[f->pos] != '\r') {
                    *out++ = f->buffer[f->pos++];
                    read_any = 1;
                }
                *out = '\0';
                if (read_any) count++;
            }
        } else if (*format == ' ' || *format == '\t' || *format == '\n' || *format == '\r') {
            while (f->pos < f->size && (f->buffer[f->pos] == ' ' || f->buffer[f->pos] == '\t' || 
                   f->buffer[f->pos] == '\n' || f->buffer[f->pos] == '\r')) {
                f->pos++;
            }
        } else {
            if (f->buffer[f->pos] == *format) f->pos++;
        }
        format++;
    }
    va_end(ap);
    return count == 0 ? -1 : count;
}

int fscanf(void *stream, const char *format, ...) __attribute__((alias("__isoc99_fscanf")));

// -----------------------------------------------------------------------------
// ТАНГЕНС, СТЕПЕНЬ И ОШИБКИ (ДЛЯ QUAKE)
// -----------------------------------------------------------------------------

// Вычисление тангенса через sin/cos на FPU[cite: 17]
double tan(double x) {
    double sin_x, cos_x;
    __asm__ volatile ("fsincos" : "=t" (cos_x), "=u" (sin_x) : "0" (x));
    return sin_x / cos_x;
}

// Возведение в степень на x87 FPU: x^y = 2^(y * log2(x)) (нужно для гаммы в view.c)[cite: 17]
double pow(double x, double y) {
    if (x <= 0.0) return 0.0;
    double res;
    __asm__ volatile (
        "fldl %2\n"              // st(1) = y
        "fldl %1\n"              // st(0) = x
        "fyl2x\n"                // st(0) = y * log2(x)
        "fld %%st(0)\n"          // дублируем значение
        "frndint\n"              // st(0) = round(val) — целая часть
        "fxch %%st(1)\n"         // st(0) = val, st(1) = round(val)
        "fsub %%st(1), %%st(0)\n"// st(0) = дробная часть
        "f2xm1\n"                // st(0) = 2^(дробь) - 1
        "fld1\n"
        "faddp\n"                // st(0) = 2^(дробь)
        "fscale\n"               // st(0) = 2^(дробь) * 2^(целое) = x^y
        "fstp %%st(1)\n"         // очищаем стек
        : "=t"(res)
        : "m"(x), "m"(y)
        : "st(1)"
    );
    return res;
}

// Заглушка текста ошибки (требуется в sys_null.c при неудачной записи файла)[cite: 17]
char* strerror(int errnum) {
    (void)errnum;
    return "I/O error";
}