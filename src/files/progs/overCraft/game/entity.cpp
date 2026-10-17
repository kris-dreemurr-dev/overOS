// =============================================================================
// entity.cpp — мобы и выпавшие предметы (см. entity.h).
// =============================================================================

#include "entity.h"
#include "config.h"
#include "items.h"
#include "world/world.h"
#include "game/player.h"
#include "game/inventory.h"
#include "game/shapes.h"

using ov::Vec3;
using ov::Mat4;
using phys::ifloor;

namespace {

float wrap_angle(float a) {
    while (a > ov::PI) a -= 2.0f * ov::PI;
    while (a < -ov::PI) a += 2.0f * ov::PI;
    return a;
}

float approach_angle(float from, float to, float max_step) {
    float d = wrap_angle(to - from);
    if (d > max_step) d = max_step;
    if (d < -max_step) d = -max_step;
    return wrap_angle(from + d);
}

// Луч и коробка (метод плит): расстояние до входа или -1
float ray_box(const Vec3& o, const Vec3& d, const Vec3& lo, const Vec3& hi) {
    float t0 = 0.0f, t1 = 1e9f;
    const float oo[3] = { o.x, o.y, o.z }, dd[3] = { d.x, d.y, d.z };
    const float l[3] = { lo.x, lo.y, lo.z }, h[3] = { hi.x, hi.y, hi.z };
    for (int a = 0; a < 3; a++) {
        if (ov::fabs(dd[a]) < 1e-6f) {
            if (oo[a] < l[a] || oo[a] > h[a]) return -1.0f;
            continue;
        }
        float inv = 1.0f / dd[a];
        float ta = (l[a] - oo[a]) * inv, tb = (h[a] - oo[a]) * inv;
        if (ta > tb) { float t = ta; ta = tb; tb = t; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

} // namespace

float Entities::rnd() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return (float)(seed_ & 0xFFFFFF) / 16777216.0f;
}

bool Entities::init(ov::Renderer& r) {
    for (int t = 0; t < ENT_TYPE_COUNT; t++) {
        const EntityInfo& ei = ENTITIES[t];
        if (!ei.model) continue;
        int tex = r.loadTexture(ei.texture);
        if (tex < 0) { err_ = ei.texture; return false; }
        // Красная копия развёртки: вспышка урона — просто другая текстура
        int w = 0, h = 0;
        const uint32_t* px = r.texturePixels(tex, &w, &h);
        if (!px) { err_ = ei.texture; return false; }
        uint32_t* red = new uint32_t[w * h];
        for (int i = 0; i < w * h; i++) {
            uint32_t c = px[i];
            uint32_t R = ((c >> 16) & 255), G = ((c >> 8) & 255), B = (c & 255);
            R = (R + 255 * 2) / 3; G = G * 5 / 10; B = B * 5 / 10;
            red[i] = (R << 16) | (G << 8) | B;
        }
        int hurt = r.addTexture(w, h, red);
        delete[] red;
        if (hurt < 0) { err_ = "красная развёртка"; return false; }
        if (!models_[t].load(ei.model, tex, hurt)) { err_ = ei.model; return false; }
        has_model_[t] = true;
    }
    return true;
}

int Entities::spawn(uint8_t type, const Vec3& pos) {
    for (int i = 0; i < MAX; i++) {
        Entity& e = ents_[i];
        if (e.alive) continue;
        e = Entity();
        e.alive = true;
        e.type = type;
        e.body.pos = pos;
        e.body.hw = ENTITIES[type].width * 0.5f;
        e.body.h = ENTITIES[type].height;
        e.health = ENTITIES[type].health;
        e.yaw = e.head_yaw = e.target_yaw = (rnd() * 2.0f - 1.0f) * ov::PI;
        e.ai_t = rnd() * 3.0f;
        return i;
    }
    return -1;
}

int Entities::count(uint8_t type) const {
    int n = 0;
    for (int i = 0; i < MAX; i++) if (ents_[i].alive && ents_[i].type == type) n++;
    return n;
}

void Entities::drop(const Vec3& pos, const Vec3& vel, uint16_t id, int count, float delay) {
    if (!item_valid(id) || count <= 0) return;
    int i = spawn(ENT_ITEM, pos);
    if (i < 0) return;
    Entity& e = ents_[i];
    e.body.vel = vel;
    e.item = id;
    e.count = count;
    e.pickup = delay;
    e.yaw = rnd() * 6.28f;
}

void Entities::throw_from(const Player& p, uint16_t id, int count) {
    Vec3 f = p.forward();
    drop(p.eye() - Vec3(0, 0.3f, 0), f * 6.0f + Vec3(0, 1.5f, 0), id, count, 1.5f);
}

// Свинья встаёт только на траву под открытым небом, как в оригинале
bool Entities::try_spawn(World& w, float fx, float fz) {
    int x = ifloor(fx), z = ifloor(fz);
    int top = w.surface(x, z);
    for (int y = top + 3; y >= top - 3 && y > 1; y--) {
        if (w.get(x, y, z) != GRASS) continue;
        if (w.get(x, y + 1, z) != AIR || w.get(x, y + 2, z) != AIR) return false;
        return spawn(ENT_PIG, Vec3((float)x + 0.5f, (float)y + 1.01f, (float)z + 0.5f)) >= 0;
    }
    return false;
}

int Entities::spawn_near(World& w, const Vec3& at, float rmin, float rmax, int n) {
    int done = 0;
    for (int tries = 0; tries < n * 20 && done < n; tries++) {
        float a = rnd() * 6.2832f, r = rmin + rnd() * (rmax - rmin);
        if (try_spawn(w, at.x + ov::cos(a) * r, at.z + ov::sin(a) * r)) done++;
    }
    return done;
}

void Entities::update(float dt, World& w, Player& p, Inventory& inv) {
    for (int i = 0; i < MAX; i++) {
        Entity& e = ents_[i];
        if (!e.alive) continue;
        Vec3 d = e.body.pos - p.pos;
        if (d.x * d.x + d.z * d.z > cfg::DESPAWN * cfg::DESPAWN) { e.alive = false; continue; }
        if (e.type == ENT_ITEM) update_item(e, dt, w, p, inv);
        else update_mob(e, dt, w, p);
    }
    spawn_t_ -= dt;
    if (spawn_t_ <= 0.0f) {
        spawn_t_ = 1.5f;
        if (count(ENT_PIG) < cfg::MAX_PIGS) spawn_near(w, p.pos, cfg::SPAWN_MIN, cfg::SPAWN_MAX, 1);
    }
}

void Entities::update_mob(Entity& e, float dt, World& w, Player& p) {
    const EntityInfo& ei = ENTITIES[e.type];
    if (e.hurt > 0.0f) e.hurt -= dt;
    if (e.invuln > 0.0f) e.invuln -= dt;
    phys::Body& b = e.body;
    bool in_water = w.get(ifloor(b.pos.x), ifloor(b.pos.y + b.h * 0.5f), ifloor(b.pos.z)) == WATER;

    if (e.death > 0.0f) {
        // Падает на бок, через секунду исчезает и роняет добычу
        e.death += dt;
        if (e.death >= 1.0f) {
            e.alive = false;
            int n = ei.drop_min + (int)(rnd() * (float)(ei.drop_max - ei.drop_min + 1));
            if (ei.drop && n > 0)
                drop(b.pos + Vec3(0, 0.4f, 0), Vec3(rnd() - 0.5f, 3.0f, rnd() - 0.5f), ei.drop, n, 0.5f);
            return;
        }
        b.vel.x *= 0.9f; b.vel.z *= 0.9f;
    } else {
        // Решения: постоять, побродить; после удара — бежать куда глаза глядят
        e.ai_t -= dt;
        if (e.panic > 0.0f) {
            e.panic -= dt;
            if (e.ai_t <= 0.0f) { e.target_yaw = (rnd() * 2.0f - 1.0f) * ov::PI; e.ai_t = 0.6f + rnd(); }
            e.walking = true;
        } else if (e.ai_t <= 0.0f) {
            e.walking = rnd() < 0.45f;
            e.target_yaw = (rnd() * 2.0f - 1.0f) * ov::PI;
            e.ai_t = e.walking ? 2.0f + rnd() * 4.0f : 2.0f + rnd() * 5.0f;
        }
        e.yaw = approach_angle(e.yaw, e.target_yaw, dt * (e.panic > 0.0f ? 8.0f : 3.0f));
        float speed = e.walking ? ei.speed * (e.panic > 0.0f ? 2.2f : 1.0f) : 0.0f;
        Vec3 f(ov::sin(e.yaw), 0.0f, -ov::cos(e.yaw));
        float k = b.on_ground ? 10.0f * dt : 2.0f * dt;
        if (k > 1.0f) k = 1.0f;
        b.vel.x += (f.x * speed - b.vel.x) * k;
        b.vel.z += (f.z * speed - b.vel.z) * k;
        // Упёрлась на ходу — перепрыгнуть ступеньку
        if (b.hit_wall && b.on_ground && speed > 0.0f) b.vel.y = cfg::JUMP_SPEED;
        // Голова: на игрока, если он рядом, иначе — куда идёт
        Vec3 to = p.eye() - (b.pos + Vec3(0, b.h * 0.8f, 0));
        float dist = ov::length(to);
        float want_yaw = e.yaw, want_pitch = 0.0f;
        if (dist < 6.0f && e.panic <= 0.0f) {
            // yaw 0 — на -Z: направление (sin yaw, -cos yaw)
            float horiz = ov::sqrt(to.x * to.x + to.z * to.z);
            want_yaw = ov::atan2(to.x, -to.z);
            want_pitch = ov::atan2(to.y, horiz);
            float rel = wrap_angle(want_yaw - e.yaw);
            if (rel > 1.2f) want_yaw = e.yaw + 1.2f;
            if (rel < -1.2f) want_yaw = e.yaw - 1.2f;
        }
        e.head_yaw = approach_angle(e.head_yaw, want_yaw, dt * 4.0f);
        e.head_pitch += (want_pitch - e.head_pitch) * (dt * 4.0f > 1.0f ? 1.0f : dt * 4.0f);
    }

    if (in_water) {
        b.vel.y += (2.0f - b.vel.y) * (dt * 4.0f > 1.0f ? 1.0f : dt * 4.0f);   // всплывает
    } else {
        b.vel.y -= cfg::GRAVITY * dt;
        if (b.vel.y < -cfg::MAX_FALL) b.vel.y = -cfg::MAX_FALL;
    }
    Vec3 before = b.pos;
    phys::move(w, b, b.vel * dt);
    float moved = ov::sqrt((b.pos.x - before.x) * (b.pos.x - before.x) + (b.pos.z - before.z) * (b.pos.z - before.z));
    e.walk_dist += moved;
    float target = moved > 0.002f ? 1.0f : 0.0f;
    e.walk_amount += (target - e.walk_amount) * (dt * 8.0f > 1.0f ? 1.0f : dt * 8.0f);
    if (b.pos.y < -20.0f) e.alive = false;
}

void Entities::update_item(Entity& e, float dt, World& w, Player& p, Inventory& inv) {
    phys::Body& b = e.body;
    e.age += dt;
    if (e.pickup > 0.0f) e.pickup -= dt;
    if (e.age > cfg::ITEM_LIFETIME) { e.alive = false; return; }
    bool in_water = w.get(ifloor(b.pos.x), ifloor(b.pos.y), ifloor(b.pos.z)) == WATER;
    if (in_water) b.vel.y += (1.0f - b.vel.y) * (dt * 3.0f > 1.0f ? 1.0f : dt * 3.0f);
    else b.vel.y -= cfg::GRAVITY * dt;
    // Трение: на земле предмет быстро останавливается
    float fr = b.on_ground ? 8.0f * dt : 0.5f * dt;
    if (fr > 1.0f) fr = 1.0f;
    b.vel.x -= b.vel.x * fr;
    b.vel.z -= b.vel.z * fr;
    phys::move(w, b, b.vel * dt);
    // Застрял в блоке (поставили сверху) — вытолкнуть вверх
    if (phys::collides(w, b.pos, b.hw, b.h)) b.pos.y += 4.0f * dt;

    if (e.pickup <= 0.0f && !p.dead) {
        // Как в оригинале: коробка игрока, расширенная на блок вбок и на полблока вверх-вниз
        float r = cfg::PICKUP_RANGE;
        if (ov::fabs(b.pos.x - p.pos.x) < 0.3f + r && ov::fabs(b.pos.z - p.pos.z) < 0.3f + r &&
            b.pos.y + b.h > p.pos.y - 0.5f && b.pos.y < p.pos.y + p.height() + 0.5f) {
            e.count = inv.add(e.item, e.count);
            if (e.count <= 0) e.alive = false;
        }
    }
}

int Entities::raycast(const Vec3& from, const Vec3& dir, float max_dist, float* dist) const {
    int best = -1;
    float bd = max_dist;
    for (int i = 0; i < MAX; i++) {
        const Entity& e = ents_[i];
        if (!e.alive || e.type == ENT_ITEM || e.death > 0.0f) continue;
        const phys::Body& b = e.body;
        float t = ray_box(from, dir, Vec3(b.pos.x - b.hw, b.pos.y, b.pos.z - b.hw), Vec3(b.pos.x + b.hw, b.pos.y + b.h, b.pos.z + b.hw));
        if (t >= 0.0f && t < bd) { bd = t; best = i; }
    }
    if (dist) *dist = bd;
    return best;
}

void Entities::attack(int i, int damage, const Vec3& from) {
    if (i < 0 || i >= MAX) return;
    Entity& e = ents_[i];
    if (!e.alive || e.invuln > 0.0f || e.death > 0.0f) return;
    e.health -= damage;
    e.hurt = 0.4f;
    e.invuln = 0.5f;
    e.panic = 5.0f;
    e.ai_t = 0.0f;
    // Отбрасывает от удара
    Vec3 d = e.body.pos - from;
    d.y = 0.0f;
    float len = ov::length(d);
    if (len > 1e-3f) d = d * (1.0f / len);
    e.body.vel.x += d.x * 6.0f;
    e.body.vel.z += d.z * 6.0f;
    e.body.vel.y = 5.0f;
    if (e.health <= 0) { e.health = 0; e.death = 0.001f; }
}

void Entities::draw_mob(ov::Renderer& r, const Entity& e) const {
    const Model& m = models_[e.type];
    const EntityInfo& ei = ENTITIES[e.type];
    Model::Pose pose;
    Mat4 base = Model::world_matrix(e.body.pos, e.yaw, ei.scale);
    if (e.death > 0.0f) {
        // На бок: поворот вокруг оси взгляда, точка — у земли
        float a = (e.death > 0.5f ? 1.0f : e.death * 2.0f) * 1.5708f;
        base = Mat4::translate(e.body.pos) * Mat4::rotateY(-e.yaw) * Mat4::rotateZ(a) * Mat4::scale(ei.scale / 16.0f);
    }
    float sw = ov::sin(e.walk_dist * 3.2f) * 0.8f * e.walk_amount;
    for (int i = 0; i < m.parts(); i++) {
        const char* n = m.part(i).name;
        if (n[0] == 'h') {                                   // head
            pose.rot[i].y = -wrap_angle(e.head_yaw - e.yaw);
            pose.rot[i].x = e.head_pitch;
        } else if (n[0] == 'l' && n[1] == 'e' && n[2] == 'g') {   // leg0..leg3: по диагонали в такт
            int k = n[3] - '0';
            pose.rot[i].x = (k == 0 || k == 3) ? sw : -sw;
        }
    }
    m.draw(r, base, pose, e.hurt > 0.0f || e.death > 0.0f);
}

void Entities::draw(ov::Renderer& r, const Vec3& eye) const {
    const float far2 = (float)(cfg::VIEW_RADIUS * cfg::CHUNK) * (float)(cfg::VIEW_RADIUS * cfg::CHUNK);
    for (int i = 0; i < MAX; i++) {
        const Entity& e = ents_[i];
        if (!e.alive) continue;
        Vec3 d = e.body.pos - eye;
        if (d.x * d.x + d.z * d.z > far2) continue;
        if (e.type != ENT_ITEM) {
            if (has_model_[e.type]) draw_mob(r, e);
            continue;
        }
        // Предмет: крутится и покачивается над землёй
        float spin = e.age * 1.6f + e.yaw;
        float bob = ov::sin(e.age * 2.5f + e.yaw) * 0.05f + 0.12f;
        bool block = item_is_block(e.item);
        float s = block ? 0.25f : 0.4f;
        int copies = e.count > 16 ? 3 : (e.count > 1 ? 2 : 1);
        for (int k = 0; k < copies; k++) {
            Vec3 off((float)k * 0.06f, (float)k * 0.05f, (float)k * 0.04f);
            Mat4 m = Mat4::translate(e.body.pos + Vec3(0, bob + s * 0.5f, 0) + off) * Mat4::rotateY(spin) * Mat4::scale(s);
            r.draw(shapes::held(e.item), m);
        }
    }
}

void Entities::draw_player(ov::Renderer& r, const Player& p, uint16_t held, float swing) const {
    if (!has_model_[ENT_PLAYER]) return;
    const Model& m = models_[ENT_PLAYER];
    const EntityInfo& ei = ENTITIES[ENT_PLAYER];
    Mat4 base = Model::world_matrix(p.pos, p.body_yaw, ei.scale);
    Model::Pose pose;
    float ph = p.walk_dist * 2.4f;
    float sw = ov::sin(ph) * 0.9f * p.walk_amount;
    int ra = -1;
    for (int i = 0; i < m.parts(); i++) {
        const char* n = m.part(i).name;
        Vec3& rot = pose.rot[i];
        Vec3& off = pose.off[i];
        bool right = n[0] == 'r', left = n[0] == 'l';
        bool arm = (right && n[6] == 'a') || (left && n[5] == 'a');
        bool leg = (right && n[6] == 'l') || (left && n[5] == 'l');
        if (n[0] == 'h') {
            rot.y = -wrap_angle(p.yaw - p.body_yaw);
            rot.x = p.pitch;
        } else if (arm) {
            rot.x = right ? -sw : sw;
            if (right) {
                ra = i;
                if (held) rot.x = rot.x * 0.5f + 0.3f;           // держит предмет — рука чуть вперёд
                rot.x += ov::sin(swing * ov::PI) * 1.4f;         // взмах
            }
            rot.z = right ? 0.05f : -0.05f;
        } else if (leg) {
            rot.x = right ? sw : -sw;
        }
        if (p.sneaking) {
            // Присел: корпус наклонён, ноги назад, голова и руки ниже
            if (n[0] == 'b') rot.x = -0.5f;
            if (arm) rot.x -= 0.4f;
            if (leg) off.z = 4.0f;
            if (n[0] == 'h' || arm || n[0] == 'b') off.y = -1.0f;
        }
    }
    m.draw(r, base, pose, p.hurt_time > 0.0f);
    // Предмет в правой руке: у кисти (пиксели части — от плеча)
    if (held && ra >= 0) {
        Mat4 hm = m.part_matrix(base, pose, ra) * Mat4::translate(Vec3(0.0f, -10.0f, -2.0f));
        if (item_is_block(held)) hm = hm * Mat4::rotateY(0.78f) * Mat4::scale(6.0f);
        else hm = hm * Mat4::rotateX(-1.2f) * Mat4::rotateY(1.5708f) * Mat4::scale(10.0f);
        r.draw(shapes::held(held), hm);
    }
}
