// Имитация игры overCraft на основной машине (под AddressSanitizer): мир,
// игрок, свиньи, предметы, инвентарь и кадр движка — тот же код, что в
// main.cpp, только ввод скриптовый. Игрок бежит, крутит головой, прыгает,
// ломает и ставит блоки, бьёт свиней. Ловит порчу памяти и падения, которые
// в QEMU мышью не воспроизвести. tools/ovcraft_bench/run.sh sim [кадров]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/ovengine_ll.h"
#include "world/world.h"
#include "game/player.h"
#include "game/entity.h"
#include "game/inventory.h"
#include "game/shapes.h"
#include "game/hand.h"
#include "game/icons.h"
#include "config.h"
#include "items.h"
uint64_t bench_tsc_per_ms = 1000000;
using namespace ov;

static World g_world;
static Player g_player;
static Entities g_ents;
static Inventory g_inv;
static HandAnim g_hand;

int main(int argc, char** argv) {
    ov_strlcpy(g_oe_dir, OVCRAFT_DIR, sizeof g_oe_dir);
    int frames = argc > 1 ? atoi(argv[1]) : 3000;
    int W = 640, H = 400;
    Renderer r;
    uint32_t* fb = new uint32_t[W * H];
    for (int i = 0; i < TEX_COUNT; i++) r.loadTexture(TEXTURES[i]);
    for (int i = 0; i < ITEM_COUNT; i++) r.loadTexture(ITEM_TEXTURES[i]);
    int crack0 = TEX_COUNT + ITEM_COUNT;
    for (int k = 0; k < 10; k++) { char p[64]; snprintf(p, sizeof p, "resources/destroy/destroy_%d.png", k); r.loadTexture(p); }
    if (!shapes::init(r, crack0) || !g_ents.init(r)) { printf("init failed: %s\n", g_ents.error()); return 1; }
    icons::init(&r);
    if (!g_world.init(&r)) return 1;
    g_player.spawn(g_world, 8, 8);
    for (int i = 0; i < 400 && !g_world.ready_at(g_player.pos); i++) g_world.update(g_player.pos);
    g_player.spawn(g_world, 8, 8);
    uint32_t seed = 12345;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFF) / 65536.0f; };
    Input in;
    int broken = 0, placed = 0, hits = 0, dropped = 0, maxtri = 0, pigs_max = 0;
    float breaking = 0; int bx = 0, by = -1000, bz = 0;
    long total_holes = 0; int worst = 0, worst_f = -1;
    for (int f = 0; f < frames; f++) {
        const float dt = 1.0f / 60.0f;
        // Сценарий: бежим вперёд, крутимся, прыгаем, жмём кнопки
        memset(in.pressed_, 0, sizeof in.pressed_);
        in.prev_ = in.buttons_;
        in.down_[key::W] = (f / 200) % 4 != 3;
        in.down_[key::CTRL] = (f / 300) % 2 == 0;
        in.down_[key::SPACE] = rnd() < 0.08f;
        in.down_[key::SHIFT] = (f / 500) % 5 == 4;
        in.dx_ = (int)((rnd() - 0.5f) * 40.0f) + ((f / 120) % 2 ? 6 : -6);
        in.dy_ = (int)((rnd() - 0.5f) * 20.0f);
        if (g_player.pitch > 0.6f) in.dy_ += 8;
        if (g_player.pitch < -0.9f) in.dy_ -= 8;
        in.buttons_ = (rnd() < 0.5f ? 1 : 0) | (rnd() < 0.08f ? 2 : 0);
        if (f % 700 == 350) { g_player.set_creative(!g_player.creative); }
        if (rnd() < 0.01f) g_inv.selected = (int)(rnd() * 9);
        g_player.look(in);
        g_player.update(dt, in, g_world);
        if (g_player.dead) { g_player.respawn(g_world); }

        Vec3 eye = g_player.eye(), fwd = g_player.forward();
        RayHit hit = g_world.raycast(eye, fwd, cfg::REACH);
        float md = 0;
        int mob = g_ents.raycast(eye, fwd, hit.hit ? ov::fmin(hit.dist, cfg::ATTACK_REACH) : cfg::ATTACK_REACH, &md);
        if (in.clicked(0)) g_hand.start_swing();
        if (mob >= 0 && in.clicked(0)) { g_ents.attack(mob, cfg::HAND_DAMAGE, g_player.pos); hits++; }
        else if (hit.hit && in.button(0)) {
            uint8_t b = g_world.get(hit.x, hit.y, hit.z);
            const BlockInfo& bi = block_info(b);
            if (hit.x != bx || hit.y != by || hit.z != bz) { breaking = 0; bx = hit.x; by = hit.y; bz = hit.z; }
            if (bi.hardness >= 0) breaking += g_player.creative ? 1.0f : dt / (bi.hardness > 0 ? bi.hardness : 0.01f);
            if (breaking >= 1.0f && bi.hardness >= 0) {
                g_world.set(hit.x, hit.y, hit.z, AIR);
                if (bi.drop) g_ents.drop(Vec3(hit.x + 0.5f, hit.y + 0.3f, hit.z + 0.5f), Vec3(0, 3, 0), bi.drop, 1, 0.3f);
                broken++; breaking = 0; by = -1000;
            }
        }
        if (!in.button(0)) { breaking = 0; by = -1000; }
        if (in.clicked(1) && hit.hit && !g_inv.held().empty() && item_is_block(g_inv.held().id) &&
            !g_player.intersects_block(hit.px, hit.py, hit.pz) && g_world.get(hit.px, hit.py, hit.pz) == AIR) {
            g_world.set(hit.px, hit.py, hit.pz, (uint8_t)g_inv.held().id);
            if (!g_player.creative) g_inv.take_held(1);
            placed++;
        }
        if (rnd() < 0.01f && !g_inv.held().empty()) { g_ents.throw_from(g_player, g_inv.held().id, 1); g_inv.take_held(1); dropped++; }
        uint16_t held = g_inv.held().empty() ? 0 : g_inv.held().id;
        g_hand.update(dt, held);
        g_ents.update(dt, g_world, g_player, g_inv);
        if (f < 600) g_ents.spawn_near(g_world, g_player.pos, 3.0f, 8.0f, 1);   // свиней — побольше и поближе
        g_world.update(g_player.pos);
        int pigs = g_ents.count(ENT_PIG);
        if (pigs > pigs_max) pigs_max = pigs;

        r.setTarget(fb, W, H, 70 + (f % 31));
        Camera cam;
        cam.proj = Mat4::perspective(radians(70.0f * (g_player.sprinting ? 1.15f : 1.0f)), (float)W / H, cfg::NEAR_Z, 400.0f);
        cam.overlay_proj = Mat4::perspective(radians(70.0f), (float)W / H, 0.05f, 10.0f);
        cam.view = Mat4::lookAt(eye, eye + fwd, Vec3(0, 1, 0));
        cam.light.ambient = 1.0f;
        // Небо — пурпурное и без тумана: дыра в геометрии сразу видна по цвету
        cam.sky_top = cam.sky_bottom = 0xFF00FF;
        r.begin(cam);
        g_world.draw(r, cam.proj * cam.view, eye);
        g_ents.draw(r, eye);
        if (f % 3 == 0) g_ents.draw_player(r, g_player, held, g_hand.swing_value());
        if (breaking > 0 && by > -1000) r.draw(shapes::crack((int)(breaking * 10)), Mat4::translate(Vec3(bx + 0.5f, by + 0.5f, bz + 0.5f)));
        hand::draw(r, g_ents.model(ENT_PLAYER), g_player, g_hand);
        r.end();
        FrameStats st = r.stats();
        // Дыры: пиксель неба, у которого и слева, и справа (или сверху и снизу) — мир
        int holes = 0;
        for (int y = 1; y < H - 1; y++)
            for (int x = 1; x < W - 1; x++) {
                const uint32_t* p = fb + y * W + x;
                if ((*p & 0xFFFFFF) != 0xFF00FF) continue;
                bool h1 = (p[-1] & 0xFFFFFF) != 0xFF00FF && (p[1] & 0xFFFFFF) != 0xFF00FF;
                bool v1 = (p[-W] & 0xFFFFFF) != 0xFF00FF && (p[W] & 0xFFFFFF) != 0xFF00FF;
                if (h1 || v1) holes++;
            }
        total_holes += holes;
        if (getenv("DBGF") && f == atoi(getenv("DBGF"))) {
            int dx = atoi(getenv("DBGX")), dy = atoi(getenv("DBGY"));
            int pct = 70 + (f % 31), iw = W * pct / 100, ih = H * pct / 100;
            fprintf(stdout, "pixel %d,%d = %06X (internal %d,%d)\n", dx, dy, fb[dy * W + dx] & 0xFFFFFF, dx * iw / W, dy * ih / H);
            r.debugPixel(dx * iw / W, dy * ih / H);
            fflush(stdout);
        }
        if (holes > worst) {
            worst = holes; worst_f = f;
            FILE* o = fopen(getenv("HOLES") ? getenv("HOLES") : "/dev/null", "wb");
            if (o) { fprintf(o, "P6 %d %d 255\n", W, H); for (int i = 0; i < W * H; i++) { unsigned char q[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] }; fwrite(q, 1, 3, o); } fclose(o); }
        }
        if (st.submitted > maxtri) maxtri = st.submitted;
        if (st.dropped) fprintf(stderr, "frame %d: dropped %d triangles\n", f, st.dropped);
        // Интерфейс: значки в слотах (кеш значков, разные размеры)
        ui_win_t win = { fb, W, H };
        for (int k = 0; k < 9; k++) icons::slot(&win, 10 + k * 40, H - 40, 32 + (f % 3) * 8, g_inv.slots[k].id, g_inv.slots[k].count);
    }
    int n = 0;
    for (int i = 0; i < Inventory::SIZE; i++) n += g_inv.slots[i].count;
    fprintf(stderr, "%d frames: broken %d, placed %d, pig hits %d, thrown %d, items in inventory %d, pigs max %d, submitted max %d, pos %.1f %.1f %.1f\n",
           frames, broken, placed, hits, dropped, n, pigs_max, maxtri, g_player.pos.x, g_player.pos.y, g_player.pos.z);
    fprintf(stderr, "holes: total %ld, worst frame %d with %d pixels\n", total_holes, worst_f, worst);
    return 0;
}
