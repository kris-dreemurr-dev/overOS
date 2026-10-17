#ifndef OV_STDLIB_H
#define OV_STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
void*  malloc(size_t n);
void*  calloc(size_t n, size_t sz);
void*  realloc(void* p, size_t n);
void   free(void* p);
int    posix_memalign(void** out, size_t align, size_t n);
void*  aligned_alloc(size_t align, size_t n);
void   abort(void);
void   exit(int code);
int    atoi(const char* s);
long   atol(const char* s);
double atof(const char* s);
long               strtol(const char* s, char** end, int base);
unsigned long      strtoul(const char* s, char** end, int base);
long long          strtoll(const char* s, char** end, int base);
unsigned long long strtoull(const char* s, char** end, int base);
double             strtod(const char* s, char** end);
void   qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*));
void*  bsearch(const void* key, const void* base, size_t n, size_t sz,
               int (*cmp)(const void*, const void*));
int    rand(void);
void   srand(unsigned seed);
char*  getenv(const char* name);
int    abs(int v);
long   labs(long v);
long long llabs(long long v);
#endif
