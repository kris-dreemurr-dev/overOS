// Куча движка под настоящим аллокатором cxxrt (арена LL_ARENA_KB): окно,
// динамическое разрешение туда-сюда, развёртывание. Печатает занятое и
// самый большой свободный кусок — то, от чего зависит код 12
#include <stdio.h>
#include "engine/renderer.h"
#include <stddef.h>
extern "C" size_t ov_heap_used(void);
extern "C" size_t ov_heap_largest_free(void);
uint64_t bench_tsc_per_ms = 1000000;
extern "C" void ov_exit(int c) { printf("!!! ov_exit(%d): нехватка памяти\n", c); fflush(stdout); __builtin_trap(); }
int main() {
    ov::Rasterizer* r = new ov::Rasterizer;
    r->startThreads(0);
    uint32_t* fb = new uint32_t[1920 * 1080];
    const int sizes[][2] = { { 1024, 640 }, { 1600, 860 }, { 1024, 640 }, { 1920, 1000 }, { 1600, 860 } };
    for (int s = 0; s < 5; s++) {
        int W = sizes[s][0], H = sizes[s][1];
        for (int f = 0; f < 200; f++) {
            int pct = 55 + (f % 46);                 // разрешение растёт по проценту за кадр
            r->setTarget(fb, W, H, pct);
            ov::Camera cam;
            r->begin(cam);
            r->end();
        }
        printf("%dx%d: занято %.1f МБ, свободный кусок %.1f МБ из %d МБ\n", W, H, ov_heap_used() / 1048576.0,
               ov_heap_largest_free() / 1048576.0, CXXRT_ARENA_KB / 1024);
    }
}
