#include <stdint.h>

// Системные вызовы через int $0x80
static inline void sys_print(const char* str) {
    __asm__ volatile ("int $0x80" : : "a"(1), "b"(str), "c"(0x00FFFFFF) : "memory");
}

static inline void sys_put_pixel(int x, int y, uint32_t color) {
    __asm__ volatile ("int $0x80" : : "a"(2), "b"(x), "c"(y), "d"(color) : "memory");
}

static inline void sys_flush(void) {
    __asm__ volatile ("int $0x80" : : "a"(3), "b"(0), "c"(0), "d"(0) : "memory");
}

static inline void sys_sleep(uint32_t ms) {
    __asm__ volatile ("int $0x80" : : "a"(4), "b"(ms), "c"(0), "d"(0) : "memory");
}

static inline void sys_clear(uint32_t color) {
    __asm__ volatile ("int $0x80" : : "a"(5), "b"(color), "c"(0), "d"(0) : "memory");
}

static inline uint8_t sys_get_key(void) {
    uint8_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(6), "b"(0), "c"(0), "d"(0) : "memory");
    return ret;
}

// Таблица синусов (на 256 делений окружности), масштабированная в 1024 раза для точности
static const int16_t sintab[256] = {
    0, 25, 50, 75, 100, 125, 150, 175, 200, 225, 249, 274, 298, 322, 345, 369, 
    392, 414, 437, 459, 480, 501, 522, 542, 562, 581, 600, 618, 636, 653, 669, 685, 
    701, 716, 730, 744, 757, 770, 782, 793, 804, 814, 824, 833, 841, 849, 856, 862, 
    868, 873, 877, 881, 884, 887, 888, 889, 890, 890, 889, 888, 887, 884, 881, 877, 
    873, 868, 862, 856, 849, 841, 833, 824, 814, 804, 793, 782, 770, 757, 744, 730, 
    716, 701, 685, 669, 653, 636, 618, 600, 581, 562, 542, 522, 501, 480, 459, 437, 
    414, 392, 369, 345, 322, 298, 274, 249, 225, 200, 175, 150, 125, 100, 75, 50, 
    25, 0, -25, -50, -75, -100, -125, -150, -175, -200, -225, -249, -274, -298, -322, -345, 
    -369, -392, -414, -437, -459, -480, -501, -522, -542, -562, -581, -600, -618, -636, -653, -669, 
    -685, -701, -716, -730, -744, -757, -770, -782, -793, -804, -814, -824, -833, -841, -849, -856, 
    -862, -868, -873, -877, -881, -884, -887, -888, -889, -890, -890, -889, -888, -887, -884, -881, 
    -877, -873, -868, -862, -856, -849, -841, -833, -824, -814, -804, -793, -782, -770, -757, -744, 
    -730, -716, -701, -685, -669, -653, -636, -618, -600, -581, -562, -542, -522, -501, -480, -459, 
    -437, -414, -392, -369, -345, -322, -298, -274, -249, -225, -200, -175, -150, -125, -100, -75, 
    -50, -25, 0
};

static inline int get_sin(uint8_t angle) {
    return sintab[angle];
}

static inline int get_cos(uint8_t angle) {
    return sintab[(uint8_t)(angle + 64)]; // Косинус со сдвигом на 90 градусов (64 в угле из 256)
}

typedef struct {
    int x, y, z;
} vec3_int_t;

typedef struct {
    int x, y;
} vec2_int_t;

// 8 вершин куба
static vec3_int_t cube_vertices[8] = {
    {-30, -30, -30},
    { 30, -30, -30},
    { 30,  30, -30},
    {-30,  30, -30},
    {-30, -30,  30},
    { 30, -30,  30},
    { 30,  30,  30},
    {-30,  30,  30}
};

// 12 ребер куба
static int cube_edges[12][2] = {
    {0,1}, {1,2}, {2,3}, {3,0},
    {4,5}, {5,6}, {6,7}, {7,4},
    {0,4}, {1,5}, {2,6}, {3,7}
};

// Алгоритм Брезенхема для линий
void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        sys_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

int main(void) {
    sys_print(">>> Starting Integer 3D Wireframe Engine on devOS...\n");
    sys_sleep(800);

    uint8_t angle_x = 0;
    uint8_t angle_y = 0;

    int center_x = 160; 
    int center_y = 100; 
    int camera_dist = 150;

    while (1) {
        if (sys_get_key() == 0x01) break; // Выход по ESC

        sys_clear(0x000000); 

        vec2_int_t projected[8];
        int sin_y = get_sin(angle_y);
        int cos_y = get_cos(angle_y);
        int sin_x = get_sin(angle_x);
        int cos_x = get_cos(angle_x);

        // 1. Вращение и проекция вершин (только целые числа)
        for (int i = 0; i < 8; i++) {
            vec3_int_t v = cube_vertices[i];

            // Вращение по оси Y
            int x1 = (v.x * cos_y - v.z * sin_y) / 1024;
            int y1 = v.y;
            int z1 = (v.x * sin_y + v.z * cos_y) / 1024;

            // Вращение по оси X
            int x2 = x1;
            int y2 = (y1 * cos_x - z1 * sin_x) / 1024;
            int z2 = (y1 * sin_x + z1 * cos_x) / 1024;

            // Смещение по глубине
            int depth = z2 + camera_dist;
            if (depth < 1) depth = 1;

            // Проекция 3D -> 2D
            projected[i].x = center_x + (x2 * 150) / depth;
            projected[i].y = center_y + (y2 * 150) / depth;
        }

        // 2. Рисуем ребра куба
        for (int i = 0; i < 12; i++) {
            int p1 = cube_edges[i][0];
            int p2 = cube_edges[i][1];
            draw_line(projected[p1].x, projected[p1].y, projected[p2].x, projected[p2].y, 0x00FF55);
        }

        sys_flush();

        angle_x += 2;
        angle_y += 3;

        sys_sleep(30);
    }

    sys_clear(0x000000);
    sys_flush();
    sys_print(">>> 3D Engine exited successfully.\n");
    return 0;
}