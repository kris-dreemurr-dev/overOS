#ifndef LIBC_LL_H
#define LIBC_LL_H
// libc_ll.h — удобная привязка к /lib/libc.ll (разделяемая libc overOS).
//
//     libc_t c;
//     if (libc_load(&c) < 0) { /* библиотеки нет */ }
//     char* buf = c.malloc(1024);
//     c.snprintf(buf, 1024, "%s: %d", "кадров", n);
//     c.free(buf);
//     libc_unload(&c);
//
// Поля — адреса функций из библиотеки. Куча libc работает через brk процесса,
// поэтому программа, загрузившая libc, не должна двигать brk сама.

#include "ovos.h"

#define LIBC_PATH "/lib/libc.ll"

typedef struct {
    int lib;

    void*    (*memcpy)(void*, const void*, uint64_t);
    void*    (*memset)(void*, int, uint64_t);
    void*    (*memmove)(void*, const void*, uint64_t);
    int      (*memcmp)(const void*, const void*, uint64_t);
    void*    (*memchr)(const void*, int, uint64_t);

    uint64_t (*strlen)(const char*);
    char*    (*strcpy)(char*, const char*);
    char*    (*strncpy)(char*, const char*, uint64_t);
    uint64_t (*strlcpy)(char*, const char*, uint64_t);
    char*    (*strcat)(char*, const char*);
    int      (*strcmp)(const char*, const char*);
    int      (*strncmp)(const char*, const char*, uint64_t);
    char*    (*strchr)(const char*, int);
    char*    (*strrchr)(const char*, int);
    char*    (*strstr)(const char*, const char*);

    int      (*isdigit)(int);
    int      (*isalpha)(int);
    int      (*isalnum)(int);
    int      (*isspace)(int);
    int      (*toupper)(int);
    int      (*tolower)(int);

    int      (*abs)(int);
    long     (*strtol)(const char*, char**, int);
    int      (*atoi)(const char*);
    char*    (*itoa)(long, char*, int);

    void     (*srand)(unsigned);
    int      (*rand)(void);

    void*    (*malloc)(uint64_t);
    void     (*free)(void*);
    void*    (*calloc)(uint64_t, uint64_t);
    void*    (*realloc)(void*, uint64_t);

    void     (*qsort)(void*, uint64_t, uint64_t, int (*)(const void*, const void*));
    void*    (*bsearch)(const void*, const void*, uint64_t, uint64_t,
                        int (*)(const void*, const void*));

    int      (*snprintf)(char*, uint64_t, const char*, ...);
    int      (*printf)(const char*, ...);
} libc_t;

// 0 — всё привязано; -1 — библиотека не открылась; -2 — не нашлось символа
static inline int libc_load(libc_t* c) {
    for (unsigned i = 0; i < sizeof(*c); i++) ((char*)c)[i] = 0;
    c->lib = ov_lib_open(LIBC_PATH);
    if (c->lib < 0) return -1;

    OV_BIND(c->lib, c->memcpy,  "memcpy");
    OV_BIND(c->lib, c->memset,  "memset");
    OV_BIND(c->lib, c->memmove, "memmove");
    OV_BIND(c->lib, c->memcmp,  "memcmp");
    OV_BIND(c->lib, c->memchr,  "memchr");
    OV_BIND(c->lib, c->strlen,  "strlen");
    OV_BIND(c->lib, c->strcpy,  "strcpy");
    OV_BIND(c->lib, c->strncpy, "strncpy");
    OV_BIND(c->lib, c->strlcpy, "strlcpy");
    OV_BIND(c->lib, c->strcat,  "strcat");
    OV_BIND(c->lib, c->strcmp,  "strcmp");
    OV_BIND(c->lib, c->strncmp, "strncmp");
    OV_BIND(c->lib, c->strchr,  "strchr");
    OV_BIND(c->lib, c->strrchr, "strrchr");
    OV_BIND(c->lib, c->strstr,  "strstr");
    OV_BIND(c->lib, c->isdigit, "isdigit");
    OV_BIND(c->lib, c->isalpha, "isalpha");
    OV_BIND(c->lib, c->isalnum, "isalnum");
    OV_BIND(c->lib, c->isspace, "isspace");
    OV_BIND(c->lib, c->toupper, "toupper");
    OV_BIND(c->lib, c->tolower, "tolower");
    OV_BIND(c->lib, c->abs,     "abs");
    OV_BIND(c->lib, c->strtol,  "strtol");
    OV_BIND(c->lib, c->atoi,    "atoi");
    OV_BIND(c->lib, c->itoa,    "itoa");
    OV_BIND(c->lib, c->srand,   "srand");
    OV_BIND(c->lib, c->rand,    "rand");
    OV_BIND(c->lib, c->malloc,  "malloc");
    OV_BIND(c->lib, c->free,    "free");
    OV_BIND(c->lib, c->calloc,  "calloc");
    OV_BIND(c->lib, c->realloc, "realloc");
    OV_BIND(c->lib, c->qsort,   "qsort");
    OV_BIND(c->lib, c->bsearch, "bsearch");
    OV_BIND(c->lib, c->snprintf, "snprintf");
    OV_BIND(c->lib, c->printf,  "printf");

    const void* const* p = (const void* const*)&c->memcpy;
    const void* const* end = (const void* const*)(&c->printf + 1);
    for (; p < end; p++) if (!*p) return -2;
    return 0;
}

static inline void libc_unload(libc_t* c) {
    if (c->lib >= 0) ov_lib_close(c->lib);
    c->lib = -1;
}

#endif
