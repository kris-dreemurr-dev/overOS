#ifndef OVENGINE_MATH3D_H
#define OVENGINE_MATH3D_H
// =============================================================================
// math3d.h — векторы и матрицы движка overOS.
//
// Своя математика, потому что libm у программ нет: sqrt — одна инструкция
// SSE, sin — многочлен после свёртки угла в [-pi/2, pi/2] (ошибка < 4e-6,
// для поворотов на экране этого с запасом).
//
// Соглашения как в OpenGL: правая система координат, камера смотрит вдоль
// -Z, матрицы применяются к вектору-столбцу (v' = M * v), хранятся по
// строкам: m[строка * 4 + столбец].
// =============================================================================

namespace ov {

constexpr float PI = 3.14159265358979f;

inline float sqrt(float x) {
    float r;
    __asm__ ("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

inline float fabs(float x) { return x < 0.0f ? -x : x; }
inline float fmin(float a, float b) { return a < b ? a : b; }
inline float fmax(float a, float b) { return a > b ? a : b; }
inline float clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline float floor(float x) {
    int i = (int)x;
    return (float)(i - (x < (float)i ? 1 : 0));
}

inline float sin(float x) {
    const float TWO_PI = 2.0f * PI;
    x -= floor(x / TWO_PI + 0.5f) * TWO_PI;              // в [-pi, pi]
    if (x > PI * 0.5f) x = PI - x;                         // sin(pi - x) = sin(x)
    else if (x < -PI * 0.5f) x = -PI - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
           x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}
inline float cos(float x) { return sin(x + PI * 0.5f); }
inline float tan(float x) { return sin(x) / cos(x); }
inline float radians(float deg) { return deg * (PI / 180.0f); }
// atan: многочлен на [-1, 1] (ошибка ~1e-5), снаружи — через 1/x
inline float atan(float x) {
    bool inv = fabs(x) > 1.0f;
    if (inv) x = 1.0f / x;
    float x2 = x * x;
    float r = x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
    if (inv) r = (x > 0.0f ? PI * 0.5f : -PI * 0.5f) - r;
    return r;
}
inline float atan2(float y, float x) {
    if (x > 0.0f) return atan(y / x);
    if (x < 0.0f) return atan(y / x) + (y >= 0.0f ? PI : -PI);
    return y > 0.0f ? PI * 0.5f : (y < 0.0f ? -PI * 0.5f : 0.0f);
}

// ------------------------------------------------------------------ векторы
struct Vec2 {
    float x, y;
    constexpr Vec2() : x(0), y(0) {}
    constexpr Vec2(float a, float b) : x(a), y(b) {}
    Vec2 operator+(Vec2 o) const { return Vec2(x + o.x, y + o.y); }
    Vec2 operator-(Vec2 o) const { return Vec2(x - o.x, y - o.y); }
    Vec2 operator*(float k) const { return Vec2(x * k, y * k); }
};

struct Vec3 {
    float x, y, z;
    constexpr Vec3() : x(0), y(0), z(0) {}
    constexpr Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator-() const { return Vec3(-x, -y, -z); }
    Vec3 operator*(float k) const { return Vec3(x * k, y * k, z * k); }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float length(const Vec3& v) { return sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 0.0f ? v * (1.0f / l) : v;
}

struct Vec4 {
    float x, y, z, w;
    constexpr Vec4() : x(0), y(0), z(0), w(0) {}
    constexpr Vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    constexpr Vec4(const Vec3& v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
    Vec4 operator+(const Vec4& o) const { return Vec4(x + o.x, y + o.y, z + o.z, w + o.w); }
    Vec4 operator-(const Vec4& o) const { return Vec4(x - o.x, y - o.y, z - o.z, w - o.w); }
    Vec4 operator*(float k) const { return Vec4(x * k, y * k, z * k, w * k); }
};

// ------------------------------------------------------------------ матрица
struct Mat4 {
    float m[16];

    static Mat4 identity() {
        Mat4 r;
        for (int i = 0; i < 16; i++) r.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return r;
    }
    static Mat4 translate(const Vec3& t) {
        Mat4 r = identity();
        r.m[3] = t.x; r.m[7] = t.y; r.m[11] = t.z;
        return r;
    }
    static Mat4 scale(float k) {
        Mat4 r = identity();
        r.m[0] = r.m[5] = r.m[10] = k;
        return r;
    }
    static Mat4 scale(const Vec3& k) {
        Mat4 r = identity();
        r.m[0] = k.x; r.m[5] = k.y; r.m[10] = k.z;
        return r;
    }
    static Mat4 rotateX(float a) {
        Mat4 r = identity();
        float c = cos(a), s = sin(a);
        r.m[5] = c; r.m[6] = -s;
        r.m[9] = s; r.m[10] = c;
        return r;
    }
    static Mat4 rotateY(float a) {
        Mat4 r = identity();
        float c = cos(a), s = sin(a);
        r.m[0] = c;  r.m[2] = s;
        r.m[8] = -s; r.m[10] = c;
        return r;
    }
    static Mat4 rotateZ(float a) {
        Mat4 r = identity();
        float c = cos(a), s = sin(a);
        r.m[0] = c; r.m[1] = -s;
        r.m[4] = s; r.m[5] = c;
        return r;
    }
    // Перспектива как gluPerspective: fovy — вертикальный угол в радианах
    static Mat4 perspective(float fovy, float aspect, float znear, float zfar) {
        Mat4 r;
        for (int i = 0; i < 16; i++) r.m[i] = 0.0f;
        float f = 1.0f / tan(fovy * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zfar + znear) / (znear - zfar);
        r.m[11] = 2.0f * zfar * znear / (znear - zfar);
        r.m[14] = -1.0f;
        return r;
    }
    // Камера в eye смотрит на target
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = normalize(target - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r = identity();
        r.m[0] = s.x;  r.m[1] = s.y;  r.m[2] = s.z;   r.m[3] = -dot(s, eye);
        r.m[4] = u.x;  r.m[5] = u.y;  r.m[6] = u.z;   r.m[7] = -dot(u, eye);
        r.m[8] = -f.x; r.m[9] = -f.y; r.m[10] = -f.z; r.m[11] = dot(f, eye);
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                float s = 0.0f;
                for (int k = 0; k < 4; k++) s += m[i * 4 + k] * o.m[k * 4 + j];
                r.m[i * 4 + j] = s;
            }
        return r;
    }
    Vec4 operator*(const Vec4& v) const {
        return Vec4(m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * v.w,
                    m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7] * v.w,
                    m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11] * v.w,
                    m[12] * v.x + m[13] * v.y + m[14] * v.z + m[15] * v.w);
    }
    // Поворот направления (без переноса): для нормалей при повороте и
    // равномерном масштабе этого достаточно
    Vec3 rotate(const Vec3& v) const {
        return Vec3(m[0] * v.x + m[1] * v.y + m[2] * v.z,
                    m[4] * v.x + m[5] * v.y + m[6] * v.z,
                    m[8] * v.x + m[9] * v.y + m[10] * v.z);
    }
};

} // namespace ov

#endif
