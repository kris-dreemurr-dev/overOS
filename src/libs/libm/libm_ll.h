#ifndef LIBM_LL_H
#define LIBM_LL_H
// libm_ll.h — удобная привязка к /lib/libm.ll (разделяемая математика overOS).
//
//     libm_t m;
//     if (libm_load(&m) < 0) { /* библиотеки нет */ }
//     double y = m.sin(x) * m.exp(-x);
//     libm_unload(&m);
//
// Каждое поле — адрес функции из библиотеки, полученный ov_lib_sym. Можно и
// напрямую: ov_lib_open("/lib/libm.ll") + ov_lib_sym(h, "sqrt").

#include "ovos.h"

#define LIBM_PATH "/lib/libm.ll"

typedef struct {
    int lib;                       // дескриптор .ll (>= 0)

    double (*fabs)(double);
    double (*copysign)(double, double);
    double (*floor)(double);
    double (*ceil)(double);
    double (*trunc)(double);
    double (*round)(double);
    double (*fmod)(double, double);
    double (*sqrt)(double);
    double (*hypot)(double, double);
    double (*ldexp)(double, int);
    double (*frexp)(double, int*);
    double (*log)(double);
    double (*log2)(double);
    double (*log10)(double);
    double (*exp)(double);
    double (*exp2)(double);
    double (*expm1)(double);
    double (*log1p)(double);
    double (*pow)(double, double);
    double (*sin)(double);
    double (*cos)(double);
    double (*tan)(double);
    double (*asin)(double);
    double (*acos)(double);
    double (*atan)(double);
    double (*atan2)(double, double);
    double (*sinh)(double);
    double (*cosh)(double);
    double (*tanh)(double);
    double (*fmin)(double, double);
    double (*fmax)(double, double);

    const double* pi;              // данные тоже экспортируются
    const double* e;
} libm_t;

// 0 — всё привязано; -1 — библиотека не открылась; -2 — не нашлось символа
static inline int libm_load(libm_t* m) {
    for (unsigned i = 0; i < sizeof(*m); i++) ((char*)m)[i] = 0;
    m->lib = ov_lib_open(LIBM_PATH);
    if (m->lib < 0) return -1;

    OV_BIND(m->lib, m->fabs,     "fabs");
    OV_BIND(m->lib, m->copysign, "copysign");
    OV_BIND(m->lib, m->floor,    "floor");
    OV_BIND(m->lib, m->ceil,     "ceil");
    OV_BIND(m->lib, m->trunc,    "trunc");
    OV_BIND(m->lib, m->round,    "round");
    OV_BIND(m->lib, m->fmod,     "fmod");
    OV_BIND(m->lib, m->sqrt,     "sqrt");
    OV_BIND(m->lib, m->hypot,    "hypot");
    OV_BIND(m->lib, m->ldexp,    "ldexp");
    OV_BIND(m->lib, m->frexp,    "frexp");
    OV_BIND(m->lib, m->log,      "log");
    OV_BIND(m->lib, m->log2,     "log2");
    OV_BIND(m->lib, m->log10,    "log10");
    OV_BIND(m->lib, m->exp,      "exp");
    OV_BIND(m->lib, m->exp2,     "exp2");
    OV_BIND(m->lib, m->expm1,    "expm1");
    OV_BIND(m->lib, m->log1p,    "log1p");
    OV_BIND(m->lib, m->pow,      "pow");
    OV_BIND(m->lib, m->sin,      "sin");
    OV_BIND(m->lib, m->cos,      "cos");
    OV_BIND(m->lib, m->tan,      "tan");
    OV_BIND(m->lib, m->asin,     "asin");
    OV_BIND(m->lib, m->acos,     "acos");
    OV_BIND(m->lib, m->atan,     "atan");
    OV_BIND(m->lib, m->atan2,    "atan2");
    OV_BIND(m->lib, m->sinh,     "sinh");
    OV_BIND(m->lib, m->cosh,     "cosh");
    OV_BIND(m->lib, m->tanh,     "tanh");
    OV_BIND(m->lib, m->fmin,     "fmin");
    OV_BIND(m->lib, m->fmax,     "fmax");
    OV_BIND(m->lib, m->pi,       "libm_pi");
    OV_BIND(m->lib, m->e,        "libm_e");

    const void* const* p = (const void* const*)&m->fabs;
    const void* const* end = (const void* const*)(&m->e + 1);
    for (; p < end; p++) if (!*p) return -2;
    return 0;
}

static inline void libm_unload(libm_t* m) {
    if (m->lib >= 0) ov_lib_close(m->lib);
    m->lib = -1;
}

#endif
