#ifndef OVCRAFT_NOISE_H
#define OVCRAFT_NOISE_H
// =============================================================================
// noise.h — шум для рельефа: хеш координат, гладкий шум значений 2D/3D и
// сумма октав (fbm). Без таблиц и без памяти: одна и та же точка мира всегда
// даёт одно и то же число, в каком бы порядке и на каком ядре ни считались
// чанки. Это и делает мир процедурным — он «записан» в seed.
// =============================================================================

#include <stdint.h>
#include "engine/math3d.h"

namespace noise {

inline uint32_t hash(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed ^ 0x9E3779B9u;
    h ^= (uint32_t)x * 0x85EBCA6Bu; h = (h << 13) | (h >> 19);
    h ^= (uint32_t)y * 0xC2B2AE35u; h = (h << 13) | (h >> 19);
    h ^= (uint32_t)z * 0x27D4EB2Fu;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

inline float hashf(int x, int y, int z, uint32_t seed) {     // 0..1
    return (float)(hash(x, y, z, seed) & 0xFFFFFF) / 16777215.0f;
}

inline float smooth(float t) { return t * t * (3.0f - 2.0f * t); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline int ifloor(float v) { int i = (int)v; return (v < (float)i) ? i - 1 : i; }

// Шум значений 2D: 0..1
inline float value2(float x, float y, uint32_t seed) {
    int xi = ifloor(x), yi = ifloor(y);
    float tx = smooth(x - (float)xi), ty = smooth(y - (float)yi);
    float a = hashf(xi, yi, 0, seed), b = hashf(xi + 1, yi, 0, seed);
    float c = hashf(xi, yi + 1, 0, seed), d = hashf(xi + 1, yi + 1, 0, seed);
    return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

// Шум значений 3D: 0..1 (пещеры)
inline float value3(float x, float y, float z, uint32_t seed) {
    int xi = ifloor(x), yi = ifloor(y), zi = ifloor(z);
    float tx = smooth(x - (float)xi), ty = smooth(y - (float)yi), tz = smooth(z - (float)zi);
    float c[2][2][2];
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) c[k][j][i] = hashf(xi + i, yi + j, zi + k, seed);
    float x00 = lerp(c[0][0][0], c[0][0][1], tx), x10 = lerp(c[0][1][0], c[0][1][1], tx);
    float x01 = lerp(c[1][0][0], c[1][0][1], tx), x11 = lerp(c[1][1][0], c[1][1][1], tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

// Сумма октав: крупные холмы + мелкая неровность. Результат 0..1
inline float fbm2(float x, float y, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; o++) {
        sum += value2(x, y, seed + (uint32_t)o * 101u) * amp;
        norm += amp;
        amp *= 0.5f;
        x *= 2.0f;
        y *= 2.0f;
    }
    return sum / norm;
}

} // namespace noise

#endif
