// Стенд моделей: Стив и свинья на полу, предметы, рука от первого лица.
//   SCENE=models|hand|held|item  CAM=x,y,z  YAW=  PITCH=  (см. run.sh)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/ovengine_ll.h"
#include "game/model.h"
#include "game/shapes.h"
#include "game/hand.h"
#include "game/player.h"
#include "blocks.h"
#include "items.h"
#include "entities.h"
uint64_t bench_tsc_per_ms = 1000000;
using namespace ov;
int main(int argc, char** argv) {
    ov_strlcpy(g_oe_dir, OVCRAFT_DIR, sizeof g_oe_dir);
    int W = 960, H = 600;
    Renderer r;
    uint32_t* fb = new uint32_t[W * H];
    r.setTarget(fb, W, H, 100);
    int n = 0;
    for (int i = 0; i < TEX_COUNT; i++) n = r.loadTexture(TEXTURES[i]);
    for (int i = 0; i < ITEM_COUNT; i++) n = r.loadTexture(ITEM_TEXTURES[i]);
    int crack0 = n + 1;
    for (int k = 0; k < 10; k++) { char p[64]; snprintf(p, sizeof p, "resources/destroy/destroy_%d.png", k); r.loadTexture(p); }
    shapes::init(r, crack0);
    Model mods[2];
    for (int t = 0; t < 2; t++) {
        int tex = r.loadTexture(ENTITIES[t].texture);
        if (tex < 0 || !mods[t].load(ENTITIES[t].model, tex, tex)) { printf("model %d: %s\n", t, mods[t].error()); return 1; }
    }
    const char* scene = getenv("SCENE") ? getenv("SCENE") : "models";
    Vec3 cam(0, 1.6f, 3.2f);
    if (getenv("CAM")) sscanf(getenv("CAM"), "%f,%f,%f", &cam.x, &cam.y, &cam.z);
    float yaw = getenv("YAW") ? atof(getenv("YAW")) : 0.0f, pitch = getenv("PITCH") ? atof(getenv("PITCH")) : -0.25f;
    Vec3 fwd(ov::sin(yaw) * ov::cos(pitch), ov::sin(pitch), -ov::cos(yaw) * ov::cos(pitch));
    Camera c;
    c.proj = Mat4::perspective(radians(70.0f), (float)W / H, 0.08f, 400.0f);
    c.overlay_proj = Mat4::perspective(radians(70.0f), (float)W / H, 0.05f, 10.0f);
    c.view = Mat4::lookAt(cam, cam + fwd, Vec3(0, 1, 0));
    c.light.ambient = 1.0f;
    r.begin(c);
    // Пол из травы 9x9 и стенка
    for (int z = -4; z <= 4; z++) for (int x = -4; x <= 4; x++) r.draw(shapes::block(GRASS), Mat4::translate(Vec3(x + 0.5f, -0.5f, z + 0.5f)));
    float a = getenv("ANIM") ? atof(getenv("ANIM")) : 0.6f;
    Model::Pose ps, pp;
    for (int i = 0; i < mods[0].parts(); i++) {
        const char* nm = mods[0].part(i).name;
        if (!strcmp(nm, "right_arm")) ps.rot[i].x = -a; if (!strcmp(nm, "left_arm")) ps.rot[i].x = a;
        if (!strcmp(nm, "right_leg")) ps.rot[i].x = a;  if (!strcmp(nm, "left_leg")) ps.rot[i].x = -a;
    }
    for (int i = 0; i < mods[1].parts(); i++) { const char* nm = mods[1].part(i).name;
        if (nm[0] == 'l') pp.rot[i].x = (nm[3] == '0' || nm[3] == '3') ? a : -a; }
    float ey = getenv("EYAW") ? atof(getenv("EYAW")) : 2.6f;
    if (!strcmp(scene, "models")) {
        mods[0].draw(r, Model::world_matrix(Vec3(-0.9f, 0, 0), ey, ENTITIES[0].scale), ps, false);
        mods[1].draw(r, Model::world_matrix(Vec3(1.0f, 0, 0), ey + 0.6f, ENTITIES[1].scale), pp, false);
        r.draw(shapes::item(PORKCHOP), Mat4::translate(Vec3(0.2f, 0.35f, 1.4f)) * Mat4::rotateY(0.5f) * Mat4::scale(0.4f));
        r.draw(shapes::block(LOG), Mat4::translate(Vec3(-0.4f, 0.2f, 1.6f)) * Mat4::rotateY(0.5f) * Mat4::scale(0.25f));
        r.draw(shapes::block(STONE), Mat4::translate(Vec3(2.5f, 0.5f, -1.5f)));
        r.draw(shapes::crack(7), Mat4::translate(Vec3(2.5f, 0.5f, -1.5f)));
    } else {
        Player p;
        HandAnim h;
        h.shown = !strcmp(scene, "held") ? (uint16_t)GRASS : (!strcmp(scene, "item") ? (uint16_t)PORKCHOP : 0);
        h.swing = getenv("SWING") ? atof(getenv("SWING")) : -1.0f;
        hand::draw(r, mods[0], p, h);
    }
    r.end();
    FILE* f = fopen(argc > 1 ? argv[1] : "/dev/null", "wb");
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) { unsigned char q[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] }; fwrite(q, 1, 3, f); }
    fclose(f);
}
