// =============================================================================
// overCraft — воксельная игра overOS на C++ и витрина движка ovengine.
//
// Процедурный мир чанками (world/), игрок с физикой и выживанием
// (game/player), мобы и выпавшие предметы (game/entity, модели .mdl в
// resources/models), инвентарь (game/inventory), рука от первого лица
// (game/hand), интерфейс (game/hud, game/icons), настройки (config.h),
// блоки, предметы и мобы (blocks.h, items.h, entities.h). Этот файл только
// связывает их: окно, ввод, шаг игры, кадр.
//
// Игра — папка /desprogs/overCraft: ovcraft.prg (этот код), ovengine.ll
// (движок, исходники в engine/) и resources/ с текстурами PNG. Всё тяжёлое —
// генерация чанков, сетки, растеризация — идёт на всех ядрах процессора:
// движок берёт себе рабочие ядра (ov_worker_start) и раздаёт им задачи.
// =============================================================================

#include "ovos.h"
#include "libui/ui.h"
#include "libui/libui_ll.h"
#include "cxxrt/cxxrt.h"
#include "engine/ovengine_ll.h"
#include "config.h"
#include "blocks.h"
#include "world/world.h"
#include "game/input.h"
#include "game/player.h"
#include "game/hud.h"
#include "game/entity.h"
#include "game/inventory.h"
#include "game/icons.h"
#include "game/shapes.h"
#include "game/hand.h"
#include "items.h"

using namespace ov;

// Окно с сообщением, если игра не может стартовать (нет движка, текстуры)
static int fail(ui_win_t* w, const char* what, const char* why) {
    while (w && ui_begin(w, UI_WAIT)) {
        ui_clear(w);
        ui_text(w, 20, 20, what, UI_DANGER);
        ui_text(w, 20, 44, why, UI_TEXT);
        ui_end(w);
    }
    return 1;
}

class FpsMeter {
public:
    void frame(uint64_t now_ms) {
        frames_++;
        if (start_ == 0) start_ = now_ms;
        if (now_ms - start_ >= 1000) {
            fps_ = (int)(frames_ * 1000 / (now_ms - start_));
            frames_ = 0;
            start_ = now_ms;
        }
    }
    int fps() const { return fps_; }
private:
    uint64_t start_ = 0, frames_ = 0;
    int fps_ = 0;
};

static World           g_world;
static Player          g_player;
static Input           g_input;
static const Input     g_no_input;          // пока открыт инвентарь или игрок мёртв: стоим
static Inventory       g_inv;
static InventoryScreen g_screen;
static Entities        g_ents;
static HandAnim        g_hand;

// Всё из инвентаря — на землю вокруг (смерть в выживании)
static void drop_everything() {
    for (int i = 0; i < Inventory::SIZE; i++) {
        Stack& s = g_inv.slots[i];
        if (s.empty()) continue;
        float a = (float)i * 2.4f;
        g_ents.drop(g_player.pos + Vec3(0, 1.0f, 0), Vec3(ov::sin(a) * 2.5f, 3.0f, ov::cos(a) * 2.5f), s.id, s.count, 1.0f);
        s.clear();
    }
}

int main() {
    libui_bind();
    ovcxx_init();
    ui_app_id("ovcraft");

    ui_win_t* w = ui_open("overCraft", 1024, 640, nullptr, WP_FLAG_RESIZABLE);
    if (!w) return 1;
    ui_redraw_all(w);                         // каждый кадр закрашивается целиком

    char dir[96];
    if (oe_game_dir(dir, sizeof(dir)) != 0) return fail(w, "Не нашлась папка игры", "ov_proc_exe не ответил");
    int rc = oe_bind(dir);
    if (rc < 0) {
        Text why;
        why.add(dir).add(rc == -1 ? "/ovengine.ll: нет файла" : rc == -3 ? "/ovengine.ll: не та версия движка"
                                                              : "/ovengine.ll: не все функции");
        return fail(w, "Не загрузился движок", why.str());
    }

    Renderer r(cfg::MAX_THREADS);
    if (!r.ok()) return fail(w, "Движок не создался", "не хватило памяти");
    // Текстуры по порядку: номер у движка = номер в blocks.h, затем предметы
    // (items.h), затем трещины; развёртки мобов грузит Entities
    const int total = TEX_COUNT + ITEM_COUNT + 10;
    for (int i = 0; i < total; i++) {
        char crack[40];
        const char* path;
        if (i < TEX_COUNT) path = TEXTURES[i];
        else if (i < TEX_COUNT + ITEM_COUNT) path = ITEM_TEXTURES[i - TEX_COUNT];
        else {
            Text t;
            t.add("resources/destroy/destroy_").num(i - TEX_COUNT - ITEM_COUNT).add(".png");
            ov_strlcpy(crack, t.str(), sizeof(crack));
            path = crack;
        }
        int t = r.loadTexture(path);
        if (t != i) {
            Text why;
            why.add(path).add(": ").add(t < 0 ? Renderer::errorText(t) : "не тот номер");
            return fail(w, "Не загрузилась текстура", why.str());
        }
    }
    const int crack_tex0 = TEX_COUNT + ITEM_COUNT;
    if (!shapes::init(r, crack_tex0)) return fail(w, "Не построились предметы", "нет картинки предмета");
    if (!g_ents.init(r)) {
        Text why;
        why.add(g_ents.error()).add(": не загрузилось (модель или развёртка)");
        return fail(w, "Не загрузились мобы", why.str());
    }
    icons::init(&r);
    if (!g_world.init(&r)) return fail(w, "Мир не создался", "не хватило памяти");

    // Мир строится вокруг точки появления, пока под ногами не будет земли
    g_player.spawn(g_world, 8, 8);
    while (!g_world.ready_at(g_player.pos)) {
        if (!ui_begin(w, 0)) { ui_close(w); return 0; }
        g_world.update(g_player.pos);
        hud::loading(w, g_world.progress_percent(g_player.pos));
        ui_end(w);
    }
    g_player.spawn(g_world, 8, 8);            // высоту — уже по готовому миру
    g_player.set_creative(cfg::START_CREATIVE);
    if (g_player.creative) g_inv.fill_creative();

    FpsMeter meter;
    uint64_t prev = ov_ms();
    const uint64_t tsc_ms = ov_tsc_per_ms();
    float frame_ms = 0.0f, break_timer = 0.0f;
    bool debug = false, was_grabbed = false, was_dead = false;
    int res = cfg::RES_MAX;                   // разрешение мира, % от окна
    float avg_ms = 0.0f;
    float fov_k = 1.0f;                       // множитель FOV (бег), сглажен
    int view = 0;                             // 0 — глаза, 1 — сзади, 2 — спереди (F5)
    float breaking = 0.0f;                    // 0..1 — сколько сломан блок под прицелом
    int bx = 0, by = -1000, bz = 0;           // какой именно
    float eat_t = 0.0f, name_time = 0.0f, msg_time = 0.0f;
    const char* msg = nullptr;
    uint16_t last_held = 0;
    int start_pigs = cfg::START_PIGS;         // свиньи у точки появления — по мере загрузки чанков
    float start_pigs_t = 10.0f;

    while (ui_begin(w, 0)) {
        uint64_t now = ov_ms();
        float dt = (float)(now - prev) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        prev = now;
        uint64_t t0 = ov_rdtsc();
        int W = ui_width(w), H = ui_height(w);

        // --- ввод
        g_input.frame(w);
        bool grabbed = ui_grabbed(w);
        if (was_grabbed && !grabbed) g_input.release_all();      // мышь отняли (Meta, другое окно)
        was_grabbed = grabbed;
        if (!grabbed && ui_mouse_clicked(w)) ui_grab(w, 1);
        Stack thrown;                                            // что выбросить в мир в этом кадре
        if (grabbed && g_input.pressed(key::ESC)) {
            if (g_screen.open) g_screen.hide(g_inv, thrown);
            else ui_grab(w, 0);
        }
        if (grabbed && g_input.pressed(key::E) && !g_player.dead) {
            if (g_screen.open) g_screen.hide(g_inv, thrown);
            else g_screen.show(W, H);
        }
        if (g_input.pressed(key::F3)) debug = !debug;
        if (g_input.pressed(key::F5)) view = (view + 1) % 3;
        if (g_input.pressed(key::F4)) {
            g_player.set_creative(!g_player.creative);
            msg = g_player.creative ? "Режим игры: творческий" : "Режим игры: выживание";
            msg_time = 2.0f;
        }
        const bool playing = grabbed && !g_screen.open && !g_player.dead;
        if (playing) {
            for (int k = 0; k < 9; k++) if (g_input.pressed(key::N1 + k)) g_inv.selected = k;
            if (g_input.scroll()) g_inv.selected = ((g_inv.selected + g_input.scroll()) % 9 + 9) % 9;
            // Q — выбросить один, Ctrl+Q — всю стопку
            if (g_input.pressed(key::Q) && !g_inv.held().empty()) {
                Stack& h = g_inv.held();
                int n = g_input.down(key::CTRL) ? h.count : 1;
                g_ents.throw_from(g_player, h.id, n);
                if (!g_player.creative) g_inv.take_held(n);
            }
        }
        if (g_screen.open) {
            Stack out;
            g_screen.update(g_input, g_inv, g_player.creative, W, H, out);
            if (!out.empty()) thrown = out;
        }
        if (!thrown.empty()) g_ents.throw_from(g_player, thrown.id, thrown.count);

        // --- игра
        if (playing) g_player.look(g_input);
        g_player.update(dt, playing ? g_input : g_no_input, g_world);
        if (g_player.dead && !was_dead) {
            Stack out;
            if (g_screen.open) g_screen.hide(g_inv, out);
            if (!out.empty()) g_ents.throw_from(g_player, out.id, out.count);
            if (!g_player.creative) drop_everything();
        }
        was_dead = g_player.dead;
        if (g_player.dead && grabbed && (g_input.clicked(0) || g_input.pressed(key::SPACE))) g_player.respawn(g_world);

        Vec3 eye = g_player.eye(), fwd = g_player.forward();
        RayHit hit = g_world.raycast(eye, fwd, cfg::REACH);
        float mob_dist = 0.0f;
        int mob = g_ents.raycast(eye, fwd, hit.hit ? ov::fmin(hit.dist, cfg::ATTACK_REACH) : cfg::ATTACK_REACH, &mob_dist);
        const Stack& held = g_inv.held();
        const ItemInfo* food = item_info(held.id);
        bool can_eat = food && food->food > 0 && (g_player.food < cfg::MAX_FOOD || g_player.creative);

        if (playing) {
            // ЛКМ: моба — ударить, блок — ломать (в выживании — со временем и трещинами)
            if (g_input.clicked(0)) g_hand.start_swing();
            if (mob >= 0 && g_input.clicked(0)) {
                g_ents.attack(mob, cfg::HAND_DAMAGE, g_player.pos);
                g_player.add_exhaustion(0.1f);
            } else if (hit.hit && g_input.button(0) && mob < 0) {
                uint8_t b = g_world.get(hit.x, hit.y, hit.z);
                const BlockInfo& bi = block_info(b);
                if (hit.x != bx || hit.y != by || hit.z != bz) { breaking = 0.0f; bx = hit.x; by = hit.y; bz = hit.z; }
                bool done = false;
                if (g_player.creative) {
                    break_timer -= dt;
                    if (g_input.clicked(0) || break_timer <= 0.0f) { done = true; break_timer = cfg::BREAK_CREATIVE; }
                } else if (bi.hardness >= 0.0f) {
                    breaking += bi.hardness > 0.0f ? dt / bi.hardness : 1.0f;
                    g_hand.start_swing();
                    if (breaking >= 1.0f) done = true;
                }
                if (done && bi.hardness >= 0.0f) {
                    g_world.set(hit.x, hit.y, hit.z, AIR);
                    if (!g_player.creative && bi.drop) {
                        Vec3 c((float)hit.x + 0.5f, (float)hit.y + 0.3f, (float)hit.z + 0.5f);
                        float a = (float)((hit.x * 7 + hit.z * 13) & 63) * 0.1f;
                        g_ents.drop(c, Vec3(ov::sin(a) * 1.2f, 3.0f, ov::cos(a) * 1.2f), bi.drop, 1, 0.3f);
                    }
                    g_player.add_exhaustion(0.005f);
                    breaking = 0.0f;
                    by = -1000;
                }
            }
            if (!g_input.button(0)) { breaking = 0.0f; break_timer = 0.0f; by = -1000; }

            // ПКМ: еда — держать; блок — поставить
            if (can_eat && g_input.button(1)) {
                eat_t += dt;
                if (eat_t >= cfg::EAT_TIME) {
                    g_player.eat(food->food, food->saturation);
                    if (!g_player.creative) g_inv.take_held(1);
                    eat_t = 0.0f;
                }
            } else {
                eat_t = 0.0f;
                if (g_input.clicked(1) && hit.hit && item_is_block(held.id) && !held.empty() &&
                    block_info((uint8_t)held.id).placeable && !g_player.intersects_block(hit.px, hit.py, hit.pz) &&
                    g_world.get(hit.px, hit.py, hit.pz) == AIR) {
                    g_world.set(hit.px, hit.py, hit.pz, (uint8_t)held.id);
                    if (!g_player.creative) g_inv.take_held(1);
                    g_hand.start_swing();
                }
            }
            // СКМ: взять блок — выбрать в панели, а в творческом и положить в руку
            if (g_input.clicked(2) && hit.hit) {
                uint8_t b = g_world.get(hit.x, hit.y, hit.z);
                bool found = false;
                for (int k = 0; k < 9; k++)
                    if (g_inv.slots[k].id == b && !g_inv.slots[k].empty()) { g_inv.selected = k; found = true; break; }
                if (!found && g_player.creative && block_info(b).placeable) { g_inv.held().id = b; g_inv.held().count = 1; }
            }
        } else {
            breaking = 0.0f;
            eat_t = 0.0f;
        }

        uint16_t held_id = g_inv.held().empty() ? 0 : g_inv.held().id;
        if (held_id != last_held) { name_time = 2.0f; last_held = held_id; }
        if (name_time > 0.0f) name_time -= dt;
        if (msg_time > 0.0f) msg_time -= dt; else msg = nullptr;
        g_hand.update(dt, held_id);
        g_hand.eat = eat_t / cfg::EAT_TIME;

        g_ents.update(dt, g_world, g_player, g_inv);
        if (start_pigs > 0 && start_pigs_t > 0.0f) {
            start_pigs_t -= dt;
            start_pigs -= g_ents.spawn_near(g_world, g_player.pos, 6.0f, 14.0f, start_pigs);
        }
        g_world.update(g_player.pos);

        // --- кадр
        r.setTarget(ui_pixels(w), W, H, res);
        // Бег — FOV шире, плавно, как эффект скорости в оригинале
        float want_k = (g_player.sprinting ? cfg::FOV_SPRINT : 1.0f) * (g_player.flying ? cfg::FOV_FLY : 1.0f);
        fov_k += (want_k - fov_k) * (dt * 10.0f > 1.0f ? 1.0f : dt * 10.0f);
        Camera cam;
        float aspect = (float)W / (float)H;
        cam.proj = Mat4::perspective(radians(cfg::FOV_DEG * fov_k), aspect, cfg::NEAR_Z, 400.0f);
        cam.overlay_proj = Mat4::perspective(radians(cfg::HAND_FOV_DEG), aspect, 0.05f, 10.0f);
        // Камера: глаза, сзади или спереди (не сквозь стену)
        Vec3 cam_pos = eye, look_at = eye + fwd;
        if (view != 0) {
            Vec3 dir = view == 1 ? -fwd : fwd;
            RayHit back = g_world.raycast(eye, dir, cfg::THIRD_PERSON_DIST);
            float d = back.hit ? ov::fmax(back.dist - 0.25f, 0.3f) : cfg::THIRD_PERSON_DIST;
            cam_pos = eye + dir * d;
            look_at = eye;
        }
        // Удар — камера на миг наклоняется вбок
        Vec3 up(0, 1, 0);
        if (g_player.hurt_time > 0.0f) {
            float roll = ov::sin(g_player.hurt_time / 0.5f * ov::PI) * 0.18f;
            Vec3 right = normalize(cross(look_at - cam_pos, up));
            up = up * ov::cos(roll) + right * ov::sin(roll);
        }
        if (g_player.dead) cam_pos.y -= 1.2f;                    // лежим
        cam.view = Mat4::lookAt(cam_pos, look_at, up);
        cam.light.ambient = 1.0f;             // свет уже в вершинах (стороны и углы)
        cam.sky_top = cfg::SKY_TOP;
        cam.sky_bottom = cfg::SKY_BOTTOM;
        cam.fog_start = cfg::FOG_START;
        cam.fog_end = cfg::FOG_END;
        Mat4 vp = cam.proj * cam.view;
        r.begin(cam);
        g_world.draw(r, vp, cam_pos);
        g_ents.draw(r, cam_pos);
        if (view != 0 && !g_player.dead) g_ents.draw_player(r, g_player, held_id, g_hand.swing_value());
        if (breaking > 0.0f && by > -1000)
            r.draw(shapes::crack((int)(breaking * 10.0f)), Mat4::translate(Vec3((float)bx + 0.5f, (float)by + 0.5f, (float)bz + 0.5f)));
        if (view == 0 && !g_player.dead) hand::draw(r, g_ents.model(ENT_PLAYER), g_player, g_hand);
        r.end();
        if (hit.hit && mob < 0 && !g_player.dead) hud::outline(w, r, vp, cam_pos, hit.x, hit.y, hit.z);

        meter.frame(now);
        if (tsc_ms) frame_ms = (float)(ov_rdtsc() - t0) / (float)tsc_ms;
        // Не успеваем — мир рисуется чуть мельче; есть запас — обратно. Время
        // сглажено, чтобы разовый всплеск (новые чанки) не дёргал картинку.
        avg_ms = avg_ms <= 0.0f ? frame_ms : avg_ms * 0.9f + frame_ms * 0.1f;
        if (cfg::DYNAMIC_RES) {
            if (avg_ms > cfg::FRAME_BUDGET && res > cfg::RES_MIN) res -= 1;
            else if (avg_ms < cfg::FRAME_BUDGET * 0.72f && res < cfg::RES_MAX) res += 1;
        }
        HudInfo h;
        h.fps = meter.fps();
        h.frame_ms = frame_ms;
        h.res = res;
        Renderer::memory(&h.eng_used, &h.eng_total, &h.eng_free);
        h.game_used = ov_heap_used();
        h.st = r.stats();
        h.chunks = g_world.chunks_loaded();
        h.sections = g_world.sections_drawn();
        h.pos = g_player.pos;
        h.yaw = g_player.yaw;
        h.flying = g_player.flying;
        h.in_water = g_player.head_in_water;
        h.grabbed = grabbed;
        h.debug = debug;
        h.player = &g_player;
        h.inv = &g_inv;
        h.name_time = name_time;
        h.message = msg;
        h.target = hit.hit ? block_info(g_world.get(hit.x, hit.y, hit.z)).name : nullptr;
        h.progress = -1;
        hud::draw(w, r, h);
        if (g_screen.open) g_screen.draw(w, g_inv, g_player.creative);
        if (g_player.dead) hud::death(w);
        ui_end(w);
    }

    ui_close(w);
    return 0;
}
