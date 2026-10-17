// =============================================================================
// player.cpp — движение, физика и выживание игрока (см. player.h).
// =============================================================================

#include "player.h"
#include "config.h"
#include "blocks.h"
#include "world/world.h"
#include "game/physics.h"

using namespace cfg;
using ov::Vec3;
using phys::ifloor;

void Player::spawn(World& w, int x, int z) {
    int top = w.surface(x, z);
    if (top < SEA_LEVEL) top = SEA_LEVEL;
    pos = Vec3((float)x + 0.5f, (float)top + 1.05f, (float)z + 0.5f);
    vel = Vec3();
    on_ground = false;
    fall_ = 0.0f;
    spawn_x_ = x;
    spawn_z_ = z;
}

void Player::respawn(World& w) {
    spawn(w, spawn_x_, spawn_z_);
    health = MAX_HEALTH;
    food = MAX_FOOD;
    saturation = START_SATURATION;
    exhaustion = 0.0f;
    air = AIR_SECONDS;
    hurt_time = invuln_ = 0.0f;
    dead = false;
    flying = false;
    sprinting = sneaking = false;
}

void Player::set_creative(bool on) {
    creative = on;
    if (!on) flying = false;
    fall_ = 0.0f;
}

Vec3 Player::forward() const {
    float cp = ov::cos(pitch);
    return Vec3(ov::sin(yaw) * cp, ov::sin(pitch), -ov::cos(yaw) * cp);
}

float Player::height() const { return sneaking ? PLAYER_H_SNEAK : PLAYER_H; }

void Player::look(const Input& in) {
    yaw += (float)in.dx() * MOUSE_SENS;
    pitch -= (float)in.dy() * MOUSE_SENS;
    if (pitch > 1.55f) pitch = 1.55f;
    if (pitch < -1.55f) pitch = -1.55f;
    if (yaw > ov::PI) yaw -= 2.0f * ov::PI;
    if (yaw < -ov::PI) yaw += 2.0f * ov::PI;
}

bool Player::intersects_block(int x, int y, int z) const {
    float h = PLAYER_W * 0.5f;
    return (float)x < pos.x + h && (float)(x + 1) > pos.x - h &&
           (float)y < pos.y + height() && (float)(y + 1) > pos.y &&
           (float)z < pos.z + h && (float)(z + 1) > pos.z - h;
}

bool Player::damage(int amount) {
    if (creative || dead || amount <= 0 || invuln_ > 0.0f) return false;
    health -= amount;
    hurt_time = 0.5f;
    invuln_ = INVULN_TIME;
    add_exhaustion(0.1f);
    if (health <= 0) { health = 0; dead = true; sprinting = false; }
    return true;
}

void Player::eat(int points, float sat) {
    food += points;
    if (food > MAX_FOOD) food = MAX_FOOD;
    saturation += sat;
    if (saturation > (float)food) saturation = (float)food;     // насыщение не больше голода
}

void Player::update(float dt, const Input& in, World& w) {
    clock_ += dt;
    if (hurt_time > 0.0f) hurt_time -= dt;
    if (invuln_ > 0.0f) invuln_ -= dt;
    if (dead) return;
    // Двойной пробел — полёт (только в творческом)
    if (in.pressed(key::SPACE)) {
        if (creative && clock_ - last_space_ < 0.3f) { flying = !flying; vel.y = 0.0f; last_space_ = -1.0f; }
        else last_space_ = clock_;
    }
    if (creative && in.pressed(key::F)) { flying = !flying; vel.y = 0.0f; }

    // Присесть: на Shift (в полёте Shift — вниз). Встать — только если над головой есть место
    bool want_sneak = in.down(key::SHIFT) && !flying;
    if (want_sneak) sneaking = true;
    else if (sneaking && !phys::collides(w, pos, PLAYER_W * 0.5f, PLAYER_H)) sneaking = false;

    // Бег: Ctrl или двойное W, пока идём вперёд; сил нет — не бежится
    bool fwd = in.down(key::W);
    bool can_sprint = fwd && !sneaking && (creative || food >= SPRINT_MIN_FOOD);
    if (in.pressed(key::W)) {
        if (clock_ - last_w_ < 0.3f && can_sprint) sprinting = true;
        last_w_ = clock_;
    }
    if (in.down(key::CTRL) && can_sprint) sprinting = true;
    if (!can_sprint) sprinting = false;

    acc_ += dt;
    if (acc_ > 0.25f) acc_ = 0.25f;                     // окно висело — не догоняем секундами
    while (acc_ >= PHYS_DT) {
        step(in, w);
        acc_ -= PHYS_DT;
    }
    // Глаза опускаются при приседании плавно, а не скачком
    float target = sneaking ? EYE_H_SNEAK : EYE_H;
    float k = dt * 12.0f > 1.0f ? 1.0f : dt * 12.0f;
    eye_h_ += (target - eye_h_) * k;

    // Тело догоняет голову: на ходу сразу, на месте — когда отстало больше чем на 50°
    float d = yaw - body_yaw;
    while (d > ov::PI) d -= 2.0f * ov::PI;
    while (d < -ov::PI) d += 2.0f * ov::PI;
    float lim = 0.87f;
    if (walk_amount > 0.2f) body_yaw += d * (dt * 8.0f > 1.0f ? 1.0f : dt * 8.0f);
    else if (d > lim) body_yaw += d - lim;
    else if (d < -lim) body_yaw += d + lim;

    vitals(dt, w);
    if (pos.y < -20.0f) {                              // выпали из мира
        if (creative) spawn(w, ifloor(pos.x), ifloor(pos.z));
        else { health = 0; dead = true; }
    }
}

void Player::step(const Input& in, World& w) {
    const float dt = PHYS_DT;
    // Куда хотим: относительно взгляда, без наклона
    Vec3 fwd(ov::sin(yaw), 0.0f, -ov::cos(yaw));
    Vec3 right(ov::cos(yaw), 0.0f, ov::sin(yaw));
    Vec3 wish;
    if (in.down(key::W)) wish += fwd;
    if (in.down(key::S)) wish -= fwd;
    if (in.down(key::D)) wish += right;
    if (in.down(key::A)) wish -= right;
    float len = ov::length(wish);
    if (len > 0.0f) wish = wish * (1.0f / len);

    uint8_t at = w.get(ifloor(pos.x), ifloor(pos.y + 0.4f), ifloor(pos.z));
    in_water = at == WATER;
    head_in_water = w.get(ifloor(pos.x), ifloor(pos.y + eye_h_), ifloor(pos.z)) == WATER;

    float speed = flying ? (sprinting ? FLY_SPEED * 2.0f : FLY_SPEED)
                         : (sneaking ? SNEAK_SPEED : (sprinting ? SPRINT_SPEED : WALK_SPEED));
    if (in_water && !flying) speed *= 0.55f;
    // Разгон: на земле резко, в воздухе вяло — как в любой игре от первого лица
    float k = flying ? 10.0f : (on_ground ? 14.0f : 3.0f);
    float t = k * dt > 1.0f ? 1.0f : k * dt;
    vel.x += (wish.x * speed - vel.x) * t;
    vel.z += (wish.z * speed - vel.z) * t;

    if (flying) {
        float up = (in.down(key::SPACE) ? 1.0f : 0.0f) - (in.down(key::SHIFT) ? 1.0f : 0.0f);
        vel.y += (up * FLY_SPEED - vel.y) * t;
    } else if (in_water) {
        vel.y -= GRAVITY * 0.25f * dt;
        if (in.down(key::SPACE)) vel.y = 3.2f;                   // всплыть
        if (vel.y < -3.0f) vel.y = -3.0f;
    } else {
        vel.y -= GRAVITY * dt;
        if (vel.y < -MAX_FALL) vel.y = -MAX_FALL;
        if (on_ground && in.down(key::SPACE)) {
            vel.y = JUMP_SPEED;
            if (sprinting) { vel.x += fwd.x * SPRINT_JUMP_BOOST; vel.z += fwd.z * SPRINT_JUMP_BOOST; }
            add_exhaustion(sprinting ? 0.2f : 0.05f);
        }
    }

    phys::Body b;
    b.pos = pos; b.vel = vel;
    b.hw = PLAYER_W * 0.5f; b.h = height();
    Vec3 d = vel * dt;
    // Присев, с края не падаем: шаг, после которого опоры нет, отменяется по своей оси
    if (sneaking && on_ground && !flying) {
        if (!phys::ground_below(w, Vec3(pos.x + d.x, pos.y, pos.z), b.hw)) { d.x = 0.0f; b.vel.x = 0.0f; }
        if (!phys::ground_below(w, Vec3(pos.x + d.x, pos.y, pos.z + d.z), b.hw)) { d.z = 0.0f; b.vel.z = 0.0f; }
    }
    bool was_ground = on_ground;
    float y0 = pos.y;
    phys::move(w, b, d);
    float moved = ov::sqrt((b.pos.x - pos.x) * (b.pos.x - pos.x) + (b.pos.z - pos.z) * (b.pos.z - pos.z));
    pos = b.pos; vel = b.vel; on_ground = b.on_ground;
    if (b.hit_wall && sprinting && !flying) sprinting = false;   // упёрся в стену — бег кончился

    // Шаги для анимации и истощение от бега
    if (on_ground && !flying) walk_dist += moved;
    float target = (on_ground || in_water) && moved > 0.001f ? 1.0f : 0.0f;
    walk_amount += (target - walk_amount) * 0.08f;
    if (!flying) add_exhaustion((sprinting ? 0.1f : (in_water ? 0.01f : 0.0f)) * moved);

    // Падение: копим высоту, на земле — урон за всё, что выше трёх блоков
    if (flying || in_water || creative) fall_ = 0.0f;
    else if (!on_ground && pos.y < y0) fall_ += y0 - pos.y;
    if (on_ground && !was_ground) {
        int dmg = (int)(fall_ - SAFE_FALL + 0.999f);
        if (dmg > 0) damage(dmg);
        fall_ = 0.0f;
    }
    if (flying && on_ground) flying = false;                     // приземлились
}

void Player::vitals(float dt, World& w) {
    (void)w;
    if (creative) { air = AIR_SECONDS; return; }
    // Истощение: каждые 4 единицы съедают очко насыщения, а без него — голода
    while (exhaustion >= 4.0f) {
        exhaustion -= 4.0f;
        if (saturation > 0.0f) { saturation -= 1.0f; if (saturation < 0.0f) saturation = 0.0f; }
        else if (food > 0) food--;
    }
    // Сытый лечится, голодный слабеет (до половины сердца — как на «нормальной» сложности)
    if (food >= 18 && health < MAX_HEALTH) {
        regen_t_ += dt;
        if (regen_t_ >= REGEN_EVERY) { regen_t_ = 0.0f; health++; add_exhaustion(6.0f); }
    } else regen_t_ = 0.0f;
    if (food <= 0) {
        starve_t_ += dt;
        if (starve_t_ >= STARVE_EVERY) { starve_t_ = 0.0f; if (health > 1) damage(1); }
    } else starve_t_ = 0.0f;
    // Воздух: под водой тратится, кончился — по 2 очка в секунду
    if (head_in_water) {
        air -= dt;
        if (air <= 0.0f) {
            air = 0.0f;
            drown_t_ += dt;
            if (drown_t_ >= 1.0f) { drown_t_ = 0.0f; damage(2); }
        }
    } else {
        air += dt * 5.0f;
        if (air > AIR_SECONDS) air = AIR_SECONDS;
        drown_t_ = 0.0f;
    }
}
