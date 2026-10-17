// Стенд для замеров движка overCraft на основной машине: см. run.sh рядом
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "engine/renderer.h"
#include "world/chunk.h"
#include "world/generator.h"
#include "world/mesher.h"
#include "blocks.h"
#include "libgfx/png.h"
uint64_t bench_tsc_per_ms = 0;
static uint64_t rd() { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
int main(int argc, char** argv) {
    timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); uint64_t t0 = rd();
    timespec s = {0, 100000000}; nanosleep(&s, 0);
    clock_gettime(CLOCK_MONOTONIC, &b); uint64_t t1 = rd();
    bench_tsc_per_ms = (t1 - t0) * 1000000ull / (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
    const int R = 6, D = 2 * R + 1;
    Chunk* ch = new Chunk[D * D];
    for (int z = 0; z < D; z++) for (int x = 0; x < D; x++) { Chunk& c = ch[z * D + x]; c.cx = x - R; c.cz = z - R; gen::fill(c, 0, 0); }
    if (getenv("FINDWATER")) { for (int z = 0; z < D * 16; z += 4) for (int x = 0; x < D * 16; x += 4) {
            const Chunk& c = ch[(z / 16) * D + x / 16]; if (c.get(x % 16, cfg::SEA_LEVEL - 1, z % 16) == WATER) printf("water %d %d\n", c.cx * 16 + x % 16, c.cz * 16 + z % 16); } return 0; }
    // GLASS=x,y,z — стеклянная стенка 5x4 (проверка прозрачности)
    if (getenv("GLASS")) { int gx, gy, gz; sscanf(getenv("GLASS"), "%d,%d,%d", &gx, &gy, &gz);
        Chunk& c = ch[R * D + R];
        for (int dy = 0; dy < 4; dy++) for (int dx = 0; dx < 5; dx++) c.set(gx + dx, gy + dy, gz, GLASS); }
    MeshScratch* sc = new MeshScratch;
    for (int z = 1; z < D - 1; z++) for (int x = 1; x < D - 1; x++) {
        const Chunk* nb[3][3];
        for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) nb[dz + 1][dx + 1] = &ch[(z + dz) * D + x + dx];
        for (int p = 0; p < 2; p++) {
            mesher::build(ch[z * D + x], nb, *sc);
            for (int i = 0; i < cfg::SECTIONS; i++) { SectionMesh& m = ch[z * D + x].sec[i];
                if (m.dirty && (m.need_v > m.cap_v || m.need_f > m.cap_f)) m.grow(m.need_v + 64, m.need_f + 32); }
        }
    }
    ov::Rasterizer r;
    r.startThreads(0);
    // Настоящие текстуры игры (тем же декодером, что в движке); без файла — шум
    for (int i = 0; i < TEX_COUNT; i++) {
        uint32_t* px = new uint32_t[256]; int w = 16, h = 16;
        char path[256]; snprintf(path, sizeof path, "%s/%s", OVCRAFT_DIR, TEXTURES[i]);
        static uint8_t file[1 << 16], scratch[1 << 16]; FILE* tf = fopen(path, "rb"); size_t n = tf ? fread(file, 1, sizeof file, tf) : 0;
        if (tf) fclose(tf);
        png_info_t info;
        bool ok = n > 33 && png_be32(file + 16) == 16 && png_be32(file + 20) == 16 && png_decode(file, (uint32_t)n, px, 256, scratch, sizeof scratch, &info) == PNG_OK;
        if (!ok) for (int k = 0; k < 256; k++) px[k] = rand() & 0xFFFFFF;
        if (i == T_WATER && getenv("NOWATER")) for (int k = 0; k < 256; k++) px[k] &= 0xFFFFFF;   // дно без воды
        r.addTexture(w, h, px, ok);
    }
    int W = argc > 1 ? atoi(argv[1]) : 1024, H = argc > 2 ? atoi(argv[2]) : 640;
    uint32_t* fb = new uint32_t[W * H];
    r.setTarget(fb, W, H, argc > 4 ? atoi(argv[4]) : 100);
    float yaw = getenv("YAW") ? atof(getenv("YAW")) : 0.84f, pitch = getenv("PITCH") ? atof(getenv("PITCH")) : 0.0f;
    ov::Vec3 eye(12.7f, 47.62f, 5.3f);
    if (getenv("EYE")) sscanf(getenv("EYE"), "%f,%f,%f", &eye.x, &eye.y, &eye.z);
    ov::Vec3 fwd(ov::sin(yaw) * ov::cos(pitch), ov::sin(pitch), -ov::cos(yaw) * ov::cos(pitch));
    ov::Camera cam;
    cam.proj = ov::Mat4::perspective(ov::radians(70.0f), (float)W / H, 0.08f, 400.0f);
    cam.view = ov::Mat4::lookAt(eye, eye + fwd, ov::Vec3(0, 1, 0));
    cam.light.ambient = 1.0f; cam.fog_start = 4.5f * 16; cam.fog_end = 5.8f * 16;
    double geo = 0, ras = 0; int N = 60; ov::FrameStats st;
    for (int it = 0; it < N; it++) {
        r.begin(cam);
        static int ord[400][2]; static int no = 0;
        if (!no) { for (int z = 1; z < D - 1; z++) for (int x = 1; x < D - 1; x++) { ord[no][0] = x; ord[no][1] = z; no++; }
            for (int i = 1; i < no; i++) for (int j = i; j > 0; j--) { int ax = ord[j][0] - R - 0, az = ord[j][1] - R, bx = ord[j-1][0] - R, bz = ord[j-1][1] - R;
                if (ax*ax+az*az < bx*bx+bz*bz) { int t0 = ord[j][0], t1 = ord[j][1]; ord[j][0] = ord[j-1][0]; ord[j][1] = ord[j-1][1]; ord[j-1][0] = t0; ord[j-1][1] = t1; } else break; } }
        for (int k = 0; k < no; k++) { int x = ord[k][0], z = ord[k][1];
            Chunk& c = ch[z * D + x];
            ov::Mat4 m = ov::Mat4::translate(ov::Vec3(c.cx * 16.0f, 0, c.cz * 16.0f));
            for (int i = 0; i < cfg::SECTIONS; i++) {
                float x0 = c.cx * 16.0f, z0 = c.cz * 16.0f, y0 = i * 16.0f;
                const bool vis[6] = { eye.x > x0, eye.x < x0 + 16, eye.y > y0, eye.y < y0 + 16, eye.z > z0, eye.z < z0 + 16 };
                for (int d = 0; d < 6; d++) if (vis[d] && c.sec[i].dir[d].nf) r.draw(&c.sec[i].dir[d], m);
            }
        }
        r.end();
        st = r.stats(); geo += st.ms_geometry; ras += st.ms_raster;
    }
    printf("%dx%d: tris %d of %d, geometry %.2f ms, raster %.2f ms (1 thread)\n", W, H, st.drawn, st.submitted, geo / N, ras / N);
    FILE* f = fopen(argc > 3 ? argv[3] : "/dev/null", "wb");
    if (f) { fprintf(f, "P6 %d %d 255\n", W, H); for (int i = 0; i < W * H; i++) { unsigned char p[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] }; fwrite(p, 1, 3, f); } fclose(f); }
}
