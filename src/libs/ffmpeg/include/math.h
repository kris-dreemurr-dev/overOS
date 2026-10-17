#ifndef OV_MATH_H
#define OV_MATH_H
// math.h сисрута: объявлено только то, что действительно есть в libm overOS.
// Чего здесь нет, FFmpeg подставляет своими реализациями (libavutil/libm.h).
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440
#define M_E        2.7182818284590452354
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_LOG2_10  3.32192809488736234787
#define M_PHI      1.61803398874989484820
#define NAN        __builtin_nanf("")
#define INFINITY   __builtin_inff()
#define HUGE_VAL   __builtin_huge_val()
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(0,1,4,3,2,x)
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_NORMAL 2
#define FP_SUBNORMAL 3
#define FP_ZERO 4
double sin(double), cos(double), tan(double), asin(double), acos(double), atan(double);
double atan2(double, double), sinh(double), cosh(double), tanh(double);
double exp(double), exp2(double), expm1(double), log(double), log2(double);
double log10(double), log1p(double), pow(double, double), sqrt(double);
double cbrt(double), hypot(double, double), fmod(double, double);
double fabs(double), floor(double), ceil(double), round(double), trunc(double);
double fmin(double, double), fmax(double, double), copysign(double, double);
double frexp(double, int*), ldexp(double, int), scalbn(double, int);
double rint(double), nearbyint(double);
long   lrint(double);
long long llrint(double);
float  sinf(float), cosf(float), tanf(float), atanf(float), atan2f(float, float);
float  expf(float), logf(float), log2f(float), powf(float, float), sqrtf(float);
float  fabsf(float), floorf(float), ceilf(float), truncf(float), rintf(float);
float  fmodf(float, float), cbrtf(float), hypotf(float, float), roundf(float);
long   lrintf(float);
void   sincos(double, double*, double*);
void   sincosf(float, float*, float*);
#endif
