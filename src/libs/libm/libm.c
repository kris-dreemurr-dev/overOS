// libm.ll — математическая библиотека overOS, разделяемая (.ll).
//
// Собирается как перемещаемый образ без libc и без таблиц импорта: всё, что
// нужно, посчитано здесь. Программа грузит её ov_lib_open("/lib/libm.ll") и
// берёт функции по именам (см. src/lib/libm_ll.h).
//
// Точность: двойная, вычисления в double на SSE. Аргументы приводятся к
// рабочему диапазону, дальше работают минимаксные многочлены и ряды. Значения
// проверяются регрессией src/programs/lltest.c против таблицы эталонов.

#include <stdint.h>

#define M_PI_C   3.14159265358979323846
#define M_PI2_C  1.57079632679489661923
#define M_LN2_C  0.69314718055994530942
#define M_LOG2E  1.44269504088896340736
#define INF_D    (1.0e308 * 10.0)

static int is_nan(double x) { return x != x; }

// ---------------------------------------------------------------- базовое
double fabs(double x) { return x < 0 ? -x : x; }

double copysign(double x, double y) {
    double a = fabs(x);
    return (y < 0) ? -a : a;
}

double floor(double x) {
    if (is_nan(x) || fabs(x) >= 4.5035996273704960e15) return x;   // уже целое
    double t = (double)(int64_t)x;
    return (t > x) ? t - 1.0 : t;
}

double ceil(double x) {
    if (is_nan(x) || fabs(x) >= 4.5035996273704960e15) return x;
    double t = (double)(int64_t)x;
    return (t < x) ? t + 1.0 : t;
}

double trunc(double x) {
    if (is_nan(x) || fabs(x) >= 4.5035996273704960e15) return x;
    return (double)(int64_t)x;
}

double round(double x) {
    return (x < 0) ? ceil(x - 0.5) : floor(x + 0.5);
}

double fmod(double x, double y) {
    if (y == 0.0 || is_nan(x) || is_nan(y)) return x * 0.0 / (y * 0.0);   // NaN
    double q = trunc(x / y);
    double r = x - q * y;
    return r;
}

double sqrt(double x) {
    if (x < 0.0) return -INF_D * 0.0;        // NaN
    if (x == 0.0 || is_nan(x)) return x;
    double r;
    __asm__ volatile ("sqrtsd %1, %0" : "=x"(r) : "x"(x));   // SSE2 даёт точный результат
    return r;
}

double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}

// ---------------------------------------------------------------- экспонента и логарифм
// frexp/ldexp через разбор бит: умножать на 2 в цикле дорого и теряет точность
double ldexp(double x, int n) {
    if (x == 0.0 || is_nan(x)) return x;
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7FF);
    if (e == 0) {                                   // денормал: нормализуем
        v.d *= 18014398509481984.0;                 // 2^54
        e = (int)((v.u >> 52) & 0x7FF) - 54;
    }
    e += n;
    if (e >= 0x7FF) return (x < 0 ? -INF_D : INF_D);
    if (e <= 0) return 0.0;
    v.u = (v.u & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)e << 52);
    return v.d;
}

double frexp(double x, int* exp_out) {
    if (x == 0.0 || is_nan(x)) { if (exp_out) *exp_out = 0; return x; }
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7FF);
    if (e == 0) {
        v.d *= 18014398509481984.0;
        e = (int)((v.u >> 52) & 0x7FF) - 54;
    }
    if (exp_out) *exp_out = e - 1022;
    v.u = (v.u & 0x800FFFFFFFFFFFFFULL) | (1022ULL << 52);
    return v.d;
}

// log(x) = 2*atanh(s), s = (m-1)/(m+1) после выделения степени двойки
double log(double x) {
    if (x < 0.0) return -INF_D * 0.0;
    if (x == 0.0) return -INF_D;
    if (is_nan(x)) return x;
    int e;
    double m = frexp(x, &e);
    if (m < 0.70710678118654752440) { m *= 2.0; e--; }   // приводим к [1/sqrt2, sqrt2)
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    // ряд atanh: s + s^3/3 + s^5/5 + ... (|s| < 0.1716, хватает до s^15)
    double sum = s2 * (1.0 / 3.0 + s2 * (1.0 / 5.0 + s2 * (1.0 / 7.0 + s2 * (1.0 / 9.0 +
                 s2 * (1.0 / 11.0 + s2 * (1.0 / 13.0 + s2 / 15.0))))));
    return 2.0 * (s + s * sum) + (double)e * M_LN2_C;
}

double log2(double x)  { return log(x) * M_LOG2E; }
double log10(double x) { return log(x) * 0.43429448190325182765; }

double exp(double x) {
    if (is_nan(x)) return x;
    if (x > 709.78) return INF_D;
    if (x < -745.2) return 0.0;
    // x = k*ln2 + r, |r| <= ln2/2; exp(r) считаем многочленом, 2^k — через ldexp
    double kd = floor(x * M_LOG2E + 0.5);
    int k = (int)kd;
    double r = x - kd * M_LN2_C;
    // Ряд Тейлора до r^12/12!: коэффициент k-го члена -- 1/k!, а не 1/(k-1)!
    double p = 1.0 + r * (1.0 + r * (1.0 / 2.0 + r * (1.0 / 6.0 + r * (1.0 / 24.0 +
               r * (1.0 / 120.0 + r * (1.0 / 720.0 + r * (1.0 / 5040.0 +
               r * (1.0 / 40320.0 + r * (1.0 / 362880.0 + r * (1.0 / 3628800.0 +
               r * (1.0 / 39916800.0 + r / 479001600.0)))))))))));
    return ldexp(p, k);
}

double exp2(double x) { return exp(x * M_LN2_C); }

// exp(x)-1 без потери разрядов около нуля (там exp(x) ~ 1 и разность съедает точность)
double expm1(double x) {
    if (fabs(x) < 0.5) {
        return x * (1.0 + x * (1.0 / 2.0 + x * (1.0 / 6.0 + x * (1.0 / 24.0 +
               x * (1.0 / 120.0 + x * (1.0 / 720.0 + x * (1.0 / 5040.0 +
               x * (1.0 / 40320.0 + x * (1.0 / 362880.0 + x * (1.0 / 3628800.0 +
               x * (1.0 / 39916800.0 + x * (1.0 / 479001600.0 +
               x * (1.0 / 6227020800.0 + x / 87178291200.0)))))))))))));
    }
    return exp(x) - 1.0;
}

// log(1+x) с той же оговоркой
double log1p(double x) {
    if (fabs(x) < 1e-4) return x * (1.0 - x * (0.5 - x * (1.0 / 3.0 - x * 0.25)));
    return log(1.0 + x);
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return (y > 0.0) ? 0.0 : INF_D;
    if (is_nan(x) || is_nan(y)) return x + y;
    if (x > 0.0) return exp(y * log(x));
    // отрицательное основание допустимо только с целой степенью
    if (y != trunc(y)) return -INF_D * 0.0;
    double r = exp(y * log(-x));
    return (fmod(fabs(y), 2.0) == 1.0) ? -r : r;
}

// ---------------------------------------------------------------- тригонометрия
// Приведение аргумента по модулю pi/2 с запасом точности (двойное представление pi)
static double reduce_pi2(double x, int* quad) {
    double k = floor(x / M_PI2_C + 0.5);
    *quad = (int)((int64_t)k & 3);
    // pi/2 разложена на две части, чтобы не терять разряды на больших x
    const double P1 = 1.57079632673412561417;
    const double P2 = 6.07710050630396597660e-11;
    const double P3 = 2.02226624879595063154e-21;
    return ((x - k * P1) - k * P2) - k * P3;
}

static double sin_core(double r) {      // |r| <= pi/4
    double r2 = r * r;
    return r + r * r2 * (-1.0 / 6.0 + r2 * (1.0 / 120.0 + r2 * (-1.0 / 5040.0 +
           r2 * (1.0 / 362880.0 + r2 * (-1.0 / 39916800.0 + r2 * (1.0 / 6227020800.0 +
           r2 * (-1.0 / 1307674368000.0)))))));
}

static double cos_core(double r) {      // |r| <= pi/4
    double r2 = r * r;
    return 1.0 + r2 * (-1.0 / 2.0 + r2 * (1.0 / 24.0 + r2 * (-1.0 / 720.0 +
           r2 * (1.0 / 40320.0 + r2 * (-1.0 / 3628800.0 + r2 * (1.0 / 479001600.0 +
           r2 * (-1.0 / 87178291200.0)))))));
}

double sin(double x) {
    if (is_nan(x)) return x;
    int q;
    double r = reduce_pi2(x, &q);
    switch (q) {
        case 0:  return sin_core(r);
        case 1:  return cos_core(r);
        case 2:  return -sin_core(r);
        default: return -cos_core(r);
    }
}

double cos(double x) {
    if (is_nan(x)) return x;
    int q;
    double r = reduce_pi2(x, &q);
    switch (q) {
        case 0:  return cos_core(r);
        case 1:  return -sin_core(r);
        case 2:  return -cos_core(r);
        default: return sin_core(r);
    }
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return INF_D;
    return sin(x) / c;
}

double atan(double x) {
    if (is_nan(x)) return x;
    int neg = x < 0.0;
    double a = fabs(x);
    int inv = 0;
    if (a > 1.0) { a = 1.0 / a; inv = 1; }
    // atan(a) = atan(T) + atan((a-T)/(1+a*T)). Три узла вместо одного: после
    // приведения |a| <= 0.2, и ряд сходится на порядки быстрее (было 5e-9).
    double shift = 0.0;
    if (a > 0.75) {
        shift = 0.78539816339744827900;         // atan(1) = pi/4
        a = (a - 1.0) / (1.0 + a);
    } else if (a > 0.375) {
        shift = 0.46364760900080611621;         // atan(0.5)
        a = (a - 0.5) / (1.0 + 0.5 * a);
    } else if (a > 0.125) {
        shift = 0.24497866312686415417;         // atan(0.25)
        a = (a - 0.25) / (1.0 + 0.25 * a);
    }
    double a2 = a * a;
    double p = a * (1.0 + a2 * (-1.0 / 3.0 + a2 * (1.0 / 5.0 + a2 * (-1.0 / 7.0 +
               a2 * (1.0 / 9.0 + a2 * (-1.0 / 11.0 + a2 * (1.0 / 13.0 +
               a2 * (-1.0 / 15.0 + a2 * (1.0 / 17.0 + a2 * (-1.0 / 19.0 +
               a2 / 21.0))))))))));
    double r = p + shift;
    if (inv) r = M_PI2_C - r;
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x == 0.0 && y == 0.0) return 0.0;
    if (x == 0.0) return (y > 0.0) ? M_PI2_C : -M_PI2_C;
    double a = atan(y / x);
    if (x > 0.0) return a;
    return (y >= 0.0) ? a + M_PI_C : a - M_PI_C;
}

double asin(double x) {
    if (x > 1.0 || x < -1.0) return -INF_D * 0.0;
    if (x == 1.0) return M_PI2_C;
    if (x == -1.0) return -M_PI2_C;
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x > 1.0 || x < -1.0) return -INF_D * 0.0;
    // Около +-1 разность pi/2 - asin(x) теряет разряды, поэтому там считаем через atan
    if (x > 0.5)  return 2.0 * atan(sqrt((1.0 - x) / (1.0 + x)));
    if (x < -0.5) return M_PI_C - 2.0 * atan(sqrt((1.0 + x) / (1.0 - x)));
    return M_PI2_C - asin(x);
}

double sinh(double x) { double e = exp(x); return (e - 1.0 / e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) * 0.5; }

double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e = expm1(2.0 * x);      // около нуля (e-1)/(e+1) теряло почти все разряды
    return e / (e + 2.0);
}

double fmin(double a, double b) { return (is_nan(a) || b < a) ? b : a; }
double fmax(double a, double b) { return (is_nan(a) || b > a) ? b : a; }

// Версии для float: приложению бывает удобнее, а точности хватает
float sqrtf(float x) { return (float)sqrt((double)x); }
float sinf(float x)  { return (float)sin((double)x); }
float cosf(float x)  { return (float)cos((double)x); }
float tanf(float x)  { return (float)tan((double)x); }
float expf(float x)  { return (float)exp((double)x); }
float logf(float x)  { return (float)log((double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float fabsf(float x) { return x < 0 ? -x : x; }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x)  { return (float)ceil((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }

// Константы библиотеки видны как обычные данные (экспортируются так же, как функции)
const double libm_pi = M_PI_C;
const double libm_e  = 2.71828182845904523536;
