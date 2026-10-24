#include "game/combat.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "audio/audio.h"
#include "game/anim_sounds.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompBreakable = 0x13;
constexpr int kCompDamage = 0x99;
constexpr int kCompBuiltinBox = 0xa6;
constexpr int kLightningGunType = 9007;   // AlienLightningGun_Player: a longer hit marker
constexpr int kCompHealth = 0x11;
constexpr int kCompAirNavigation = 0x1b;
constexpr int kCompGroundNavigation = 0x1d;
constexpr int kCompAwareness = 0x1f;
constexpr int kCompProjectile = 0x28;
constexpr int kCompWeapon = 0x47;
constexpr int kCompNpcWeapon = 0x48;
constexpr int kCompNpc = 0x4b;
constexpr int kCompAi = 0x4e;
constexpr int kCompPowerUp = 0x4a;
constexpr int kCompActorsCollision = 0x0d;
constexpr int kCompTargetable = 0x8d;
constexpr int kPlayerTeamBit = 1 << 1;   // the player's team (1) in awareness masks
constexpr int kCompSpawnManager = 0x62;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The first clip (in `keys` order) whose name contains a key.
std::string pick_clip(const std::vector<std::string>& clips, std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        for (const std::string& c : clips)
            if (lower(c).find(k) != std::string::npos) return c;
    return {};
}

float num(const Value& v, size_t i, float fallback) {
    if (i >= v.size()) return fallback;
    const Value& x = v[i];
    return x.type == Value::Float || x.type == Value::Int ? x.as_float() : fallback;
}

std::string str(const Value& v, size_t i) {
    return i < v.size() && v[i].type == Value::String ? v[i].s : std::string();
}

// Effect names the targetable components use that the library spells differently.
std::string effect_name(const std::string& name) {
    return name == "bloody_blood" ? "impact_blood_red" : name;
}

// gol sound lists: [[name], ...] or [[name, volume], ...].
std::vector<std::string> sound_list(const Value& v) {
    std::vector<std::string> out;
    for (const Value& e : v.list) {
        if (e.type == Value::String && !e.s.empty()) out.push_back(e.s);
        else if (e.size() > 0 && e[0].type == Value::String && !e[0].s.empty()) out.push_back(e[0].s);
    }
    return out;
}

// CProjectileComponent [18] material -> effect, [20] material -> sounds, [21] -> decal.
Impacts read_impacts(const Value& pr) {
    Impacts im;
    if (pr.size() > 18)
        for (const Value& e : pr[18].list)
            if (e.size() >= 2 && e[1].type == Value::String) im.effects[e[0].i] = e[1].s;
    if (pr.size() > 20)
        for (const Value& e : pr[20].list)
            if (e.size() >= 2) im.sounds[e[0].i] = sound_list(e[1]);
    if (pr.size() > 21)   // [21]: material -> decal cell, size
        for (const Value& e : pr[21].list)
            if (e.size() >= 3) im.decals.entries.push_back({e[0].i, e[1].i, e[2].as_float()});
    return im;
}

float yaw_towards(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec2 d(to - from);
    return glm::degrees(std::atan2(-d.x, d.y));   // yaw 0 faces +Y, positive turns left
}

float angle_diff(float a, float b) {   // b - a in (-180, 180]
    float d = std::fmod(b - a, 360.0f);
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    return d;
}

// Additive effect quad from `a` to `b`, `width` wide, facing `eye`.
void beam(std::vector<ColorVertex>& out, const glm::vec3& a, const glm::vec3& b, float width, const glm::vec4& ca,
          const glm::vec4& cb, const glm::vec3& eye) {
    const glm::vec3 dir = b - a;
    glm::vec3 side = glm::cross(dir, eye - (a + b) * 0.5f);
    if (glm::length(side) < 1e-6f) return;
    side = glm::normalize(side) * (width * 0.5f);
    const uint32_t pa = pack_rgba(ca), pb = pack_rgba(cb);
    out.insert(out.end(), {{a - side, pa}, {a + side, pa}, {b + side, pb}, {a - side, pa}, {b + side, pb}, {b - side, pb}});
}

void glow(std::vector<ColorVertex>& out, const glm::vec3& p, float size, const glm::vec4& c, const glm::vec3& eye) {
    const glm::vec3 view = glm::normalize(p - eye);
    glm::vec3 right = glm::cross(view, glm::vec3(0, 0, 1));
    if (glm::length(right) < 1e-4f) right = glm::vec3(1, 0, 0);
    right = glm::normalize(right) * size;
    const glm::vec3 up = glm::normalize(glm::cross(right, view)) * size;
    const uint32_t centre = pack_rgba(c), edge = pack_rgba(glm::vec4(glm::vec3(c), 0));
    const glm::vec3 ring[4] = {p + right, p + up, p - right, p - up};
    for (int k = 0; k < 4; ++k) out.insert(out.end(), {{p, centre}, {ring[k], edge}, {ring[(k + 1) % 4], edge}});
}

}  // namespace

Combat::Combat(Scene& scene, Player& player, LevelLogic& logic, const Gol& gol)
    : scene_(scene), player_(player), logic_(logic), gol_(gol), ragdolls_(RagdollSettings::load(gol)) {
    // The player's arsenal (CWeaponManager), in the weapon panel's order; the campaign
    // starts with the assault rifle and scripts unlock the rest (UnlockWeapon).
    for (const char* name : {"AssaultRifle_Player", "AutomaticShotGun_Player", "Handgun_Player", "SniperRifle_Player",
                             "RocketLauncher_Player", "GrenadeGun_Player", "FlameThrower_Player", "AlienWeapon_Player",
                             "RailGun_Player", "AlienLightningGun_Player", "ExecutorPlasmaGun_Player", "Crystal_ItemWeapon"})
        weapons_.push_back(load_weapon(name)), weapon_types_.push_back(name);
    for (const WeaponDef& w : weapons_) {
        clip_.push_back(w.clip);
        reserve_.push_back(w.reserve);
        owned_.push_back(0);
    }
    owned_[0] = 1;
    // Not in the arsenal: the jeep's turret and the mounted machine guns.
    for (const char* name : {"VehicleWeaponPlayer", "MG_Player", "Player_Mech_Cockpit"}) {
        weapons_.push_back(load_weapon(name)), weapon_types_.push_back(name);
        clip_.push_back(weapons_.back().clip), reserve_.push_back(-1), owned_.push_back(0);
    }
    vehicle_weapon_ = int(weapons_.size()) - 3, mounted_gun_ = int(weapons_.size()) - 2, mech_weapon_ = int(weapons_.size()) - 1;
    // The player: CHealthComponent and CPlayerBaseComponent (regeneration per second, delay).
    for (const auto& [id, type] : gol_) {
        if (type.name != "Player") continue;
        if (const Value* h = resolved_template(type, gol_, kCompHealth)) player_max_health = num(*h, 0, 150);
        // CComponentPlayerComponent: falls (gravity, the fastest fall), the jump, fall damage
        // ([7] the speed it starts at, [4] per unit over) and the landing clips' speeds.
        if (const Value* pc = resolved_template(type, gol_, 0x2a); pc && pc->size() > 15) {
            player_.gravity = -num(*pc, 13, -9.8f), player_.max_fall_speed = num(*pc, 6, 30);
            player_.jump_speed = num(*pc, 14, 8), player_.jump_time = num(*pc, 15, 0.8f);
            player_.step_min_ms = num(*pc, 71, 400), player_.step_max_ms = num(*pc, 72, 600);
            player_.rush_limit = num(*pc, 21, 2.5f), player_.rush_recover = num(*pc, 22, 3), player_.rush_restart = num(*pc, 23, 1);
            fall_damage_from_ = num(*pc, 7, 15), fall_damage_per_ = num(*pc, 4, 16);
            landing_speeds_ = {num(*pc, 8, 5), num(*pc, 9, 15), num(*pc, 10, 20)};
        }
        if (const Value* b = resolved_template(type, gol_, 0x29)) {
            if (b->size() > 5) player_hurt_sounds_ = sound_list((*b)[5]);
            regen_rate_ = num(*b, 1, 20);
            regen_pause_ = num(*b, 2, 3000) / 1000.0f;
            view_hurt_pause_ = num(*b, 3, 0) / 1000.0f, view_hurt_heavy_ = num(*b, 4, 0);
            base_shield_percent_ = num(*b, 0, 50);
        }
    }
    base_max_health_ = player_max_health, base_regen_pause_ = regen_pause_;
    for (auto& row : difficulty_factor_) row[0] = row[1] = row[2] = 1;
    if (auto it = gol_.find(4509); it != gol_.end())
        if (const Value* v = resolved_template(it->second, gol_, 0x3d))
            for (size_t list = 0; list < 7 && list < v->size(); ++list)
                for (size_t d = 0; d < 3 && d < (*v)[list].size(); ++d) {
                    const Value& f = (*v)[list][d];
                    difficulty_factor_[list][d] = f.size() > 0 ? f[0].as_float() : f.as_float();
                }
    player_health = player_max_health;
    load_grenade();
}

void Combat::set_difficulty(int d) {
    // CHealthComponent::ReInit (the player's maximum) and CPlayerBaseComponent::Init /
    // StopHealthRegeneration (the pause before regenerating).
    difficulty_ = std::clamp(d, 0, 2);
    const bool full = player_health >= player_max_health;
    player_max_health = base_max_health_ * difficulty_factor_[1][difficulty_];
    regen_pause_ = base_regen_pause_ * difficulty_factor_[3][difficulty_];
    if (full || player_health > player_max_health) player_health = player_max_health;
}

Combat::GrenadeDef Combat::read_grenade(const Gol& gol, int id) {
    GrenadeDef d;
    auto it = gol.find(id);
    if (it == gol.end()) return d;
    d.type = id;
    // CComponentGrenade: fuse (ms), damage, ?, damage radius, throw speed, ?, sticky, ...,
    // explosion effect, explosion object, ..., bounce sound, ..., explosion sounds.
    if (const Value* g = resolved_template(it->second, gol, 0x26); g && g->size() > 18) {
        d.fuse = num(*g, 0, 2300) / 1000.0f;
        d.damage = num(*g, 1, 40);
        d.radius = num(*g, 3, 10);
        d.speed = num(*g, 4, 25);
        d.sticky = (*g)[6].i != 0;
        if (!str(*g, 10).empty()) d.effect = str(*g, 10);
        d.bounce_sound = str(*g, 15);
        d.explode_sounds = sound_list((*g)[18]);
        if (g->size() > 19)
            for (const Value& e : (*g)[19].list)
                if (e.size() >= 3) d.decals.entries.push_back({e[0].i, e[1].i, e[2].as_float()});
        for (const Value& e : (*g)[12].list)
            if (e.size() > 0 && e[0].i == 22) d.burst_on_actor = true;
    }
    if (const Value* fx = resolved_template(it->second, gol, 0x92); fx && fx->size() > 0 && (*fx)[0].size() > 0)
        d.trail = str((*fx)[0][0], 0);
    return d;
}

void Combat::load_grenade() {
    for (const auto& [id, type] : gol_) {
        if (type.name == "Grenade_Player") grenade_def_ = read_grenade(gol_, id);
        if (type.name == "NPC_Grenade") npc_grenade_def_ = read_grenade(gol_, id);
        if (type.name == "Mine_Player") mine_def_ = read_grenade(gol_, id);
    }
}

void Combat::throw_grenade(const glm::vec3& eye, const glm::vec3& aim) {
    // Out of the right hand, along the view and a little up.
    const glm::vec3 right = glm::normalize(glm::cross(aim, glm::vec3(0, 0, 1)) + glm::vec3(1e-4f));
    glm::vec3 at = eye + aim * 0.4f + right * 0.2f - glm::vec3(0, 0, 0.15f);
    float t = 1;
    if (scene_.collision().raycast(eye, at, t)) at = eye;   // too close to a wall
    const GrenadeDef& def = throwing_mine_ ? mine_def_ : grenade_def_;
    launch_grenade(def, at, aim * (def.speed * 0.6f) + glm::vec3(0, 0, 3.5f), false);
    if (logic_.trace) log(std::string(throwing_mine_ ? "mine" : "grenade") + " thrown (type " + std::to_string(def.type) + ")");
}

bool Combat::mines_placed() const {
    for (const Grenade& g : live_grenades_)
        if (g.live && g.def == &mine_def_) return true;
    return false;
}

void Combat::detonate() {
    // CWeaponComponent::Detonate raises EvDetonate; every mine of the player's goes off.
    ++detonations_;
    for (Grenade& g : live_grenades_) {
        if (!g.live || g.def != &mine_def_) continue;
        g.live = false;
        scene_.set_object_visible(g.object, false);
        if (g.trail >= 0) scene_.particles().stop(g.trail), g.trail = -1;
        explode(g);
    }
}

void Combat::launch_grenade(const GrenadeDef& def, const glm::vec3& at, const glm::vec3& vel, bool npc) {
    if (def.type < 0) return;
    Grenade* g = nullptr;
    for (Grenade& x : live_grenades_)
        if (!x.live && x.def == &def) g = &x;
    if (!g) {
        live_grenades_.push_back({});
        g = &live_grenades_.back();
        g->object = scene_.spawn_object(def.type, -3000 - int(live_grenades_.size()), at, 0);
    }
    g->def = &def, g->npc = npc;
    g->pos = at, g->vel = vel;
    g->fuse = def.fuse;
    g->bounce_wait = 0, g->stuck = false;
    g->live = true;
    scene_.set_object_visible(g->object, true);
    scene_.move_object(g->object, g->pos);
    g->trail = def.trail.empty() ? -1 : scene_.particles().start(def.trail, glm::translate(glm::mat4(1), g->pos));
}

void Combat::npc_throw_grenade(Target& t, const glm::vec3& target) {
    if (t.grenades <= 0) return;
    --t.grenades;
    const float y = glm::radians(t.yaw);
    const glm::vec3 hand = t.pos + glm::vec3(0, 0, t.stand_height * 0.9f) + glm::vec3(-std::sin(y), std::cos(y), 0) * (t.radius + 0.2f);
    // A lob landing at the target: across in a time set by the throw speed, with gravity.
    constexpr float kGravity = 12.0f;
    const glm::vec3 d = target - hand;
    const float across = glm::length(glm::vec2(d));
    const float time = std::clamp(across / std::max(npc_grenade_def_.speed * 0.6f, 1.0f), 0.6f, 2.5f);
    const glm::vec3 vel(d.x / time, d.y / time, (d.z + 0.5f * kGravity * time * time) / time);
    launch_grenade(npc_grenade_def_, hand, vel, true);
    if (logic_.trace) log(std::to_string(t.id) + " throws a grenade (" + std::to_string(t.grenades) + " left)");
}

std::vector<glm::vec3> Combat::grenade_threats() const {
    std::vector<glm::vec3> out;
    for (const Grenade& g : live_grenades_)
        if (g.live && g.npc && g.def && glm::distance(g.pos, player_center()) < g.def->radius) out.push_back(g.pos);
    return out;
}

void Combat::update_grenades(float dt) {
    constexpr float kGravity = 12.0f;
    for (Grenade& g : live_grenades_) {
        if (!g.live || g.stuck) continue;
        g.bounce_wait = std::max(g.bounce_wait - dt, 0.0f);
        g.vel.z -= kGravity * dt;
        const glm::vec3 to = g.pos + g.vel * dt;
        float t = 1;
        glm::vec3 n(0, 0, 1);
        if (glm::length(g.vel) > 1e-3f && scene_.collision().raycast(g.pos, to, t, nullptr, &n)) {
            g.pos = glm::mix(g.pos, to, t) + n * 0.05f;
            if (g.def->sticky) {   // a mine: set on the surface, turned to face out of it
                g.stuck = true, g.vel = glm::vec3(0);
                const glm::vec3 side = std::abs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
                const glm::vec3 x = glm::normalize(glm::cross(side, n)), y = glm::cross(n, x);
                scene_.set_object_transform(g.object, glm::mat4(glm::vec4(x, 0), glm::vec4(y, 0), glm::vec4(n, 0), glm::vec4(g.pos, 1)));
                if (g.trail >= 0) scene_.particles().stop(g.trail), g.trail = -1;
                sound(g.def->bounce_sound, &g.pos);   // sfx_detonator_hit_ground_attach
                if (logic_.trace) {
                    char line[128];
                    std::snprintf(line, sizeof line, "mine stuck at (%.1f %.1f %.1f), facing (%.2f %.2f %.2f)", g.pos.x, g.pos.y, g.pos.z, n.x, n.y, n.z);
                    log(line);
                }
                continue;
            }
            const float speed = glm::length(g.vel);
            g.vel = glm::reflect(g.vel, n) * 0.4f;
            if (n.z > 0.7f && glm::length(g.vel) < 1.0f) g.vel = glm::vec3(0);   // at rest
            if (speed > 2.0f && g.bounce_wait <= 0) sound(g.def->bounce_sound, &g.pos), g.bounce_wait = 0.15f;
        } else {
            g.pos = to;
        }
        bool burst = false;
        if (g.def->burst_on_actor)
            for (const Target& t : targets_)
                if (t.actor && alive(t) && glm::length(glm::vec2(g.pos - t.pos)) < t.radius && g.pos.z > t.pos.z && g.pos.z < t.pos.z + t.height)
                    burst = true;
        g.spin += glm::length(g.vel) * dt * 4.0f;
        scene_.set_object_transform(g.object, glm::rotate(glm::translate(glm::mat4(1), g.pos), g.spin, glm::vec3(1, 0, 0)));
        if (g.trail >= 0) scene_.particles().move(g.trail, glm::translate(glm::mat4(1), g.pos));
        if (!g.def->sticky && ((g.fuse -= dt) <= 0 || burst)) {
            g.live = false;
            scene_.set_object_visible(g.object, false);
            if (g.trail >= 0) scene_.particles().stop(g.trail), g.trail = -1;
            explode(g);
        }
    }
}

bool Combat::unlock_power(const std::string& name, bool on) {
    std::string n = lower(name);
    if (n == "jetpack") return false;   // the level logic's
    for (const char* known : {"Repel", "Paralyze", "Slow"}) {
        if (lower(known) != n) continue;
        auto it = std::find(powers_.begin(), powers_.end(), known);
        if (on && it == powers_.end()) powers_.push_back(known);
        if (!on && it != powers_.end() && powers_.size() > 1) {
            powers_.erase(it);
            power_index_ = std::min(power_index_, int(powers_.size()) - 1);
        }
        return true;
    }
    return false;
}

bool Combat::select_power(const std::string& name) {
    for (size_t i = 0; i < powers_.size(); ++i)
        if (lower(powers_[i]) == lower(name)) return power_index_ = int(i), true;
    return false;
}

void Combat::cycle_power() {
    if (!powers_.empty()) power_index_ = (power_index_ + 1) % int(powers_.size());
}

bool Combat::use_power_now(const std::string& name) {
    unlock_power(name, true);
    if (!select_power(name)) return false;
    power_cooldown_ = 0;
    use_selected_power(last_eye_, last_aim_);
    return true;
}

void Combat::use_selected_power(const glm::vec3& eye, const glm::vec3& aim) {
    const std::string& p = power();
    if (p == "Paralyze") paralyze(eye, aim);
    else if (p == "Slow") {
        power_cooldown_ = kPowerCooldown, power_used_ = true;
        logic_.bullet_time(5000, 0.3f);   // the world slows for a while
        sound("sfx_slow_motion_in", nullptr);
    } else {
        repel(eye, aim);
    }
}

void Combat::paralyze(const glm::vec3& eye, const glm::vec3& aim) {
    // Freezes the enemies in front for five seconds (Paralyze_Bullet: 500 m reach).
    power_cooldown_ = kPowerCooldown, power_used_ = true;
    sound("sfx_plasma_hand_shot", nullptr);
    effect("bullet_paralyze", eye + aim * 1.5f, aim);
    // What the shot hits is frozen (any object with health: Boreas' broken doors and power
    // modules too), and the enemies in a narrow cone around it.
    float hit_t;
    glm::vec3 hit_at;
    if (const int hit = ray_target(eye, aim, 500.0f, hit_t, hit_at); hit >= 0 && line_clear(eye, hit_at - aim * 0.2f)) {
        Target& t = targets_[size_t(hit)];
        if (!t.actor || t.hostile) paralyze_target(t);
        if (paralyze_damage > 0 && t.actor && t.hostile) apply_damage(t, paralyze_damage, true);
    }
    for (Target& t : targets_) {
        if (!t.actor || !t.hostile || !alive(t)) continue;
        const glm::vec3 c = t.pos + glm::vec3(0, 0, t.height * 0.5f);
        const glm::vec3 to = c - eye;
        const float d = glm::length(to);
        if (d > 40.0f || d < 1e-3f || glm::dot(to / d, aim) < 0.85f || !line_clear(eye, c)) continue;
        paralyze_target(t);
    }
}

bool Combat::script_invisibility(int npc, bool on) {
    Target* t = find_target(npc);
    if (!t || t->cloak_model.empty()) return false;
    t->script_cloak = on;
    if (on) cloak(*t, true);
    else uncloak(*t);
    return true;
}

void Combat::cloak(Target& t, bool instant) {
    using C = Target::Cloak;
    if (t.cloak == C::Invisible || (t.cloak == C::FadingOut && !instant)) return;
    const glm::vec3 at = t.pos + glm::vec3(0, 0, 1);
    sound(t.cloak_sound, &at);
    if (t.cloak == C::Visible && !scene_.set_object_material(t.object, t.cloak_model, t.cloak_material)) return;
    t.cloak = instant ? C::Invisible : C::FadingOut;
    t.cloak_timer = instant ? std::uniform_real_distribution<float>(t.invisible_min, t.invisible_max)(rng_) : t.cloak_fade;
    scene_.set_object_param(t.object, "BlendFactor", glm::vec4(instant ? -0.5f : 0.5f), 1);
}

void Combat::uncloak(Target& t) {
    using C = Target::Cloak;
    if (t.cloak == C::Visible || t.cloak == C::FadingIn) return;
    const glm::vec3 at = t.pos + glm::vec3(0, 0, 1);
    sound(t.uncloak_sound, &at);
    t.cloak = C::FadingIn, t.cloak_timer = 0;
}

// UpdateInvisibility: hurt enough over the last 5 s (0.8 of the damage that sends it under)
// after its least time seen, it fades out; unseen for its time, it fades back.
void Combat::update_cloak(Target& t, float dt) {
    using C = Target::Cloak;
    if (t.cloak_model.empty()) return;
    std::erase_if(t.recent_damage, [&](const auto& d) { return clock_ - d.first > 5.0f; });
    switch (t.cloak) {
        case C::Visible: {
            if (!alive(t)) break;
            t.visible_left -= dt;
            float hurt = 0;
            for (const auto& d : t.recent_damage) hurt += d.second;
            if (t.auto_cloak && alive(t) && t.visible_left <= 0 && hurt >= t.cloak_damage * 0.8f) cloak(t, false);
            break;
        }
        case C::FadingOut:
            t.cloak_timer -= dt;
            if (t.cloak_timer <= 0) {
                t.cloak = C::Invisible;
                t.cloak_timer = std::uniform_real_distribution<float>(t.invisible_min, t.invisible_max)(rng_);
                scene_.set_object_param(t.object, "BlendFactor", glm::vec4(-0.5f), 1);
            } else {
                scene_.set_object_param(t.object, "BlendFactor", glm::vec4(t.cloak_timer / t.cloak_fade - 0.5f), 1);
            }
            break;
        case C::Invisible:
            if (!t.script_cloak && (t.cloak_timer -= dt) <= 0) uncloak(t);
            break;
        case C::FadingIn:
            t.cloak_timer += dt;
            if (t.cloak_timer >= t.cloak_fade) {
                t.cloak = C::Visible, t.visible_left = t.visible_min;
                scene_.set_object_material(t.object, "", "");
            } else {
                scene_.set_object_param(t.object, "BlendFactor", glm::vec4(t.cloak_timer / t.cloak_fade - 0.5f), 1);
            }
            break;
    }
}

void Combat::paralyze_target(Target& t) {
    if (t.paralyzed <= 0 && !t.paralyze_model.empty()) scene_.set_object_material(t.object, t.paralyze_model, t.paralyze_material);
    t.paralyzed = t.paralyze_time;
}

// CHealthComponent::Update: the paralyze texture runs up the body as the time goes
// (TexFactor (0, 0.5 - left / duration)); its own materials again at the end.
void Combat::update_paralysis(Target& t, float dt) {
    t.paralyzed -= dt;
    if (t.paralyze_model.empty()) return;
    if (t.paralyzed > 0)
        scene_.set_object_param(t.object, "TexFactor", glm::vec4(0, 0.5f - t.paralyzed / std::max(t.paralyze_time, 0.01f), 0, 0), 2);
    else
        scene_.set_object_material(t.object, "", "");
}

int Combat::paralyzed(int id) const {
    const Target* t = find_target(id);
    return t ? int(t->paralyzed > 0) : -1;
}

void Combat::repel(const glm::vec3& eye, const glm::vec3& aim) {
    power_cooldown_ = kPowerCooldown;
    power_used_ = true;
    sound("sfx_plasma_hand_shot", nullptr);
    effect("bullet_repel", eye + aim * 1.5f, aim);
    const glm::vec3 flat = glm::normalize(glm::vec3(aim.x, aim.y, 0) + glm::vec3(0, 1e-4f, 0));
    for (Target& t : targets_) {
        if (!t.actor || !alive(t)) continue;
        const glm::vec3 c = t.pos + glm::vec3(0, 0, t.height * 0.5f);
        const glm::vec3 to = c - eye;
        const float d = glm::length(to);
        const float reach = 12.0f * repel_scale;
        if (d > reach || d < 1e-3f || glm::dot(to / d, aim) < 0.7f || !line_clear(eye, c)) continue;
        killing_repel_ = true, damage_dir_ = to / d;
        apply_damage(t, 6.0f * (1 - d / (reach + 2)), true);
        killing_repel_ = false;
        // Knocked back along the push, kept on the walkable area.
        if (t.mobile && alive(t)) {
            t.pos += flat * (4.0f * (1 - d / reach));
            stay_on_mesh(t);
            if (!t.clip_hurt.empty()) t.state = Target::State::Hurt, t.timer = 0, play(t, t.clip_hurt, false);
            scene_.set_object_pose(t.object, t.pos, t.yaw);
        }
    }
}

void Combat::explode(const Grenade& g) {
    if (logic_.trace) {
        char line[128];
        std::snprintf(line, sizeof line, "grenade explodes at (%.1f %.1f %.1f), %.1f m from the player", g.pos.x, g.pos.y, g.pos.z,
                      glm::distance(g.pos, player_center()));
        log(line);
    }
    effect(g.def->effect, g.pos);
    sound(g.def->explode_sounds, &g.pos);
    // CGrenadeComponent::PlaceDecal: its scorch on the surface under it.
    const glm::vec3 from = g.pos + glm::vec3(0, 0, 0.3f), to = g.pos - glm::vec3(0, 0, 1.0f);
    float t = 1, size = 0;
    uint8_t material = 0;
    glm::vec3 normal(0, 0, 1);
    if (int cell = -1; scene_.collision().raycast(from, to, t, &material, &normal, nullptr, true) && g.def->decals.find(material, cell, size))
        scene_.decals().add(scene_.collision(), from + (to - from) * t, -normal, cell, size);
    splash_damage(g.pos, g.def->damage, g.def->radius, !g.npc);
}

void Combat::splash_damage(const glm::vec3& at, float damage, float r, bool by_player) {
    for (Target& t : targets_) {
        if (!alive(t)) continue;
        const LevelLogic::Object* o = logic_.object(t.id);
        if (!o || !o->enabled) continue;
        const glm::vec3 c = t.pos + glm::vec3(0, 0, t.height * 0.5f);
        const float d = glm::distance(at, c);
        if (d >= r || !line_clear(at + glm::vec3(0, 0, 0.2f), c)) continue;
        damage_cause_ = Cause::Explosion, blast_at_ = at;
        apply_damage(t, damage * (1 - d / r), by_player);
        damage_cause_ = Cause::Shot;
    }
    for (Breakable& b : breakables_) {   // CBreakableComponent: a blast (event 41) on those without capsules
        const LevelLogic::Object* o = logic_.object(b.id);
        if (!b.capsules.empty() || (o && !o->enabled)) continue;
        const float d = glm::distance(at, glm::vec3(scene_.object_matrix(b.object)[3]));
        if (d < r) damage_breakable(b, damage * (1 - d / r));
    }
    const float d = glm::distance(at, player_center());
    if (d < r && line_clear(at + glm::vec3(0, 0, 0.2f), player_.eye())) {
        damage_player(damage * (by_player ? 0.5f : 1.0f) * (1 - d / r), at);   // the player's own: half
    }
    if (d < r * 3) logic_.shake(400, 1.5f * (1 - d / (r * 3)));
}



void Combat::load_breakable(int id, size_t object, const LevelObject& o, const Value& v) {
    if (v.size() < 3) return;
    Breakable b;
    b.id = id, b.type = o.type, b.object = object;
    b.max_health = b.health = num(v, 0, 0);   // CBreakableComponent::CommonInit
    for (const Value& s : v[1].list)
        if (s.size() >= 3) b.stages.push_back({num(s, 0, 0), s[1].i, s[2].i != 0});
    for (const Value& c : v[2].list) {
        if (c.size() < 11) continue;
        if (b.capsules.empty()) b.capsule_health = num(c, 0, 1);   // the first one's health stands for all
        // Init: along its z (a sphere for no length), turned by its rotation (degrees) and
        // placed at its position, in its node's space or the object's.
        glm::mat4 m = glm::translate(scene_.object_matrix(object, str(c, 10)), {num(c, 4, 0), num(c, 5, 0), num(c, 6, 0)});
        m = glm::rotate(m, glm::radians(num(c, 9, 0)), {0, 0, 1});
        m = glm::rotate(m, glm::radians(num(c, 8, 0)), {0, 1, 0});
        m = glm::rotate(m, glm::radians(num(c, 7, 0)), {1, 0, 0});
        const float half = num(c, 2, 0) * 0.5f;
        Breakable::Capsule cap;
        cap.a = glm::vec3(m * glm::vec4(0, 0, -half, 1)), cap.b = glm::vec3(m * glm::vec4(0, 0, half, 1));
        cap.radius = num(c, 3, 0) * glm::length(glm::vec3(m[0]));
        cap.state = c[1].i;
        b.capsules.push_back(cap);
    }
    if (b.stages.empty() && b.capsules.empty()) return;
    breakables_.push_back(std::move(b));
}

void Combat::load_damage_zone(int id, const LevelObject& o, const Value& v) {
    if (v.size() < 17) return;
    DamageZone z;
    z.id = id;
    // CTriggerShapeAABB: the box [-w/2, w/2] x [-d/2, d/2] x [0, h] in the object's frame.
    glm::vec3 box(1);
    if (const Value* b = component(o, kCompBuiltinBox); b && b->size() >= 3) box = {num(*b, 0, 1), num(*b, 1, 1), num(*b, 2, 1)};
    z.lo = {-box.x * 0.5f, -box.y * 0.5f, 0}, z.hi = {box.x * 0.5f, box.y * 0.5f, box.z};
    // CComponentDamage: id, position, rotation (degrees, X then Y then Z), scale, ...
    glm::mat4 m = glm::translate(glm::mat4(1), {num(v, 1, 0), num(v, 2, 0), num(v, 3, 0)});
    m = glm::rotate(m, glm::radians(num(v, 6, 0)), {0, 0, 1});
    m = glm::rotate(m, glm::radians(num(v, 5, 0)), {0, 1, 0});
    m = glm::rotate(m, glm::radians(num(v, 4, 0)), {1, 0, 0});
    z.to_local = glm::inverse(glm::scale(m, {num(v, 7, 1), num(v, 8, 1), num(v, 9, 1)}));
    z.per_second = num(v, 11, 0);
    z.teams = uint32_t(v[15].i);
    z.sound = str(v, 16);
    if (z.per_second > 0) damage_zones_.push_back(std::move(z));
}

void Combat::update_damage_zones(float dt) {
    if (player_dead || dt <= 0) return;
    // The player's capsule, feet to head, in each zone's frame against its box.
    const glm::vec3 feet = player_.center - glm::vec3(0, 0, player_.radius.z), head = player_.center + glm::vec3(0, 0, player_.radius.z);
    for (DamageZone& z : damage_zones_) {
        if (const LevelLogic::Object* o = logic_.object(z.id); !o || !o->enabled) continue;
        if (z.teams && !(z.teams & kPlayerTeamBit)) continue;
        const glm::vec3 a(z.to_local * glm::vec4(feet, 1)), b(z.to_local * glm::vec4(head, 1));
        float t0 = 0, t1 = 1;   // the segment's part within the box's slabs
        for (int k = 0; k < 3 && t0 <= t1; ++k) {
            const float d = b[k] - a[k];
            if (std::abs(d) < 1e-6f) {
                if (a[k] < z.lo[k] || a[k] > z.hi[k]) t0 = 2;
                continue;
            }
            float u0 = (z.lo[k] - a[k]) / d, u1 = (z.hi[k] - a[k]) / d;
            if (u0 > u1) std::swap(u0, u1);
            t0 = std::max(t0, u0), t1 = std::min(t1, u1);
        }
        if (t0 > t1) continue;
        // TestTargetForAction: [11] a second; its sound on the player while not playing.
        damage_player(z.per_second * dt, glm::vec3(glm::inverse(z.to_local) * glm::vec4(0, 0, 0, 1)));
        if (!z.sound.empty() && logic_.audio && !logic_.audio->playing(z.sound_handle)) z.sound_handle = logic_.audio->play(z.sound);
    }
}

namespace {

// The squared distance between segments p1q1 and p2q2, and the fraction along the first
// of its point nearest the second (Ericson, Real-Time Collision Detection 5.1.9).
float segment_distance2(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2, float& s) {
    const glm::vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    float t = 0;
    s = 0;
    if (a <= 1e-8f && e <= 1e-8f) return glm::dot(r, r);
    if (a <= 1e-8f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = glm::dot(d1, r);
        if (e <= 1e-8f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2), denom = a * e - b * b;
            s = denom != 0 ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) t = 0, s = std::clamp(-c / a, 0.0f, 1.0f);
            else if (t > 1) t = 1, s = std::clamp((b - c) / a, 0.0f, 1.0f);
        }
    }
    const glm::vec3 d = p1 + d1 * s - (p2 + d2 * t);
    return glm::dot(d, d);
}

}  // namespace

void Combat::shoot_breakables(const glm::vec3& from, const glm::vec3& to, float damage, bool wall, uint32_t owner) {
    // CBreakableComponent::onEvent: a shot (event 36) through its capsules, nearest first;
    // a hit on its collision (37) for one without.
    const float len = glm::distance(from, to);
    if (len < 1e-4f) return;
    const glm::vec3 past = to + (to - from) / len * 0.3f;   // what the shot stopped on may hold them
    for (Breakable& b : breakables_) {
        if (const LevelLogic::Object* o = logic_.object(b.id); o && !o->enabled) continue;
        if (b.capsules.empty()) {
            if (wall && owner == uint32_t(b.object)) damage_breakable(b, damage);
            continue;
        }
        if (b.capsule_health <= 0) continue;
        int best = -1;
        float best_s = 2;
        for (size_t k = 0; k < b.capsules.size(); ++k) {
            const Breakable::Capsule& c = b.capsules[k];
            float s = 0;
            if (segment_distance2(from, past, c.a, c.b, s) <= c.radius * c.radius && s < best_s) best_s = s, best = int(k);
        }
        if (best < 0) continue;
        b.capsule_health -= damage;
        if (b.capsule_health <= 0) set_breakable_state(b, b.capsules[size_t(best)].state);
    }
}

void Combat::damage_breakable(Breakable& b, float damage) {
    // CBreakableComponent::ReceiveDamage: each stage the share of health left falls to (the
    // lowest wins) sets its state; one may switch the collision off.
    if (b.health <= 0) return;
    b.health -= damage;
    const float left = b.health / std::max(b.max_health, 1e-3f) * 100.0f;
    int state = -1;
    for (const Breakable::Stage& s : b.stages) {
        if (s.below >= b.stage || left > s.below) continue;
        b.stage = s.below, state = s.state;
        if (s.collision_off) scene_.set_object_collision(b.object, false);
    }
    if (state >= 0) set_breakable_state(b, state);
}

void Combat::set_breakable_state(Breakable& b, int state) {
    LevelLogic::Object* o = logic_.object(b.id);
    const std::string name = logic_.anim_sounds().state_name(b.type, state);
    if (!o || name.empty() || o->state == name) return;
    if (logic_.trace) log("breakable " + std::to_string(b.id) + ": " + name);
    logic_.set_state(*o, name);
}

int Combat::find_weapon(const std::string& name) const {
    const std::string want = lower(name);
    for (size_t i = 0; i < weapons_.size(); ++i)
        if (lower(weapons_[i].name) == want || lower(weapon_types_[i]) == want) return int(i);
    return -1;
}

void Combat::use_special_weapon(int index, bool on) {
    if (index < 0) return;
    if (on && current_ != index) {
        if (!special_weapon()) before_special_ = current_;
        current_ = index, reload_left_ = 0, fire_cooldown_ = 0.3f;
        clip_[size_t(index)] = weapons_[size_t(index)].clip, reserve_[size_t(index)] = -1;
    } else if (!on && current_ == index) {
        current_ = before_special_, reload_left_ = 0;
    }
}

int Combat::weapon_count() const {
    int n = 0;
    for (uint8_t o : owned_) n += o;
    return n;
}

int Combat::owned_weapon(int k) const {
    for (size_t i = 0; i < owned_.size(); ++i)
        if (owned_[i] && k-- == 0) return int(i);
    return -1;
}

int Combat::cycle_weapon(int step) const {
    const int n = int(weapons_.size());
    for (int k = 1; k < n; ++k) {
        const int i = ((current_ + step * k) % n + n) % n;
        if (owned_[size_t(i)]) return i;
    }
    return current_;
}

void Combat::take_core(int kind, float percent) {
    core = kind, core_percent = percent;
    player_health = player_max_health * (1 + percent / 100);
    critical_ = false;
}

bool Combat::can_refill() const {
    for (size_t i = 0; i < owned_.size(); ++i) {
        if (!owned_[i] || int(i) == vehicle_weapon_ || int(i) == mounted_gun_ || int(i) == mech_weapon_ || reserve_[i] < 0) continue;
        if (clip_[i] < weapons_[i].clip || reserve_[i] < weapons_[i].reserve) return true;
    }
    return false;
}

void Combat::refill_weapons() {
    for (size_t i = 0; i < owned_.size(); ++i) {
        if (!owned_[i] || int(i) == vehicle_weapon_ || int(i) == mounted_gun_ || int(i) == mech_weapon_ || reserve_[i] < 0) continue;
        clip_[i] = weapons_[i].clip, reserve_[i] = weapons_[i].reserve;
    }
}

bool Combat::unlock_weapon(const std::string& name) {
    if (lower(name) == "grenade") return grenades_enabled = true;
    if (lower(name) == "mines") return mines_enabled = true;
    const int i = find_weapon(name);
    if (i < 0) return false;
    if (!owned_[size_t(i)]) {   // a new weapon comes loaded, with full reserves
        owned_[size_t(i)] = 1;
        clip_[size_t(i)] = weapons_[size_t(i)].clip;
        reserve_[size_t(i)] = weapons_[size_t(i)].reserve;
    }
    return true;
}

bool Combat::lock_weapon(const std::string& name) {
    if (lower(name) == "grenade") {
        grenades_enabled = false;
        return true;
    }
    if (lower(name) == "mines") {
        mines_enabled = false;
        return true;
    }
    const int i = find_weapon(name);
    if (i < 0) return false;
    owned_[size_t(i)] = 0;
    if (i == current_) {
        const int next = owned_weapon(0);
        if (next >= 0) current_ = next, reload_left_ = 0;
    }
    return true;
}

bool Combat::use_weapon(const std::string& name) {
    const int i = find_weapon(name);
    if (i < 0) return false;
    unlock_weapon(name);
    if (i != current_) current_ = i, reload_left_ = 0, fire_cooldown_ = 0.4f;
    return true;
}

bool Combat::has_weapon(const std::string& name) const {
    const int i = find_weapon(name);
    return i >= 0 && owned_[size_t(i)];
}

bool Combat::give_ammo(const std::string& name, int amount) {
    if (lower(name) == "grenade") {
        grenades = std::max(grenades + amount, 0);
        return true;
    }
    const int i = find_weapon(name);
    if (i < 0) return false;
    int& r = reserve_[size_t(i)];
    const int max = weapons_[size_t(i)].reserve;
    if (r >= 0) r = max >= 0 ? std::min(r + amount, max) : r + amount;
    return true;
}

void Combat::respawn() {
    if (!player_dead) return;
    player_dead = false;
    player_health = player_max_health, core = -1;
    protect_ = 3.0f;
    dead_timer_ = 0;
    logic_.respawn_player();
    log("player respawns at " + (logic_.checkpoint() >= 0 ? "checkpoint " + std::to_string(logic_.checkpoint()) : std::string("the start")));
}

Combat::Saved Combat::save() const {
    Saved s;
    s.owned = owned_, s.clip = clip_, s.reserve = reserve_;
    // In a vehicle, at a gun or in the mech: the player's own weapon is the one kept.
    s.current = special_weapon() ? before_special_ : current_;
    s.grenades = grenades, s.mines = mines;
    s.powers = powers_, s.power_index = power_index_;
    for (const Target& t : targets_) {
        if (!t.actor || t.spawned) continue;
        const LevelLogic::Object* o = logic_.object(t.id);
        s.actors.push_back({t.id, o && o->dead, t.pos, t.yaw});
    }
    s.stats = stats_;
    return s;
}

void Combat::restore_arsenal(const Saved& s) {
    if (s.owned.size() == owned_.size() && s.clip.size() == owned_.size() && s.reserve.size() == owned_.size())
        owned_ = s.owned, clip_ = s.clip, reserve_ = s.reserve;
    if (s.current >= 0 && size_t(s.current) < owned_.size() && owned_[size_t(s.current)]) current_ = s.current;
    reload_left_ = 0, fire_cooldown_ = 0, spread_ = weapon().hip.min, cross_alpha_ = 0;
    grenades = s.grenades, mines = s.mines;
    if (!s.powers.empty()) powers_ = s.powers, power_index_ = std::clamp(s.power_index, 0, int(powers_.size()) - 1);
    power_cooldown_ = 0;
}

void Combat::restore(const Saved& s) {
    restore_arsenal(s);
    // CLevelStatistics::Load on a respawn: the counts go back to the checkpoint's, but
    // the retries and the play time run on.
    const Stats now = stats_;
    stats_ = s.stats;
    stats_.targets = now.targets, stats_.actors = now.actors, stats_.hostile = now.hostile;
    stats_.deaths = now.deaths, stats_.play_time = now.play_time;
    critical_ = false;
    for (const Saved::Actor& a : s.actors) {
        Target* t = find_target(a.id);
        if (!t || !t->actor) continue;
        if (a.dead) {   // CNPCComponent::SaveLoad: gone, not a corpse
            t->state = Target::State::Dead;
            if (LevelLogic::Object* o = logic_.object(a.id)) o->dead = true, o->health = 0, logic_.show(*o, false);
            continue;
        }
        t->yaw = a.yaw;
        place_actor(a.id, a.pos);
    }
    // Breakables come back in the states they were in (the level logic's); their damage
    // follows from those.
    AnimSounds& states = logic_.anim_sounds();
    for (Breakable& b : breakables_) {
        const LevelLogic::Object* o = logic_.object(b.id);
        if (!o || o->state.empty()) continue;
        auto reached = [&](int index) {   // the state, or the one it leads to
            const std::string name = states.state_name(b.type, index);
            const AnimSounds::State* st = name.empty() ? nullptr : states.state(b.type, name);
            return !name.empty() && (name == o->state || (st && st->next == o->state));
        };
        for (const Breakable::Stage& st : b.stages) {
            if (st.below >= b.stage || !reached(st.state)) continue;
            b.stage = st.below, b.health = b.max_health * st.below / 100.0f;
            if (st.collision_off) scene_.set_object_collision(b.object, false);
        }
        for (const Breakable::Capsule& c : b.capsules)
            if (reached(c.state)) b.capsule_health = 0;
    }
}

bool Combat::is_actor(int id) const {
    const Target* t = find_target(id);
    return t && t->actor;
}

void Combat::campaign_loadout(const std::string& level) {
    // Campaign order: San Francisco 1 and 2, then the rest. Later levels start with what
    // the San Francisco levels unlock.
    const std::string l = lower(level);
    if (l.find("san_francisco_1") != std::string::npos) return;
    for (const char* w : {"AutomaticShotGun", "HandGun"}) unlock_weapon(w);
    if (l.find("san_francisco_2") == std::string::npos) unlock_weapon("SniperRifle");
    grenades = std::max(grenades, 3);
}

const Value* Combat::component(const LevelObject& o, int comp) const {
    if (auto it = o.components.find(comp); it != o.components.end()) return &it->second;
    auto g = gol_.find(o.type);
    return g == gol_.end() ? nullptr : resolved_template(g->second, gol_, comp);
}

WeaponDef Combat::load_weapon(const std::string& type_name) const {
    WeaponDef w;
    w.name = type_name;
    for (const auto& [id, type] : gol_) {
        if (type.name != type_name) continue;
        const Value* v = resolved_template(type, gol_, kCompWeapon);
        if (!v || v->size() < 28) break;
        const Value& f = *v;
        w.type = id;
        w.name = f[0].s;
        // CComponentWeapon::Load: ..., max ammo (+0x4c), clip size (+0x50), ...
        w.reserve = f[16].i;
        w.clip = std::max(f[17].i, 1);
        w.hip = {num(f, 23, 1), num(f, 24, 2), num(f, 25, 10), num(f, 26, 20), num(f, 27, 1.5f)};
        w.aim = {num(f, 28, 0.2f), num(f, 29, 1), num(f, 30, 1.5f), num(f, 31, 1), num(f, 32, 0.7f)};
        if (f.size() > 46) {
            w.cross_aiming = f[33].i != 0, w.cross_px = f[34].i, w.cross_plus = f[36].i != 0;
            w.scope = f[46].i != 0;
        }
        if (f.size() > 52) w.aim_move = num(f, 52, 1);
        if (f.size() > 35 && f[35].type == Value::String) w.sprite = f[35].s;
        // Fire modes {?, projectile type, continuous, continuous delay (ms), ...}.
        if (f[4].size() > 0 && f[4][0].size() > 3) {
            const Value& mode = f[4][0];
            w.automatic = mode[2].i != 0;
            const int delay = mode[3].i;
            w.interval = delay > 0 ? delay / 1000.0f : 0.1f;
            if (mode.size() > 18) {
                w.shot_sounds = sound_list(mode[15]);
                w.reload_sound = mode[16].s, w.loop_sound = mode[17].s, w.end_sound = mode[18].s;
            }
            if (auto p = gol_.find(mode[1].i); p != gol_.end() && resolved_template(p->second, gol_, 0x26)) {
                w.grenade = mode[1].i;   // fired like a throw (CWeaponComponent::Shoot: StartRealThrow)
            } else if (p != gol_.end()) {
                if (const Value* pr = resolved_template(p->second, gol_, kCompProjectile); pr && pr->size() > 4) {
                    w.damage_far = num(*pr, 1, 1), w.damage_near = num(*pr, 2, 2);
                    w.far_dist = num(*pr, 3, 30), w.near_dist = num(*pr, 4, 30);
                    w.impacts = read_impacts(*pr);
                    // [8] speed (per ms times 1000: CProjectileComponent::LaunchBullet).
                    w.bullet_speed = num(*pr, 8, 0);
                    w.splash_damage = num(*pr, 6, 0), w.splash_radius = num(*pr, 7, 0);
                    if (pr->size() > 39) w.bullet_life = std::max((*pr)[39].as_float() / 1000.0f, 0.1f);
                    w.looks = read_looks(mode[1].i);
                    if (pr->size() > 32 && (*pr)[32].i != 0 && (*pr)[30].type == Value::String)
                        w.beam_texture = (*pr)[30].s, w.beam_width = num(*pr, 31, 0.2f);
                    w.flies = (w.splash_damage > 0 && w.splash_radius > 0) ||
                              (w.bullet_speed >= 1 && w.bullet_speed < 150 && (!w.looks.fx.empty() || w.looks.body >= 0));
                }
            }
        }
        // The fire rate is in the weapon's animation states (CStatesSetComponent): the
        // looping attack clip carries a GameEvent per shot. Use the typical gap between them.
        if (const Value* states = resolved_template(type, gol_, 0x0e); states && states->size() > 1) {
            for (const char* want : {"Attack_Loop", "AimAttack"}) {
                std::vector<float> times;
                for (const Value& st : (*states)[1].list) {
                    if (st.size() < 12 || st[0].s != want || st[11].size() == 0) continue;
                    const Value& clip = st[11][0];
                    if (clip.size() < 3) continue;
                    for (const Value& group : clip[2].list)
                        if (group.size() >= 3 && group[0].s == "GameEvent")
                            for (const Value& ev : group[2].list)
                                if (ev.size() >= 2 && ev[1].i >= 0) times.push_back(float(ev[1].i));
                }
                if (times.size() < 3) continue;
                std::sort(times.begin(), times.end());
                std::vector<float> gaps;
                for (size_t i = 1; i < times.size(); ++i) gaps.push_back(times[i] - times[i - 1]);
                std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
                w.interval = gaps[gaps.size() / 2] / 1000.0f;
                break;
            }
        }
        w.reload_time = w.clip > 15 ? 1.8f : 1.3f;
        break;
    }
    return w;
}

void Combat::start(bool keep_stats) {
    nav_.build(scene_.level());
    live_grenades_.clear();   // their objects went with the old level
    tracers_.clear(), trail_pool_.clear(), beams_.clear();
    throw_delay_ = -1;
    min_enemies_on_player = 1, max_shooters_ = 2, grenade_ticket_ = kGrenadeTicketWait;
    targets_.clear();
    spawners_.clear();
    bolts_.clear();
    pickups_.clear(), pickup_info_ = {};
    breakables_.clear(), damage_zones_.clear();
    if (keep_stats) {   // counted again below
        stats_.targets = stats_.actors = stats_.hostile = 0;
    } else {
        stats_ = {};
    }
    critical_ = false;
    const Level& level = scene_.level();
    for (auto& [id, obj] : logic_.objects_) {
        if (obj.index >= level.objects.size()) continue;
        const LevelObject& o = level.objects[obj.index];
        if (o.kind != "object") continue;
        if (const Value* sm = o.components.count(kCompSpawnManager) ? &o.components.at(kCompSpawnManager) : nullptr;
            sm && sm->size() >= 4) {
            // CComponentSpawnManager::Load: spawn points, waves {entries {type, delay, waypoint},
            // group, threshold}, loop, team mask, ?
            SpawnManager m;
            m.id = id;
            for (const Value& p : (*sm)[0].list) m.points.push_back(p.list.empty() ? p.i : p[0].i);
            for (const Value& w : (*sm)[1].list) {
                if (w.size() < 3) continue;
                SpawnManager::Wave wave;
                for (const Value& e : w[0].list)
                    if (e.size() >= 3) wave.entries.push_back({e[0].i, e[1].i, e[2].i});
                wave.group = w[1].i;
                wave.threshold = w[2].i;
                m.waves.push_back(std::move(wave));
            }
            m.loop = (*sm)[2].i != 0;
            m.teams = uint32_t((*sm)[3].i);
            spawners_.push_back(std::move(m));
            continue;
        }
        if (const Value* pu = component(o, kCompPowerUp)) {
            load_pickup(id, o, *pu);
            continue;
        }
        if (const Value* br = component(o, kCompBreakable)) load_breakable(id, obj.index, o, *br);
        if (const Value* dz = component(o, kCompDamage)) {
            load_damage_zone(id, o, *dz);
            continue;
        }
        const Value* health = component(o, kCompHealth);
        if (!health || !component(o, kCompTargetable)) continue;
        Target t;
        t.id = id;
        t.object = obj.index;
        t.type = o.name;
        t.pos = obj.position;
        t.yaw = obj.yaw;
        if (!obj.dead) obj.health = num(*health, 0, 10);   // CHealthComponent: max health
        t.max_health = std::max(num(*health, 0, 10), 1.0f);
        if (const float ms = num(*health, 3, 0); ms > 0) t.paralyze_time = ms / 1000.0f;
        if (health->size() > 9) t.hurt_sounds = sound_list((*health)[6]), t.death_sounds = sound_list((*health)[9]);
        if (const Value* tg = component(o, kCompTargetable)) {
            t.hit_effect = effect_name(str(*tg, 12).empty() ? str(*tg, 8) : str(*tg, 12));
            t.hit_node = str(*tg, 13);
            t.death_effect = effect_name(str(*tg, 14));
            t.death_node = str(*tg, 15);
        }
        t.explosive = lower(o.name).find("explosive") != std::string::npos;
        if (const Value* phys = component(o, 0x8f); phys && phys->size() > 6 && (*phys)[1].i != 0)   // CPhysicsObjectComponent
            t.throwable = true, t.throw_speed = std::max(num(*phys, 6, 22), 1.0f);
        if (const Value* motion = component(o, 0x3b); motion && motion->size() > 6) t.throw_lift = num(*motion, 6, 1);   // CThrowMotionComponent
        if (t.explosive) t.radius = 0.5f, t.height = 1.0f;
        const bool ai = component(o, kCompAi) || component(o, 0x55) || component(o, 0x21) || component(o, 0x57) ||
                        component(o, 0x59) || component(o, 0x23);
        if (ai && (component(o, kCompGroundNavigation) || component(o, kCompAirNavigation))) setup_actor(t, o);
        if (const Value* ds = component(o, 0x9f); ds && ds->size() >= 10 && !t.actor) {   // CDropShipAIComponent::Load
            t.dropship = true;
            set_projectile(t, (*ds)[0].i);
            t.ds_muzzle = str(*ds, 1), t.ds_sound = str(*ds, 10);
            t.ds_turn_max = num(*ds, 2, 20), t.ds_turn_accel = std::max(num(*ds, 3, 10), 1.0f);
            t.ds_pause = num(*ds, 7, 1), t.ds_miss_min = num(*ds, 8, 15), t.ds_miss_max = num(*ds, 9, 30);
            t.ds_yaw = obj.yaw;
            if (const Value* aw = component(o, kCompAwareness); aw && aw->size() > 2)
                t.team = (*aw)[0].i, t.attacks = uint32_t((*aw)[1].i), t.sight = num(*aw, 2, 700);
            t.hostile = (t.attacks & kPlayerTeamBit) != 0;
            t.radius = 4, t.height = 3, t.pos.z -= 1.5f;   // a hull to hit
        }
        if (const Value* car = component(o, 0x96); car && car->size() > 0 && component(o, 0x97) && !t.actor) {
            t.car = true;
            if (const Value* aw = component(o, kCompAwareness); aw && aw->size() > 5)
                t.team = (*aw)[0].i, t.attacks = uint32_t((*aw)[1].i), t.sight = num(*aw, 2, 40), t.fov = num(*aw, 3, 180), t.close = num(*aw, 5, 10);
            t.hostile = (t.attacks & kPlayerTeamBit) != 0;
            t.radius = 1.6f, t.height = 2.2f, t.pos.z -= 0.9f;   // the body, from the ground
            t.car_gun_type = (*car)[0].i;
            auto g = gol_.find(t.car_gun_type);
            const Value* w = g == gol_.end() ? nullptr : resolved_template(g->second, gol_, kCompWeapon);
            if (w && w->size() > 27 && (*w)[4].size() > 0 && (*w)[4][0].size() > 18) {
                const Value& mode = (*w)[4][0];
                set_projectile(t, mode[1].i);
                // Damage: near [2] within [4] m, else far [1].
                if (auto b = gol_.find(mode[1].i); b != gol_.end())
                    if (const Value* pr = resolved_template(b->second, gol_, kCompProjectile); pr && pr->size() > 4 && num(*pr, 4, 0) <= 0)
                        t.bolt_damage = num(*pr, 1, t.bolt_damage);
                t.muzzle_type = mode[9].i, t.muzzle_node = str(mode, 11), t.muzzle_clips = first_state_clips(t.muzzle_type);
                t.car_loop_sound = str(mode, 17), t.car_stop_sound = str(mode, 18);
                t.car_spread_min = num(*w, 23, 0.2f), t.car_spread_max = num(*w, 24, 1);
                t.car_spread_recover = num(*w, 26, 0.25f), t.car_spread_fire = num(*w, 27, 0.1f);
                t.car_spread = t.car_spread_min;
            } else {
                t.ranged = false;
            }
        }
        targets_.push_back(std::move(t));
        ++stats_.targets;
        if (targets_.back().actor) ++stats_.actors;
        if (targets_.back().hostile) ++stats_.hostile;
    }
    load_covers();
    load_jump_points();
    log("combat: " + std::to_string(stats_.targets) + " targets, " + std::to_string(stats_.actors) + " actors (" +
        std::to_string(stats_.hostile) + " hostile), " + std::to_string(spawners_.size()) + " spawn managers, " + std::to_string(pickups_.size()) + " pick-ups, " +
        std::to_string(breakables_.size()) + " breakables, nav mesh " +
        std::to_string(nav_.triangle_count()) + " triangles");
    for (const WeaponDef& w : weapons_) {
        char line[160];
        std::snprintf(line, sizeof line, "weapon %s: %.0f ms between shots, clip %d, reserve %d, damage %.1f-%.1f", w.name.c_str(),
                      w.interval * 1000, w.clip, w.reserve, w.damage_near, w.damage_far);
        log(line);
    }
}

bool Combat::set_projectile(Target& t, int type) {
    auto p = gol_.find(type);
    if (p == gol_.end()) return false;
    const Value* pr = resolved_template(p->second, gol_, kCompProjectile);
    if (!pr || pr->size() <= 8) return false;
    t.bolt_impacts = read_impacts(*pr);
    t.ranged = true;
    t.bolt_damage = num(*pr, 2, 3);
    t.bolt_speed = std::max(num(*pr, 8, 40), 10.0f);
    t.bolt_looks = read_looks(type);
    t.bolt_splash = num(*pr, 6, 0), t.bolt_radius = num(*pr, 7, 0);
    if (lower(p->second.name).find("throw") != std::string::npos || lower(p->second.name).find("plasma") != std::string::npos)
        t.bolt_color = {0.35f, 0.6f, 1.0f, 1}, t.burst = 2, t.burst_gap = 0.35f;
    return true;
}

bool Combat::set_bullet_type(int npc, int type) {
    Target* t = find_target(npc);
    return t && t->actor && set_projectile(*t, type);
}

void Combat::setup_actor(Target& t, const LevelObject& o) {
    t.actor = true;
    t.mobile = component(o, kCompGroundNavigation) != nullptr;
    t.headshots = true;
    if (const Value* aw = component(o, kCompAwareness); aw && aw->size() > 5) {
        t.team = (*aw)[0].i;
        t.attacks = uint32_t((*aw)[1].i);
        t.sight = num(*aw, 2, 40);
        t.fov = num(*aw, 3, 180);
        t.close = num(*aw, 5, 10);
    }
    t.hostile = (t.attacks & kPlayerTeamBit) != 0;
    if (const Value* c = component(o, kCompActorsCollision); c && c->size() > 1 && num(*c, 0, 0) > 0) {
        t.radius = num(*c, 0, 0.6f);
        t.height = num(*c, 1, 1.9f);
    } else {
        t.radius = 0.9f, t.height = 1.1f, t.headshots = false;   // spiders and other low creatures
    }
    // Ranged weapon: CNPCComponent weapon list {type, right hand node, left} ->
    // CNPCWeaponComponent -> its projectile. The weapon is held at the hand node.
    auto attach = [&](int type, const std::string& node) {
        const size_t object = scene_.spawn_object(type, -5000 - attachment_id_++, t.pos, t.yaw);
        if (object == size_t(-1)) return false;
        scene_.set_object_visible(object, false);
        t.attached.push_back({object, node});
        return true;
    };
    t.attached.clear(), t.armed = false, t.muzzle_object = size_t(-1), t.muzzle_type = -1;
    if (const Value* npc = component(o, kCompNpc); npc && npc->size() > 1 && (*npc)[1].size() > 0) {
        const Value& slot = (*npc)[1][0];
        const int weapon = slot[0].i;
        if (auto w = gol_.find(weapon); w != gol_.end()) {
            if (slot.size() > 1 && slot[1].type == Value::String) t.armed = attach(weapon, slot[1].s);
            if (const Value* nw = resolved_template(w->second, gol_, kCompNpcWeapon); nw && nw->size() > 0) {
                if (nw->size() > 22) t.fire_sounds = sound_list((*nw)[22]);
                if (nw->size() > 23) t.reload_sounds = sound_list((*nw)[23]);
                t.mag_size = (*nw)[1].i > 0 ? (*nw)[1].i : -1, t.per_shot = std::max((*nw)[2].i, 1), t.mag = t.mag_size;
                set_projectile(t, (*nw)[0].i);
                if (nw->size() > 10 && (*nw)[10].type == Value::String)
                    t.muzzle_type = (*nw)[9].i, t.muzzle_node = (*nw)[10].s, t.muzzle_clips = first_state_clips(t.muzzle_type);
            }
        }
    }
    if (const Value* h = component(o, kCompHealth); h && h->size() > 20 && (*h)[13].type == Value::String) {
        t.death_model = (*h)[13].s, t.death_material = (*h)[14].s;
        if ((*h)[15].type == Value::String) t.paralyze_model = (*h)[15].s, t.paralyze_material = (*h)[16].s;
        if ((*h)[20].type == Value::String) t.vanish_sound = (*h)[20].s;
    }
    t.dissolve = -1;
    t.ragdoll.reset();
    t.has_ragdoll = false;
    if (const Value* sp = component(o, 0xa3); sp && sp->size() > 9 && (*sp)[0].type == Value::String && t.shadow == size_t(-1)) {
        t.shadow = scene_.spawn_model((*sp)[0].s, false);
        t.shadow_offset = glm::vec3(num(*sp, 1, 0), num(*sp, 2, 0), num(*sp, 3, 0));
        t.shadow_seen = (*sp)[4].i != 0;
        t.shadow_nodes.clear();
        for (const Value& n : (*sp)[9].list)
            if (n.size() > 0 && n[0].type == Value::String) t.shadow_nodes.push_back(n[0].s);
        if (t.shadow != size_t(-1)) {
            scene_.set_object_visible(t.shadow, false);
            scene_.set_object_ground_shadow(t.shadow);
            // NV_CharacterShadow's technique by how many feet it follows (ONEPED ... FIVEPED).
            static const char* const kPeds[] = {"ONEPED", "BIPED", "TRIPED", "QUADRUPED", "FIVEPED"};
            if (!t.shadow_nodes.empty()) scene_.set_object_technique_all(t.shadow, kPeds[std::min<size_t>(t.shadow_nodes.size(), 5) - 1]);
            // OnObjActivate: the material's own SPosition heights are the least each may sink to.
            t.shadow_low.assign(t.shadow_nodes.size(), 0.0f), t.shadow_radius.assign(t.shadow_nodes.size(), 0.3f);
            for (size_t i = 0; i < t.shadow_nodes.size(); ++i) {
                if (glm::vec4 v; scene_.object_param_default(t.shadow, "SPosition" + std::to_string(i), v)) t.shadow_low[i] = v.z;
                if (glm::vec4 v; scene_.object_param_default(t.shadow, "SRadius" + std::to_string(i), v)) t.shadow_radius[i] = v.x;
                scene_.set_object_param(t.shadow, "SRadius" + std::to_string(i), glm::vec4(t.shadow_radius[i]), 1);
            }
        }
    }
    if (const Value* r = component(o, 0x8e); r && r->size() > 19 && (*r)[0].i != 0) {   // CPhysicsRagdollComponent
        t.has_ragdoll = ragdolls_.shapes.count((*r)[1].i) > 0, t.ragdoll_kind = (*r)[1].i;
        t.ragdoll_hit = num(*r, 17, 90) * num(*r, 18, 1), t.ragdoll_hit_max = num(*r, 19, 100), t.ragdoll_blast = num(*r, 11, 32);
    }
    t.attack_choices.clear(), t.attack_state.clear(), t.attack_ms = -1;
    if (const Value* npc = component(o, kCompNpc); npc && npc->size() > 54)
        for (const Value& a : (*npc)[54].list)
            if (a.size() > 1 && a[0].type == Value::String && a[1].as_float() > 0) t.attack_choices.emplace_back(a[0].s, a[1].as_float());
    // Only when its states fire by events (the marines fire by their behaviour scripts).
    {
        bool fires = false;
        const auto& st = states_of(o.type);
        for (const auto& [name, w] : t.attack_choices)
            for (std::string n = name; !n.empty() && !fires;) {
                auto it = st.find(n);
                if (it == st.end()) break;
                for (const auto& c : it->second.clips)
                    for (const auto& e : c.second) fires |= e.second == 1 || e.second == 2 || e.second == 3 || e.second == 18 || e.second == 23;
                if (it->second.loop || it->second.next == n) break;
                n = it->second.next;
            }
        if (!fires) t.attack_choices.clear();
    }
    t.cloak = Target::Cloak::Visible, t.script_cloak = false, t.recent_damage.clear();
    if (const Value* npc = component(o, kCompNpc); npc && npc->size() > 53) {   // super speed, jumping
        t.dash_side = num(*npc, 30, 7), t.dash_min = num(*npc, 31, 5), t.dash_speed = std::max(num(*npc, 32, 12), 1.0f);
        t.jump_reach = num(*npc, 52, 8), t.jump_walk = num(*npc, 53, 2);
        if (npc->size() > 59) t.telekinesis_sound = str(*npc, 56), t.telekinesis_end_sound = str(*npc, 57), t.grab_node = str(*npc, 59);
    }
    if (const Value* npc = component(o, kCompNpc); npc && npc->size() > 69 && (*npc)[62].type == Value::String && !(*npc)[62].s.empty()) {
        t.cloak_model = (*npc)[62].s, t.cloak_material = (*npc)[63].s;
        t.cloak_fade = std::max(num(*npc, 64, 800) / 1000.0f, 0.05f);
        t.auto_cloak = (*npc)[65].i != 0;
        t.cloak_sound = (*npc)[68].s, t.uncloak_sound = (*npc)[69].s;
    }
    if (const Value* ai = component(o, 78); ai && ai->size() > 45) {
        t.invisible_min = num(*ai, 41, 3000) / 1000.0f, t.invisible_max = std::max(num(*ai, 42, 3000) / 1000.0f, t.invisible_min);
        t.visible_min = num(*ai, 43, 1000) / 1000.0f, t.cloak_damage = num(*ai, 45, 1);
    }
    // CComponentEffectsLocal: [1] technique, [3] time parameter, [4] start, [5] end, [6]
    // restart from, [7] the impact point wanted, [8] its parameter.
    if (const Value* hl = component(o, 0x93); hl && hl->size() > 8 && (*hl)[1].type == Value::String && !(*hl)[1].s.empty()) {
        t.hurt_technique = (*hl)[1].s, t.hurt_param = (*hl)[3].s;
        t.hurt_start = num(*hl, 4, 0.1f), t.hurt_end = num(*hl, 5, 1), t.hurt_again = num(*hl, 6, 0.6f);
        t.hurt_impact_param = (*hl)[7].i != 0 ? (*hl)[8].s : std::string();
    }
    if (const Value* att = component(o, 0x91); att && att->size() > 0)
        for (const Value& a : (*att)[0].list)
            if (a.size() > 1 && a[1].type == Value::String) attach(a[0].i, a[1].s);
    // CComponentNPCWeapon: [6] range, [7] spread max (deg), [8] spread step, [18] sweep
    // radius max, [19] min, [20] shrink rate, [21] turn speed. CComponentAI (78): [14]/[15]
    // accuracy distances, [16] accuracy time (ms), [18] attack behaviours, [19] their choice.
    if (const Value* npc = component(o, kCompNpc); npc && npc->size() > 1 && (*npc)[1].size() > 0)
        if (auto w = gol_.find((*npc)[1][0][0].i); w != gol_.end())
            if (const Value* nw = resolved_template(w->second, gol_, kCompNpcWeapon); nw && nw->size() > 21) {
                t.spread_max = num(*nw, 7, 1), t.spread_step = num(*nw, 8, 0.1f);
                t.sweep_max = num(*nw, 18, 2), t.sweep_min = num(*nw, 19, 0.05f);
                t.sweep_rate = num(*nw, 20, 1), t.sweep_speed = num(*nw, 21, 180);
            }
    if (const Value* ai = component(o, 78); ai && ai->size() > 19) {
        t.acc_near = num(*ai, 14, 10), t.acc_far = num(*ai, 15, 50), t.acc_time = num(*ai, 16, 25000) / 1000.0f;
        for (const Value& b : (*ai)[18].list) {
            if (b.size() < 2 || b[1].type != Value::String) continue;
            // Its script: NpcAction_Attack(obj, "Fire") per shot, a Wait(ms) after each.
            Target::Behaviour beh;
            beh.name = b[0].s;
            std::vector<float> waits;
            size_t line_start = 0;
            const std::string& code = b[1].s;
            while (line_start < code.size()) {
                size_t end = code.find('\n', line_start);
                if (end == std::string::npos) end = code.size();
                std::string line = code.substr(line_start, end - line_start);
                line_start = end + 1;
                const size_t first = line.find_first_not_of(" \t");
                if (first == std::string::npos || line.compare(first, 2, "--") == 0) continue;
                if (line.find("NpcAction_Attack") != std::string::npos && line.find("\"Fire\"") != std::string::npos) ++beh.shots;
                if (const size_t w = line.find("Wait("); w != std::string::npos) waits.push_back(std::strtof(line.c_str() + w + 5, nullptr) / 1000.0f);
            }
            if (!waits.empty()) beh.gap = waits.front(), beh.pause = waits.back() > waits.front() ? waits.back() : 0;
            if (beh.shots > 0) t.behaviours.push_back(beh);
        }
        read_ai(t, *ai);
        if ((*ai)[19].type == Value::String) {
            const std::string& code = (*ai)[19].s;
            if (const size_t f = code.find("function "); f != std::string::npos)
                if (const size_t e = code.find('(', f); e != std::string::npos) t.choose_fn = code.substr(f + 9, e - f - 9);
        }
    }
    t.sweep_r = t.sweep_max;
    t.stand_height = t.height;
    if (const Value* npc = component(o, kCompNpc); npc && npc->size() > 7 && npc_grenade_def_.type >= 0 && (*npc)[4].i == npc_grenade_def_.type)
        t.grenades = (*npc)[5].i, t.grenade_min = num(*npc, 6, 20), t.grenade_max = num(*npc, 7, 50);
    t.fire_range = std::min(t.sight, 35.0f);
    t.melee_damage = t.ranged ? 15.0f : 8.0f;

    const auto clips = scene_.clip_names(t.object);
    t.clip_idle = pick_clip(clips, {"001_idle", "idle1", "_idle"});
    t.clip_alert = pick_clip(clips, {"idle_attack_assaultrifle", "002_idle_attack", "008_idle_attack", "idle_attack"});
    t.clip_run = pick_clip(clips, {"068_fast_run", "024_walk_very_fast", "009_run", "fast_run", "walk_fast", "_run", "walk"});
    t.clip_walk = pick_clip(clips, {"_walk_forward", "007_walk", "_walk", "walk"});
    t.clip_fire = pick_clip(clips, {"106_upper_fire_suppresive", "030_plasma_fire_multi", "_fire"});
    t.clip_melee = pick_clip(clips, {"025_atack_both_claws", "004_atack_right_claw", "066_melee_attack_right_hand_onplace",
                                     "melee", "atack", "attack"});
    t.clip_hurt = pick_clip(clips, {"108_demage_from_front", "018_hurt_front_chest", "036_hurt_torso", "hurt", "demage"});
    t.clip_jump = pick_clip(clips, {"009_jump_cycle", "125_jump_loop", "jump_cycle", "jump_loop", "jump"});
    for (const std::string& c : clips) {
        const std::string l = lower(c);
        const bool death = l.find("death") != std::string::npos || l.find("_die") != std::string::npos;
        const bool special = l.find("cover") != std::string::npos || l.find("by_") != std::string::npos ||
                             l.find("expl") != std::string::npos || l.find("fire") != std::string::npos;
        if (death && !special && scene_.clip_length(t.object, c) >= 0.5f) t.clip_deaths.push_back(c);
    }
    if (t.clip_deaths.empty()) {
        const std::string any = pick_clip(clips, {"die", "death"});
        if (!any.empty()) t.clip_deaths.push_back(any);
    }
    t.clip_idle_default = t.clip_idle;
    if (t.clip_alert.empty()) t.clip_alert = t.clip_idle;
    if (t.clip_fire.empty()) t.clip_fire = t.clip_alert;
    if (t.mobile) {
        scene_.set_root_lock(t.object, "Bip01");
        if (!t.clip_run.empty()) t.run_speed = std::clamp(scene_.clip_root_speed(t.object, t.clip_run), 1.5f, 9.0f);
        glm::vec3 on;
        t.tri = nav_.nearest(t.pos + glm::vec3(0, 0, 0.5f), 3.0f, on);
        if (t.tri >= 0) t.pos = on;
    }
    scene_.set_object_pose(t.object, t.pos, t.yaw);
    play(t, t.clip_idle, true);
    t.fire_timer = std::uniform_real_distribution<float>(0.3f, 1.5f)(rng_);
}

void Combat::play(Target& t, const std::string& clip, bool loop) {
    if (clip.empty() || (t.playing == clip && loop)) return;
    if (!scene_.play_clip(t.object, clip, loop)) return;
    t.playing = clip;
    if (t.clip_speed_set) scene_.set_clip_speed(t.object, 1), t.clip_speed_set = false;
}

void Combat::play_state(Target& t, const std::string& state, bool loop) {
    const int type = scene_.level().objects[t.object].type;
    const std::string clip = logic_.state_clip(type, state);
    const float speed = logic_.state_speed(type, state);
    if (clip.empty() || (t.playing == clip && loop)) return;
    if (!scene_.play_clip(t.object, clip, loop, speed < 0)) return;
    t.playing = clip;
    scene_.set_clip_speed(t.object, std::fabs(speed));
    t.clip_speed_set = true;
}

bool Combat::alive(const Target& t) const {
    const LevelLogic::Object* o = logic_.object(t.id);
    return o && o->enabled && !o->dead && t.state != Target::State::Dying && t.state != Target::State::Dead;
}

glm::vec3 Combat::player_center() const { return player_.center + glm::vec3(0, 0, 0.3f); }

bool Combat::line_clear(const glm::vec3& a, const glm::vec3& b) const {
    float t = 1;
    return !scene_.collision().raycast(a, b, t);
}

Combat::Target* Combat::find_target(int id) {
    for (Target& t : targets_)
        if (t.id == id) return &t;
    return nullptr;
}

bool Combat::npc_order(int npc, const std::string& order, int arg, bool flag) {
    Target* t = find_target(npc);
    if (t && t->dropship) {   // CDropShipAIComponent::OnEvent (18): a fire now, or no more turning
        if (order == "fire") t->ds_fire_at = 0;
        else if (order == "disable_rotation") t->ds_no_turn = true;   // (whatever the argument)
        else return false;
        return true;
    }
    if (!t || !t->actor) return false;
    if (order == "follow_mode") t->follow = flag;
    else if (order == "lock_in_cover" || order == "locked_in_cover" || order == "imobile") t->hold = flag;
    else if (order == "lock_ai") t->ai_locked = flag;
    else if (order == "link_region") {
        t->has_region = false;
        const LevelLogic::Object* r = arg > 0 ? logic_.object(arg) : nullptr;
        if (r && r->index < scene_.level().objects.size() && scene_.level().objects[r->index].base) {
            // The region's box: its scale about its position (taken as half sizes).
            const glm::vec3 half = glm::abs(scene_.level().objects[r->index].base->scale);
            t->has_region = true, t->region_min = r->position - half, t->region_max = r->position + half;
        }
    } else if (order == "run" || order == "fly") {
        const LevelLogic::Object* wp = logic_.object(arg);
        if (!wp) return false;
        NpcGoal g;
        g.kind = NpcGoal::Kind::Move, g.pos = wp->position, g.run = true, g.id = -1;
        if (t->goals.size() < 16) t->goals.push_back(g);
    } else if (order == "jump" || order == "jump_at" || order == "AI_JumpSpecial") {   // a leap to the waypoint
        const LevelLogic::Object* wp = logic_.object(arg);
        if (!wp) return false;
        NpcGoal g;
        g.kind = NpcGoal::Kind::Jump, g.pos = wp->position, g.id = -1;
        if (t->goals.size() < 16) t->goals.push_back(g);
    } else if (order == "walk" || order == "patrol") {
        // Walking to the waypoint; patrols carry on along its path's following waypoints.
        int wp_id = arg;
        for (int k = 0; k < 16 && wp_id > 0 && t->goals.size() < 16; ++k) {
            const LevelLogic::Object* wp = logic_.object(wp_id);
            if (!wp) break;
            NpcGoal g;
            g.kind = NpcGoal::Kind::Move, g.pos = wp->position, g.run = false, g.id = -1;
            t->goals.push_back(g);
            if (order == "walk") break;
            wp_id = logic_.next_waypoint(wp_id);
            if (wp_id == arg) break;   // a loop: once round
        }
    } else if (order == "teleport") {   // NPCSuperSpeedToWaypoint
        const LevelLogic::Object* wp = logic_.object(arg);
        if (!wp) return false;
        NpcGoal g;
        g.kind = NpcGoal::Kind::Teleport, g.pos = wp->position, g.id = -1, g.dash = true;
        t->goals.clear();
        t->goals.push_back(g);
    } else if (order == "set_target") {   // 0: the player
        return lock_target(npc, arg, true);
    } else if (order == "take_cover") {
        t->hold = false, t->choose_timer = 0;
    } else if (order == "cloack" || order == "decloack" || order == "uncloack") {
        // Cloaked: unseen and not a target (the original's cloak shader is not ported).
        const bool cloak = order == "cloack";
        scene_.set_object_visible(t->object, !cloak);
        t->invulnerable = cloak;
    } else if (order == "use_machinegun") {
        // CMachineGunAIComponent::ActivateBy: behind the gun, which turns and fires for it.
        const LevelLogic::Object* gun = logic_.object(arg);
        if (!gun || gun->index >= scene_.level().objects.size() || !gun_def(scene_.level().objects[gun->index].type)) return false;
        t->gun = arg, t->gun_state = 0, t->gun_timer = t->gun_shot = 0, t->gun_yaw = gun->yaw;
        t->hold = true, t->leaping = false;
        t->goals.clear();
    } else {
        return false;
    }
    return true;
}

bool Combat::lock_target(int npc, int target, bool on) {
    Target* t = find_target(npc);
    if (!t || !t->actor) return false;
    t->locked = on ? target : -1;
    t->choose_timer = 0;
    if (on) t->aware = true;
    return true;
}

bool Combat::set_idle_state(int npc, const std::string& state) {
    Target* t = find_target(npc);
    if (!t || !t->actor) return false;
    std::string clip = state.empty() ? "" : logic_.state_clip(scene_.level().objects[t->object].type, state);
    if (clip.empty() && !state.empty()) clip = pick_clip(scene_.clip_names(t->object), {lower(state).c_str()});
    t->clip_idle = clip.empty() ? t->clip_idle_default : clip;
    if (t->state == Target::State::Idle && t->goals.empty() && !t->aware) play(*t, t->clip_idle, true);
    return true;
}

bool Combat::ignore_target(int npc, int target, bool on) {
    Target* t = find_target(npc);
    if (!t || !t->actor) return false;
    auto it = std::find(t->ignored.begin(), t->ignored.end(), target);
    if (on && it == t->ignored.end()) t->ignored.push_back(target);
    if (!on && it != t->ignored.end()) t->ignored.erase(it);
    t->choose_timer = 0;
    return true;
}

bool Combat::enemy_points(const Target& t, int enemy, glm::vec3& feet, glm::vec3& chest) const {
    (void)t;
    if (enemy == -1) {
        if (player_dead) return false;
        feet = player_.center - glm::vec3(0, 0, player_.radius.z);
        chest = player_center();
        return true;
    }
    if (enemy < 0 || size_t(enemy) >= targets_.size() || !alive(targets_[size_t(enemy)])) return false;
    const Target& e = targets_[size_t(enemy)];
    feet = e.pos;
    chest = e.pos + glm::vec3(0, 0, e.height * (e.actor ? 0.65f : 0.5f));
    return true;
}

bool Combat::fights(const Target& t, int enemy) const {
    const int id = enemy == -1 ? 0 : enemy >= 0 && size_t(enemy) < targets_.size() ? targets_[size_t(enemy)].id : -1;
    if (id < 0 || std::find(t.ignored.begin(), t.ignored.end(), id) != t.ignored.end()) return false;
    if (id == t.locked) return true;
    if (enemy == -1) return (t.attacks & kPlayerTeamBit) != 0 && !player_dead;
    const Target& e = targets_[size_t(enemy)];
    return &e != &t && e.actor && alive(e) && (t.attacks & (1u << e.team)) != 0;
}

void Combat::keep_enemies_on_player() {
    // CAIController::Update: if fewer than that many (or than there are hostile actors) of
    // the aware ones have the player as their target, the nearest of the others who could
    // fight it (not locked on another, not ignoring it) turn to it.
    if (player_dead || min_enemies_on_player <= 0) return;
    int registered = 0, on_player = 0;
    std::vector<std::pair<float, size_t>> could;
    const glm::vec3 player = logic_.player.center;
    for (size_t i = 0; i < targets_.size(); ++i) {
        const Target& t = targets_[i];
        if (!t.actor || !t.hostile || !alive(t)) continue;
        if (const LevelLogic::Object* o = logic_.object(t.id); !o || !o->enabled) continue;
        ++registered;
        if (!t.aware) continue;
        if (t.enemy == -1) ++on_player;
        else if (t.locked < 0 && fights(t, -1)) could.push_back({glm::distance(t.pos, player), i});
    }
    int want = std::min(registered, min_enemies_on_player) - on_player;
    if (want <= 0 || could.empty()) return;
    std::sort(could.begin(), could.end());
    for (size_t k = 0; k < could.size() && want > 0; ++k, --want) {
        Target& t = targets_[could[k].second];
        t.enemy = -1, t.choose_timer = 0.4f;
    }
}

int Combat::choose_enemy(const Target& t) const {
    // A locked target while it lasts; else the nearest enemy in sight (the current one if
    // none is), by team masks.
    if (t.locked == 0) return player_dead ? kNoEnemy : -1;
    if (t.locked > 0) {
        for (size_t i = 0; i < targets_.size(); ++i)
            if (targets_[i].id == t.locked) return alive(targets_[i]) ? int(i) : kNoEnemy;
        return kNoEnemy;
    }
    int best = kNoEnemy;
    float best_d = 1e9f;
    auto consider = [&](int enemy) {
        glm::vec3 feet, chest;
        if (!fights(t, enemy) || !enemy_points(t, enemy, feet, chest)) return;
        const float d = glm::distance(t.pos, feet) * (enemy == t.enemy ? 0.8f : 1.0f);   // stay on the current one
        if (d < best_d && d < t.sight && sees_point(t, chest)) best = enemy, best_d = d;
    };
    consider(-1);
    for (size_t i = 0; i < targets_.size(); ++i)
        if (targets_[i].actor) consider(int(i));
    if (best == kNoEnemy && t.enemy != kNoEnemy && fights(t, t.enemy)) return t.enemy;   // chase the one it had
    return best;
}

bool Combat::sees_point(const Target& t, const glm::vec3& target) const {
    const glm::vec3 eye = t.pos + glm::vec3(0, 0, t.height * 0.85f);
    const float d = glm::distance(eye, target);
    if (d > t.sight) return false;
    if (d > t.close) {
        const float off = std::abs(angle_diff(t.yaw, yaw_towards(t.pos, target)));
        if (off > t.fov * 0.5f) return false;
    }
    return line_clear(eye, target);
}

void Combat::notice(Target& t) {
    if (t.aware) return;
    t.aware = true;
    if (!t.hostile) return;
    logic_.runtime_.send_event("EnemyAware", t.id);
    // Others close by hear about it.
    for (Target& o : targets_)
        if (&o != &t && o.actor && o.hostile && !o.aware && alive(o) && glm::distance(o.pos, t.pos) < 15.0f) {
            o.aware = true;
            logic_.runtime_.send_event("EnemyAware", o.id);
        }
}

void Combat::face(Target& t, const glm::vec3& towards, float dt) {
    const float want = yaw_towards(t.pos, towards);
    const float d = angle_diff(t.yaw, want);
    const float step = 360.0f * dt;
    t.yaw += std::clamp(d, -step, step);
}

void Combat::move_along_path(Target& t, float dt, float speed_scale) {
    float budget = t.run_speed * speed_scale * dt;
    while (budget > 0 && t.path_next < t.path.size()) {
        const glm::vec3 next = t.path[t.path_next];
        glm::vec2 d = glm::vec2(next - t.pos);
        const float len = glm::length(d);
        // Corners count as reached within the actor's radius: crowds can't all stand on one point.
        const bool last = t.path_next + 1 == t.path.size();
        if (len < (last ? 0.05f : std::max(t.radius, 0.3f))) {
            ++t.path_next;
            continue;
        }
        const float step = std::min(budget, len);
        t.pos += glm::vec3(d / len * step, 0);
        budget -= step;
        face(t, next, dt);
        if (step >= len) ++t.path_next;
    }
    // Through a doorway the mesh has a gap: keep going in a straight line.
    if (t.path_next < t.path_off_mesh.size() && t.path_off_mesh[t.path_next]) {
        const glm::vec3 next = t.path[t.path_next];
        const int tri = nav_.locate(t.pos + glm::vec3(0, 0, 0.3f), 2.0f);
        if (tri >= 0) t.tri = tri, t.pos.z = nav_.height(tri, glm::vec2(t.pos));
        else t.pos.z += (next.z - t.pos.z) * std::min(1.0f, dt * 5);
        return;
    }
    stay_on_mesh(t);
}

void Combat::path_to(Target& t, const glm::vec3& goal_pos) {
    t.path.clear();
    t.path_off_mesh.clear();
    t.path_next = 0;
    glm::vec3 goal, here;
    const int goal_tri = nav_.nearest(goal_pos + glm::vec3(0, 0, 0.3f), 4.0f, goal);
    if (t.tri < 0) t.tri = nav_.nearest(t.pos + glm::vec3(0, 0, 0.3f), 4.0f, here);
    if (goal_tri >= 0 && t.tri >= 0)
        nav_.find_path(t.tri, t.pos, goal_tri, goal, t.path, [this](int door) { return logic_.door_passable(door); }, &t.path_off_mesh);
    if (t.path.empty()) {   // no route on the mesh: straight there
        t.path = {goal_pos};
        t.path_off_mesh = {1};
    }
}

void Combat::run_goal(Target& t, float dt) {
    NpcGoal& g = t.goals.front();
    const bool first = !g.started;
    g.started = true;
    g.timer += dt;
    bool done = false;
    switch (g.kind) {
        case NpcGoal::Kind::Move:
            if (first) path_to(t, g.pos);
            play(t, g.run || t.clip_walk.empty() ? t.clip_run : t.clip_walk, true);
            move_along_path(t, dt, g.run ? 1.0f : 0.45f);
            done = t.path_next >= t.path.size() || g.timer > 40;
            break;
        case NpcGoal::Kind::Jump: {
            // With the JumpUpDown states (the Alien Commander's): crouching, in the air, landing.
            const int type = scene_.level().objects[t.object].type;
            const std::string start = logic_.state_clip(type, "JumpUpDownStart"), air = logic_.state_clip(type, "JumpUpDownLoop"),
                              land = logic_.state_clip(type, "JumpUpDownStop");
            const bool staged = !start.empty() && !air.empty() && !land.empty();
            const float before = staged ? scene_.clip_length(t.object, start) : 0.0f, after = staged ? scene_.clip_length(t.object, land) : 0.0f;
            if (first) {
                g.length = std::clamp(glm::distance(t.pos, g.pos) / 8.0f, 0.4f, 1.2f), t.leap_from = t.pos;
                if (staged) play(t, start, false);
            }
            face(t, g.pos, dt);
            if (g.timer < before) break;   // crouching
            const float k = std::min((g.timer - before) / g.length, 1.0f);
            if (k < 1) {
                t.pos = glm::mix(t.leap_from, g.pos, k) + glm::vec3(0, 0, 4 * k * (1 - k) * 1.5f);
                play(t, staged ? air : t.clip_jump.empty() ? t.clip_run : t.clip_jump, true);
                break;
            }
            if (t.pos != g.pos) {   // down
                t.pos = g.pos;
                glm::vec3 on;
                t.tri = nav_.nearest(t.pos + glm::vec3(0, 0, 0.3f), 2.0f, on);
                if (staged) play(t, land, false);
            }
            done = g.timer >= before + g.length + after;
            break;
        }
        case NpcGoal::Kind::Turn:
            face(t, g.pos, dt);
            play(t, t.clip_idle, true);
            done = g.timer > 0.6f;
            break;
        case NpcGoal::Kind::Teleport:
            if (g.dash && first && try_dash(t, g.pos, true)) break;   // NPCSuperSpeedToWaypoint
            if (g.dash && t.dashing) break;
            if (!g.dash || first) t.pos = g.pos, t.tri = -1;
            done = true;
            break;
        case NpcGoal::Kind::Action:
            if (first) {
                // The state's clip, by name, else something that looks like using a console.
                const auto clips = scene_.clip_names(t.object);
                std::string clip = logic_.state_clip(scene_.level().objects[t.object].type, g.state);
                if (clip.empty()) clip = pick_clip(clips, {lower(g.state).c_str()});
                if (clip.empty()) clip = pick_clip(clips, {"console", "button", "interact", "_use"});
                if (!clip.empty()) play(t, clip, false), g.length = scene_.clip_length(t.object, clip);
                if (g.length <= 0) g.length = 1.5f, play(t, t.clip_idle, true);
            }
            done = g.timer >= g.length;
            break;
    }
    scene_.set_object_pose(t.object, t.pos, t.yaw);
    if (!done) return;
    const int id = g.id;
    t.goals.pop_front();
    t.state = Target::State::Idle;
    const std::string prefix = "NpcCmdFinished_" + std::to_string(t.id) + "_";
    logic_.runtime_.send_event(prefix + std::to_string(id));
    if (t.goals.empty()) logic_.runtime_.send_event(prefix + "-1");
    logic_.runtime_.send_event("NpcCmdFinished", t.id);
}

bool Combat::push_goal(int actor, const NpcGoal& g) {
    for (Target& t : targets_)
        if (t.id == actor && t.actor) {
            if (t.goals.size() < 16) t.goals.push_back(g);
            return true;
        }
    return false;
}

bool Combat::teleport_to_player(int npc, int goal_id) {
    const Target* t = find_target(npc);
    if (!t || !t->actor) return false;
    const glm::vec3 feet = player_center() - glm::vec3(0, 0, player_.radius.z);
    glm::vec3 away = t->pos - feet;
    away.z = 0;
    away = glm::length(away) > 1e-3f ? glm::normalize(away) : glm::vec3(0, 1, 0);
    NpcGoal g;
    g.kind = NpcGoal::Kind::Teleport;
    g.id = goal_id;
    g.pos = feet + away * 8.0f;
    for (float d : {8.0f, 5.0f, 3.0f}) {   // the nearest walkable point at that distance or closer
        glm::vec3 on;
        if (nav_.nearest(feet + away * d + glm::vec3(0, 0, 0.5f), 4.0f, on) >= 0) {
            g.pos = on;
            break;
        }
    }
    return push_goal(npc, g);
}

bool Combat::goal_pending(int actor, int goal_id) const {
    for (const Target& t : targets_)
        if (t.id == actor)
            return std::any_of(t.goals.begin(), t.goals.end(), [&](const NpcGoal& g) { return goal_id < 0 || g.id == goal_id; });
    return false;
}

bool Combat::place_actor(int id, const glm::vec3& position) {
    for (Target& t : targets_) {
        if (t.id != id || !t.actor) continue;
        t.pos = position;
        t.tri = -1;
        t.path.clear(), t.path_next = 0, t.leaping = false;
        glm::vec3 on;
        t.tri = nav_.nearest(position + glm::vec3(0, 0, 0.3f), 2.0f, on);
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return true;
    }
    return false;
}

void Combat::cancel_goals(int actor) {
    for (Target& t : targets_)
        if (t.id == actor) t.goals.clear();
}

void Combat::stay_on_mesh(Target& t) {
    // Follow the surface; pushed or cut off it, step back onto the nearest point.
    int tri = nav_.locate(t.pos + glm::vec3(0, 0, 0.3f), 2.0f);
    if (tri >= 0) {
        t.pos.z = nav_.height(tri, glm::vec2(t.pos));
    } else {
        glm::vec3 on;
        tri = nav_.nearest(t.pos + glm::vec3(0, 0, 0.3f), 3.0f, on);
        if (tri >= 0) t.pos = on;
    }
    if (tri >= 0) t.tri = tri;
}

void Combat::update_actor(Target& t, float dt) {
    using S = Target::State;
    t.timer += dt;
    t.hurt_cooldown = std::max(t.hurt_cooldown - dt, 0.0f);
    if (t.state == S::Dying) {
        const bool falling = t.ragdoll && t.ragdoll->step(dt, scene_.collision(), scene_);
        if (t.ragdoll) {   // where the body is (its placement moves with it; the bones are its own)
            const glm::vec3 c = t.ragdoll->centre();
            t.pos.x = c.x, t.pos.y = c.y;
            scene_.set_object_pose(t.object, t.pos, t.yaw);
        }
        if (t.ragdoll ? !falling : scene_.clip_finished(t.object)) {
            t.state = S::Dead;
            // The death clip done: dissolving through the death material, with its sound.
            if (!t.death_model.empty() && scene_.set_object_material(t.object, t.death_model, t.death_material)) {
                t.dissolve = 0;
                scene_.set_object_param(t.object, "BlendFactor", glm::vec4(-0.5f), 1);
                if (!t.vanish_sound.empty()) {
                    const glm::vec3 at = t.pos + glm::vec3(0, 0, 0.5f);
                    sound({t.vanish_sound}, &at);
                }
            }
        }
        return;
    }
    if (t.state == S::Dead) {
        if (t.dissolve >= 0) {   // BlendFactor -0.5 (all there) to 0.5 (all gone) over 1.5 s
            t.dissolve += dt;
            scene_.set_object_param(t.object, "BlendFactor", glm::vec4(std::min(t.dissolve / 1.5f, 1.0f) - 0.5f), 1);
            if (t.dissolve >= 1.5f) {
                t.dissolve = -1;
                if (LevelLogic::Object* o = logic_.object(t.id); o && o->visible) logic_.show(*o, false);
                scene_.set_object_material(t.object, "", "");   // its own materials again, should it come back
            }
        }
        return;
    }
    t.dash_wait = std::max(t.dash_wait - dt, 0.0f);
    t.jump_wait -= dt, t.throw_wait -= dt;
    if (t.dashing) {
        update_dash(t, dt);
        if (t.dashing || t.goals.empty()) return;
    }
    if (!t.goals.empty()) {
        run_goal(t, dt);
        return;
    }
    if (t.paralyzed > 0) {   // the Paralyze power: frozen in place
        update_paralysis(t, dt);
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return;
    }
    if (t.ai_locked) {   // lock_ai: stands as it is
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return;
    }
    if (t.attacks == 0 && t.locked < 0) return;
    if ((t.choose_timer -= dt) <= 0) t.choose_timer = 0.4f, t.enemy = choose_enemy(t);
    glm::vec3 target, chest;
    if (!enemy_points(t, t.enemy, target, chest)) t.enemy = kNoEnemy, target = t.pos, chest = t.pos;
    const bool on_player = t.enemy == -1;
    const float dist = glm::distance(glm::vec2(t.pos), glm::vec2(target));
    const bool sees = t.enemy != kNoEnemy && sees_point(t, chest);
    if (sees) {
        // Noticing takes a moment, scaled by the global awareness factor (0: never).
        t.awareness += dt * 2.0f * awareness_factor;
        if (t.awareness >= 1 || t.locked >= 0) notice(t);
        t.lost_sight = 0;
    } else {
        t.lost_sight += dt;
    }
    if (t.gun >= 0) {
        update_gunner(t, dt, chest, sees);
        return;
    }
    if (t.mobile && !t.hold && !nav_.empty() && (t.leaping || t.tri < 0) && (t.aware || t.spawned)) {
        if (!t.leaping) {
            glm::vec3 to;
            const int tri = nav_.nearest(t.pos, 15.0f, to);
            if (tri >= 0) {
                t.leaping = true, t.leap_t = 0, t.leap_from = t.pos, t.leap_to = to, t.leap_tri = tri;
                play(t, t.clip_jump.empty() ? t.clip_run : t.clip_jump, true);
            }
        }
        if (t.leaping) {
            const float duration = std::clamp(glm::distance(t.leap_from, t.leap_to) / 8.0f, 0.4f, 1.0f);
            t.leap_t = std::min(t.leap_t + dt / duration, 1.0f);
            t.pos = glm::mix(t.leap_from, t.leap_to, t.leap_t) + glm::vec3(0, 0, 4 * t.leap_t * (1 - t.leap_t) * 1.5f);
            face(t, t.leap_to, dt);
            if (t.leap_t >= 1) t.leaping = false, t.tri = t.leap_tri, t.pos = t.leap_to;
            scene_.set_object_pose(t.object, t.pos, t.yaw);
            return;
        }
    }
    // Allies hold their ground: they fight what they see but do not hunt.
    const bool idle = !t.aware || t.enemy == kNoEnemy || (!t.hostile && !sees && t.locked < 0);
    if (idle && t.cover >= 0) leave_cover(t);
    if (idle) {
        if (!t.hostile) {   // allies: leave whatever the scripts play alone, unless they were fighting
            const glm::vec3 feet = player_.center - glm::vec3(0, 0, player_.radius.z);
            if (t.follow && !t.hold && t.mobile && glm::distance(glm::vec2(t.pos), glm::vec2(feet)) > 5.0f) {
                t.state = S::Chase;   // follow_mode: keep up with the player
                go_towards(t, feet, 3.0f, dt);
                scene_.set_object_pose(t.object, t.pos, t.yaw);
                return;
            }
            if (t.state != S::Idle) t.state = S::Idle, play(t, t.clip_idle, true);
            return;
        }
        if (t.state != S::Idle) t.state = S::Idle;
        play(t, t.aware ? t.clip_alert : t.clip_idle, true);
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return;
    }

    if (t.held >= 0) {   // telekinesis under way
        update_throw(t, dt, chest);
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return;
    }
    // ChooseAttack: between attacks, a telekinetic may throw an object instead.
    if (t.state == S::Attack && t.attack_state.empty() && t.reload_left <= 0 && sees && try_throw(t, target)) {
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return;
    }
    // CAIComponent::HideFromThreats: a jumper not busy attacking or reloading may jump.
    const bool between_attacks = t.attack_choices.empty() ? t.shots_left <= 0 : t.attack_state.empty();
    if ((t.state == S::Chase || (t.state == S::Attack && between_attacks)) && t.reload_left <= 0 && t.cover < 0 &&
        try_jump(t, target, chest)) {
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return;
    }
    // CAIComponent::CanAttack: the enemy out of sight, a dasher dashes aside to where it is in
    // sight (TrySSRandom), out of any cover.
    if (t.state != S::Hurt && t.state != S::Melee && !sees && t.lost_sight > 0.5f && try_dash_aside(t, target, chest)) {
        leave_cover(t);
        return;
    }
    // Tactical fighters take cover and fire from it (combat_ai.cpp).
    if (t.state != S::Hurt && t.state != S::Melee && fight_from_cover(t, dt, target, chest, sees, on_player)) {
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return;
    }
    switch (t.state) {
    case S::Hurt:
        if (scene_.clip_finished(t.object) || t.timer > 0.8f) t.state = S::Chase, t.timer = 0;
        break;
    case S::Melee:
        face(t, target, dt);
        if (!t.melee_hit && t.timer > 0.45f) {
            t.melee_hit = true;
            if (dist < t.melee_range + 0.8f && std::abs(target.z - t.pos.z) < 2.5f) {
                if (on_player) damage_player(t.melee_damage, t.pos);
                else if (t.enemy >= 0) apply_damage(targets_[size_t(t.enemy)], t.melee_damage, false);
            }
        }
        if (scene_.clip_finished(t.object) || t.timer > 1.4f) t.state = S::Chase, t.timer = 0;
        break;
    case S::Attack:
        face(t, target, dt);
        if (t.reload_left <= 0 && t.attack_state.empty()) {   // between attacks by states: their attack idle
            const std::vector<std::string> idle = t.attack_choices.empty() ? std::vector<std::string>{} : logic_.state_clips(scene_.level().objects[t.object].type, "AttackIdle");
            play(t, idle.empty() ? t.clip_fire : idle[0], true);
        }
        attack_fire(t, chest, on_player, sees, dt);
        // Lost the line of fire, or too far: close in again.
        if ((!sees && t.lost_sight > 0.8f) || dist > t.fire_range * 1.2f) t.state = S::Chase, t.timer = 0, t.token = false, t.shots_left = 0;
        if (dist < t.melee_range && !t.clip_melee.empty() && t.fire_timer > 0.3f) {
            t.state = S::Melee, t.timer = 0, t.melee_hit = false;
            play(t, t.clip_melee, false);
        }
        break;
    case S::Idle:
    case S::Chase:
    default: {
        t.state = S::Chase;
        const bool in_melee = dist < t.melee_range;
        if (in_melee && !t.ranged) {
            t.state = S::Melee, t.timer = 0, t.melee_hit = false;
            play(t, t.clip_melee, false);
            break;
        }
        if (t.ranged && sees && dist < t.fire_range) {
            t.state = S::Attack, t.timer = 0;
            break;
        }
        if (!t.mobile || t.hold || nav_.empty() || t.tri < 0) {
            face(t, target, dt);
            play(t, t.clip_alert, true);
            break;
        }
        // Within its region (link_region): as close to the enemy as the box allows.
        glm::vec3 goal = target;
        if (t.has_region) goal = glm::vec3(glm::clamp(glm::vec2(target), glm::vec2(t.region_min), glm::vec2(t.region_max)), target.z);
        go_towards(t, goal, t.ranged ? 1.5f : t.melee_range * 0.8f, dt);
        if (t.has_region && t.path_next >= t.path.size()) face(t, target, dt);
        break;
    }
    }
    scene_.set_object_pose(t.object, t.pos, t.yaw);
}

// Firing at the enemy: the aim sweep and the bursts (one token-holder's burst at a time at the
// player; the attack choice script picks each burst's shots).
bool Combat::need_reload(const Target& t, int shots) const {
    if (t.mag_size <= 0) return false;
    shots = std::min(shots, t.mag_size / t.per_shot);
    return t.mag < t.per_shot * shots;
}

void Combat::refill(Target& t) {
    t.mag = t.mag_size;
    const glm::vec3 at = t.pos + glm::vec3(0, 0, 1);
    sound(t.reload_sounds, &at);
}

const std::unordered_map<std::string, Combat::StateDef>& Combat::states_of(int type) {
    if (auto it = states_cache_.find(type); it != states_cache_.end()) return it->second;
    auto& out = states_cache_[type];
    std::function<void(int)> gather = [&](int id) {
        auto g = gol_.find(id);
        if (g == gol_.end()) return;
        if (auto t = g->second.templates.find(0x0e); t != g->second.templates.end() && t->second.size() > 1 && out.empty()) {
            const auto& list = t->second[1].list;
            for (const Value& st : list) {
                if (st.size() < 12 || st[0].type != Value::String) continue;
                StateDef d;
                d.loop = st[2].i != 0;
                if (const int n = st[6].i; n > 0 && size_t(n) < list.size() && list[size_t(n)].size() > 0) d.next = list[size_t(n)][0].s;
                for (const Value& clip : st[11].list) {
                    if (clip.size() < 3 || clip[0].type != Value::String) continue;
                    std::vector<std::pair<float, int>> ev;
                    for (const Value& group : clip[2].list)
                        if (group.size() >= 3 && group[0].type == Value::String && group[0].s == "GameEvent")
                            for (const Value& e : group[2].list)
                                if (e.size() >= 4) ev.emplace_back(e[1].as_float(), e[3].i);
                    d.clips.emplace_back(clip[0].s, std::move(ev));
                }
                out.emplace(st[0].s, std::move(d));
            }
        }
        for (int p : g->second.parents) gather(p);
    };
    gather(type);
    return out;
}

bool Combat::play_attack_state(Target& t, const std::string& state) {
    const auto& st = states_of(scene_.level().objects[t.object].type);
    auto it = st.find(state);
    if (it == st.end() || it->second.clips.empty()) return false;
    const auto& c = it->second.clips[size_t(rng_()) % it->second.clips.size()];
    if (!scene_.play_clip(t.object, c.first, it->second.loop)) return false;
    t.playing = c.first, t.attack_state = state, t.attack_clip = c.first, t.attack_ms = -1;
    if (logic_.trace) log(std::to_string(t.id) + " attack state " + state + " (" + c.first + ")");
    return true;
}

// An attack by states (CNPCComponent::Attack: the chosen state, then the ones after it while
// they are attacks): each GameEvent 1/2/23 fires its first weapon, 3/18 its second
// (CNPCComponent::OnEvent: FireWithWeapons), all as its one projectile here.
void Combat::event_attack(Target& t, const glm::vec3& chest, bool on_player) {
    if (t.attack_state.empty()) {
        if (t.fire_timer > 0) return;
        int shooters = 0;
        for (const Target& o : targets_) shooters += o.token && &o != &t && alive(o);
        if (on_player && (shooters >= max_shooters_ || burst_cooldown_ > 0)) {
            t.fire_timer = 0.3f;
            return;
        }
        float total = 0;
        for (const auto& [n, w] : t.attack_choices) total += w;
        float roll = std::uniform_real_distribution<float>(0.0f, total)(rng_);
        std::string pick = t.attack_choices.back().first;
        for (const auto& [n, w] : t.attack_choices)
            if ((roll -= w) < 0) {
                pick = n;
                break;
            }
        if (!play_attack_state(t, pick)) return;
        if (on_player) t.token = true, burst_cooldown_ = kBurstInterval;
        t.shots_left = t.burst;   // the aim sweep's bookkeeping
    }
    const auto& st = states_of(scene_.level().objects[t.object].type);
    auto it = st.find(t.attack_state);
    Scene::ClipState cs;
    if (it == st.end() || t.playing != t.attack_clip || !scene_.clip_state(t.object, cs)) {
        t.attack_state.clear(), t.token = false;
        return;
    }
    for (const auto& c : it->second.clips) {
        if (c.first != t.attack_clip) continue;
        for (const auto& [ms, id] : c.second)
            if (ms > t.attack_ms && ms <= cs.ms && (id == 1 || id == 2 || id == 3 || id == 18 || id == 23)) fire_bolt(t, chest);
    }
    t.attack_ms = cs.ms;
    if (!scene_.clip_finished(t.object)) return;
    // On to the next attack state with a clip, or done.
    const std::string next = it->second.next;
    auto n = st.find(next);
    if (!next.empty() && next != t.attack_state && n != st.end() && !n->second.loop && !n->second.clips.empty() && play_attack_state(t, next)) return;
    t.attack_state.clear(), t.token = false;
    t.fire_timer = t.burst_pause * std::uniform_real_distribution<float>(0.8f, 1.6f)(rng_);
}

void Combat::attack_fire(Target& t, const glm::vec3& chest, bool on_player, bool sees, float dt) {
    // Reloading (DoReload: the Reload state's clip), and out of rounds: a reload first.
    if (t.reload_left > 0) {
        if ((t.reload_left -= dt) <= 0) refill(t), t.fire_timer = std::min(t.fire_timer, 0.2f);
        return;
    }
    if (need_reload(t, 1)) {
        t.token = false, t.shots_left = 0;
        const std::vector<std::string> clips = logic_.state_clips(scene_.level().objects[t.object].type, t.crouched ? "CrouchReload" : "Reload");
        Scene::ClipState cs;
        if (!clips.empty() && scene_.play_clip(t.object, clips[size_t(rng_()) % clips.size()], false) && scene_.clip_state(t.object, cs)) {
            t.playing = cs.name, t.reload_left = std::max(cs.length / 1000.0f, 0.3f);
            if (logic_.trace) log(std::to_string(t.id) + " reloads (" + cs.name + ")");
        } else {
            refill(t);
        }
        return;
    }
    if (sees) {   // tracking it: the sweep closes in, the aim time grows
        t.aim_time += dt;
        if (!t.locked_on) {
            t.sweep_r = std::max(t.sweep_r - t.sweep_rate * dt, t.sweep_min);
            t.sweep_a = std::fmod(t.sweep_a + t.sweep_speed * t.sweep_sign * dt + 360.0f, 360.0f);
            t.locked_on = t.sweep_r <= t.sweep_min;
        }
    } else {   // CNPCWeaponComponent::ResetTargetLocked
        t.locked_on = false, t.sweep_r = t.sweep_max, t.aim_time = 0, t.spread = 0;
        t.sweep_sign = std::uniform_int_distribution<int>(0, 1)(rng_) ? 1.0f : -1.0f;
    }
    t.fire_timer -= dt;
    if (!t.attack_choices.empty()) {
        event_attack(t, chest, on_player);
        return;
    }
    if (t.fire_timer <= 0) {
        if (t.shots_left <= 0) {
            // A new burst at the player needs one of the few attack tokens; otherwise aim and wait.
            int shooters = 0;
            for (const Target& o : targets_) shooters += o.token && &o != &t && alive(o);
            if (on_player && (shooters >= max_shooters_ || burst_cooldown_ > 0)) {
                t.fire_timer = 0.3f;
                play(t, t.clip_alert, true);
                return;
            }
            t.shots_left = t.burst;
            // The type's attack choice (BA_ChooseAttack: by accuracy and chance) picks a
            // behaviour: its shots and the gap between them.
            t.behaviour.clear();
            if (!t.choose_fn.empty()) logic_.runtime_.call(t.choose_fn, t.id, t.id);
            for (const Target::Behaviour& b : t.behaviours)
                if (b.name == t.behaviour) t.shots_left = b.shots, t.burst_gap = b.gap;
            if (on_player) t.token = true, burst_cooldown_ = kBurstInterval;
        }
        fire_bolt(t, chest);
        --t.shots_left;
        if (t.shots_left > 0) {
            t.fire_timer = t.burst_gap;
        } else {
            t.token = false;
            t.fire_timer = t.burst_pause * std::uniform_real_distribution<float>(0.8f, 1.6f)(rng_);
        }
    }
}

void Combat::go_towards(Target& t, const glm::vec3& target, float stop, float dt) {
    t.repath -= dt;
    const bool crossing = t.path_next < t.path_off_mesh.size() && t.path_off_mesh[t.path_next];
    if (!crossing && (t.repath <= 0 || glm::distance(t.path_goal, target) > 2.0f)) {
        t.repath = 0.5f;
        glm::vec3 goal;
        const int goal_tri = nav_.nearest(target + glm::vec3(0, 0, 0.3f), 4.0f, goal);
        t.path.clear();
        t.path_next = 0;
        if (goal_tri >= 0 && t.tri >= 0 &&
            nav_.find_path(t.tri, t.pos, goal_tri, goal, t.path, [this](int door) { return logic_.door_passable(door); },
                           &t.path_off_mesh))
            t.path_goal = target;
    }
    if (t.path_next < t.path.size() && glm::distance(glm::vec2(t.pos), glm::vec2(target)) > stop) {
        play(t, t.clip_run, true);
        move_along_path(t, dt);
    } else {
        face(t, target, dt);
        play(t, t.hostile || t.aware ? t.clip_alert : t.clip_idle, true);
    }
}

float Combat::accuracy(int npc) const {
    const Target* t = find_target(npc);
    if (!t) return 0;
    glm::vec3 feet, chest;
    if (!enemy_points(*t, t->enemy, feet, chest)) return 0;
    const float dist = glm::distance(t->pos, feet);
    const float k = dist < t->acc_near ? 1.0f : dist < t->acc_far ? (dist - t->acc_near) / (t->acc_far - t->acc_near) : 0.0f;
    const float need = (1 - k) * t->acc_time;
    return need <= 0 || t->aim_time >= need ? 1.0f : t->aim_time / need;
}

void Combat::set_attack_behaviour(int npc, const std::string& name) {
    if (Target* t = find_target(npc)) t->behaviour = name;
}

ProjectileLooks Combat::read_looks(int type) const {
    ProjectileLooks l;
    auto p = gol_.find(type);
    if (p == gol_.end()) return l;
    if (const Value* pr = resolved_template(p->second, gol_, kCompProjectile); pr && pr->size() > 36) {
        l.trail = (*pr)[36].i;
        if ((*pr)[22].i != 0 && (*pr)[23].type == Value::String && !(*pr)[23].s.empty()) l.fx.push_back((*pr)[23].s);
    }
    if (const Value* fx = resolved_template(p->second, gol_, 0x92); fx && fx->size() > 0)
        for (const Value& e : (*fx)[0].list)
            if (e.size() > 2 && e[0].type == Value::String && !e[0].s.empty() && e[2].i != 0) l.fx.push_back(e[0].s);
    if (const Value* mesh = resolved_template(p->second, gol_, 0x01); mesh && mesh->size() > 0 && (*mesh)[0].type == Value::String &&
        !(*mesh)[0].s.empty())
        l.body = type;
    return l;
}

std::vector<std::string> Combat::first_state_clips(int type) const {
    std::vector<std::string> out;
    auto p = gol_.find(type);
    if (p == gol_.end()) return out;
    if (const Value* v = resolved_template(p->second, gol_, 0x0e); v && v->size() > 1 && (*v)[1].size() > 0)
        if (const Value& st = (*v)[1][0]; st.size() > 11)
            for (const Value& clip : st[11].list)
                if (clip.size() > 0 && clip[0].type == Value::String) out.push_back(clip[0].s);
    if (const Value* a = resolved_template(p->second, gol_, 0x0c); out.empty() && a && a->size() > 1 && (*a)[1].type == Value::String &&
        !(*a)[1].s.empty())
        out.push_back((*a)[1].s);   // no states: its animation's clip
    return out;
}

void Combat::build_beams(const glm::vec3& eye, ParticleList& out) {
    // Turned about its length to face the eye; the texture tiles along it (a square per
    // two widths) and runs forward, the whole fading over a second.
    for (const Beam& b : beams_) {
        const GpuTexture* tex = scene_.texture(b.texture);
        const float len = glm::distance(b.from, b.to);
        if (!tex || len < 0.01f) continue;
        const glm::vec3 dir = (b.to - b.from) / len;
        glm::vec3 side = glm::cross(dir, eye - (b.from + b.to) * 0.5f);
        side = (glm::length(side) > 1e-4f ? glm::normalize(side) : glm::vec3(0, 0, 1)) * (b.width * 0.5f);
        const float u0 = -b.age * 4.0f, u1 = u0 + len / (b.width * 2);
        const uint32_t rgba = pack_rgba(glm::vec4(1, 1, 1, 1 - b.age));
        const ParticleVertex v[4] = {{b.from + side, {u0, 0}, rgba}, {b.to + side, {u1, 0}, rgba},
                                     {b.to - side, {u1, 1}, rgba}, {b.from - side, {u0, 1}, rgba}};
        out.batches.push_back({tex, true, uint32_t(out.vertices.size()), 6});
        out.vertices.insert(out.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
    }
}

void Combat::hurt_look(Target& t, const glm::vec3* at) {
    if (t.hurt_technique.empty() || (t.hurt_time >= 0 && t.hurt_time < t.hurt_again)) return;
    if (t.hurt_time < 0 && !scene_.set_object_technique_all(t.object, t.hurt_technique)) return;
    t.hurt_time = t.hurt_start;
    if (at && !t.hurt_impact_param.empty()) scene_.set_object_param(t.object, t.hurt_impact_param, glm::vec4(*at, 0), 3);
    set_hurt_time(t);
}

// The time parameter ([3], "HurtTime") is in no effect: NV_NormalRim's hurt wave reads
// its radius from the unbound uniform HurtDist, which it is taken to drive, a 0.15 m ring
// spreading from the hit.
void Combat::set_hurt_time(Target& t) {
    if (!t.hurt_param.empty()) scene_.set_object_param(t.object, t.hurt_param, glm::vec4(t.hurt_time), 1);
    scene_.set_object_param(t.object, "HurtDist", glm::vec4(t.hurt_time), 1);
}

void Combat::update_hurt_looks(float dt) {
    for (Target& t : targets_) {
        if (t.hurt_time < 0) continue;
        t.hurt_time += dt;
        if (t.hurt_time > t.hurt_end) {
            scene_.set_object_technique_all(t.object, "");
            t.hurt_time = -1;
        } else {
            set_hurt_time(t);
        }
    }
}

void Combat::place_attachments() {
    // CCharacterShadowPlaneComponent::Update: each actor's shadow under it while it lives.
    for (Target& t : targets_) {
        if (t.shadow == size_t(-1)) continue;
        const LevelLogic::Object* o = logic_.object(t.id);
        const bool shown = o && o->visible && o->enabled && alive(t) && t.cloak == Target::Cloak::Visible;
        scene_.set_object_visible(t.shadow, shown);
        if (!shown) {
            t.shadow_factor = 0;
            continue;
        }
        const float y = glm::radians(t.yaw);
        const glm::vec3 at = t.pos + glm::vec3(t.shadow_offset.x * std::cos(y) - t.shadow_offset.y * std::sin(y),
                                               t.shadow_offset.x * std::sin(y) + t.shadow_offset.y * std::cos(y), t.shadow_offset.z);
        scene_.set_object_pose(t.shadow, at + glm::vec3(0, 0, 0.02f), t.yaw);   // just off the floor: depth-tested here
        const glm::mat4 into = glm::inverse(scene_.object_matrix(t.shadow));   // the shader works in the plane's space
        for (size_t i = 0; i < t.shadow_nodes.size(); ++i) {
            glm::vec3 d(into * scene_.object_matrix(t.object, t.shadow_nodes[i])[3]);
            d.z = std::clamp(d.z, i < t.shadow_low.size() ? t.shadow_low[i] : 0.0f, 100.0f);
            scene_.set_object_param(t.shadow, "SPosition" + std::to_string(i), glm::vec4(d, 0), 3);
        }
        // Faded in while the player sees it (within 32 m), out when not: a tenth of the way a frame.
        float target = 1;
        if (t.shadow_seen) {
            const glm::vec3 eye = player_.eye(), to = at + glm::vec3(0, 0, 0.02f) - eye;
            const float len = glm::length(to);
            target = len < 1e-3f || line_clear(eye, eye + to * (std::min(len, 32.0f) / len)) ? 1.0f : 0.0f;
        }
        t.shadow_factor += (target - t.shadow_factor) * 0.1f;
        scene_.set_object_param(t.shadow, "Factor", glm::vec4(t.shadow_factor), 1);
    }
    for (Target& t : targets_) {
        if (t.attached.empty()) continue;
        const LevelLogic::Object* o = logic_.object(t.id);
        const bool shown = o && o->visible && o->enabled;
        for (Target::Attachment& a : t.attached) {
            if (shown) scene_.set_object_transform(a.object, scene_.object_matrix(t.object, a.node));
            if (shown != a.shown) scene_.set_object_visible(a.object, shown), a.shown = shown;
        }
        if (t.muzzle_shown) {
            if (!shown || scene_.clip_finished(t.muzzle_object)) {
                scene_.set_object_visible(t.muzzle_object, false), t.muzzle_shown = false;
            } else {
                scene_.set_object_transform(t.muzzle_object, scene_.object_matrix(t.attached[0].object, t.muzzle_node));
            }
        }
    }
}

void Combat::launch_bolt(Bolt b, const ProjectileLooks& looks) {
    const glm::vec3 dir = glm::normalize(b.vel);
    if (looks.trail >= 0 && (b.trail = take_trail(looks.trail, b.pos)) != size_t(-1)) {
        b.trail_type = looks.trail, b.from = b.pos;
        scene_.play_clip(b.trail, "start", false);
        scene_.set_object_visible(b.trail, true);
    }
    const glm::mat4 placed = bolt_matrix(b.pos, dir);
    for (const std::string& e : looks.fx)
        if (const int h = scene_.particles().start(e, placed); h >= 0) b.fx.push_back(h);
    if (looks.body >= 0 && (b.body = take_trail(looks.body, b.pos)) != size_t(-1)) {
        b.body_type = looks.body;
        scene_.set_object_transform(b.body, placed);
        scene_.set_object_visible(b.body, true);
    }
    bolts_.push_back(std::move(b));
}

// A projectile's placement: at `pos`, its +Y along the flight.
glm::mat4 Combat::bolt_matrix(const glm::vec3& pos, const glm::vec3& dir) {
    glm::vec3 side = glm::cross(dir, glm::vec3(0, 0, 1));
    side = glm::length(side) > 1e-4f ? glm::normalize(side) : glm::vec3(1, 0, 0);
    glm::mat4 m(1);
    m[0] = glm::vec4(side, 0), m[1] = glm::vec4(dir, 0), m[2] = glm::vec4(glm::cross(side, dir), 0), m[3] = glm::vec4(pos, 1);
    return m;
}

const Combat::GunDef* Combat::gun_def(int type) {
    if (auto it = guns_.find(type); it != guns_.end()) return &it->second;
    auto g = gol_.find(type);
    const Value* v = g == gol_.end() ? nullptr : resolved_template(g->second, gol_, 0x43);
    if (!v || v->size() < 15) return nullptr;
    GunDef d;
    if (auto b = gol_.find((*v)[0].i); b != gol_.end())
        if (const Value* pr = resolved_template(b->second, gol_, kCompProjectile); pr && pr->size() > 8) {
            d.damage = num(*pr, 2, 20), d.speed = std::max(num(*pr, 8, 480), 10.0f);
            d.impacts = read_impacts(*pr);
            d.looks = read_looks((*v)[0].i);
        }
    d.turn = num(*v, 3, 90), d.pitch_min = num(*v, 4, -45), d.pitch_max = num(*v, 5, 45);
    d.gunner_node = str(*v, 6);
    d.interval = std::max(num(*v, 7, 200), 30.0f) / 1000.0f;
    d.burst_min = num(*v, 8, 3000) / 1000.0f, d.burst_max = num(*v, 9, 5000) / 1000.0f;
    d.pause_min = num(*v, 10, 1000) / 1000.0f, d.pause_max = num(*v, 11, 3000) / 1000.0f;
    d.loop_sound = str(*v, 13), d.stop_sound = str(*v, 14);
    return &(guns_[type] = std::move(d));
}

// CMachineGunAIComponent::Update: the gun turns to its gunner's target within its arc
// (UpdateBodyRotation / CheckAndUpdateRotation) and, on it, fires bursts with pauses
// between (UpdateAttack, StartFire / StopFire); the gunner stands behind it.
void Combat::update_gunner(Target& t, float dt, const glm::vec3& chest, bool sees) {
    LevelLogic::Object* gun = logic_.object(t.gun);
    const GunDef* d = gun && gun->index < scene_.level().objects.size() ? gun_def(scene_.level().objects[gun->index].type) : nullptr;
    if (!d) {
        t.gun = -1;
        return;
    }
    const bool target = t.aware && t.enemy != kNoEnemy && sees;
    const float want = target ? yaw_towards(gun->position, chest) : gun->yaw;
    const float aim = gun->yaw + std::clamp(angle_diff(gun->yaw, want), -d->turn * 0.5f, d->turn * 0.5f);
    const float step = 120.0f * dt;
    t.gun_yaw += std::clamp(angle_diff(t.gun_yaw, aim), -step, step);
    scene_.set_object_pose(gun->index, gun->position, t.gun_yaw);
    glm::vec3 at;
    if (!(!d->gunner_node.empty() && scene_.node_position(gun->index, d->gunner_node, at)) &&
        !scene_.node_position(gun->index, "Dummy_poz_hands_on_minigun", at)) {
        const float y = glm::radians(t.gun_yaw);
        at = gun->position - glm::vec3(-std::sin(y), std::cos(y), 0) * 1.3f;
    }
    t.pos = glm::vec3(at.x, at.y, gun->position.z), t.yaw = t.gun_yaw;
    scene_.set_object_pose(t.object, t.pos, t.yaw);
    play_state(t, "AttackMachineGun", true);
    const bool on_it = target && std::abs(angle_diff(t.gun_yaw, want)) < 5.0f;
    auto stop_loop = [&] {
        if (t.gun_loop >= 0 && logic_.audio) logic_.audio->stop(t.gun_loop, 50);
        t.gun_loop = -1;
    };
    auto between = [&](float a, float b) { return std::uniform_real_distribution<float>(std::min(a, b), std::max(a, b))(rng_); };
    switch (t.gun_state) {
        case 0:   // idle: a burst once it is on a target
            if (on_it) {
                t.gun_state = 1, t.gun_timer = between(d->burst_min, d->burst_max), t.gun_shot = 0;
                if (logic_.audio && !d->loop_sound.empty()) {
                    Audio::Params p;
                    p.position = &gun->position, p.loop = 1;
                    t.gun_loop = logic_.audio->play(d->loop_sound, p);
                }
            }
            break;
        case 1: {   // a burst: a shot every interval while on it
            t.gun_timer -= dt, t.gun_shot -= dt;
            if (on_it && t.gun_shot <= 0) {
                t.gun_shot = d->interval;
                glm::vec3 muzzle;
                if (!scene_.node_position(gun->index, "muzzle", muzzle)) muzzle = gun->position + glm::vec3(0, 0, 1.4f);
                glm::vec3 dir = glm::normalize(chest - muzzle);
                std::uniform_real_distribution<float> u(-1.0f, 1.0f);   // a little spread
                const glm::vec3 side = glm::normalize(glm::cross(dir, glm::vec3(0, 0, 1)) + glm::vec3(1e-6f, 0, 0)), up = glm::cross(side, dir);
                dir = glm::normalize(dir + (side * u(rng_) + up * u(rng_)) * std::tan(glm::radians(1.0f)));
                Bolt b{muzzle, dir * d->speed, d->damage, 3.0f, glm::vec4(1, 0.8f, 0.4f, 1), int(&t - targets_.data())};
                launch_bolt(b, d->looks);
                ++stats_.enemy_shots;
            }
            if (t.gun_timer <= 0 || !target) {
                t.gun_state = 2, t.gun_timer = between(d->pause_min, d->pause_max);
                stop_loop();
                if (!d->stop_sound.empty()) sound(d->stop_sound, &gun->position);
            }
            break;
        }
        default:   // a pause
            if ((t.gun_timer -= dt) <= 0) t.gun_state = 0;
            break;
    }
}

// CDropShipAIComponent::Update / Fire.
void Combat::update_dropship(Target& t, float dt) {
    if (!alive(t)) return;
    const glm::vec3 at(scene_.object_matrix(t.object)[3]);
    const glm::vec3 eye = player_.eye();
    const bool target = t.hostile && !player_dead && glm::distance(at, eye) < t.sight && line_clear(at, eye);
    const float off = target ? angle_diff(t.ds_yaw, yaw_towards(at, eye)) : 0.0f;
    // Hovering (not on its path), it turns to face its target: up to its turn speed, slowing
    // to stop facing it.
    if (!t.ds_no_turn && !logic_.mover_moving(t.id) && target && std::abs(off) > 30.0f) {
        const float most = std::min(t.ds_turn_max, std::sqrt(2 * t.ds_turn_accel * std::abs(off)));
        const float want = off > 0 ? most : -most;
        t.ds_turn += std::clamp(want - t.ds_turn, -t.ds_turn_accel * dt, t.ds_turn_accel * dt);
    } else {
        t.ds_turn -= std::clamp(t.ds_turn, -t.ds_turn_accel * dt, t.ds_turn_accel * dt);
    }
    if (std::abs(t.ds_turn) > 1e-3f) {
        t.ds_yaw += t.ds_turn * dt;
        scene_.set_object_pose(t.object, at, t.ds_yaw);
    }
    // Facing it: Attack (its fire clip at half speed, the rocket at 321 ms of it), then a
    // pause before the next.
    if (t.ds_fire_at >= 0) {
        if ((t.ds_fire_at -= dt) > 0) return;
        t.ds_fire_at = -1, t.ds_wait = t.ds_pause;
        if (!target) return;
        glm::vec3 muzzle = at;
        if (!t.ds_muzzle.empty()) scene_.node_position(t.object, t.ds_muzzle, muzzle);
        glm::vec3 dir = glm::normalize(eye - muzzle);
        // Off by [8]-[9] degrees, any way round.
        const glm::vec3 side = glm::normalize(glm::cross(dir, glm::vec3(0, 0, 1)) + glm::vec3(1e-6f, 0, 0)), up = glm::cross(side, dir);
        const float miss = glm::radians(std::uniform_real_distribution<float>(t.ds_miss_min, std::max(t.ds_miss_max, t.ds_miss_min))(rng_));
        const float round = std::uniform_real_distribution<float>(0, glm::two_pi<float>())(rng_);
        dir = glm::normalize(dir * std::cos(miss) + (side * std::cos(round) + up * std::sin(round)) * std::sin(miss));
        Bolt b{muzzle, dir * t.bolt_speed, t.bolt_damage, 5.0f, t.bolt_color, int(&t - targets_.data())};
        b.splash = t.bolt_splash, b.radius = t.bolt_radius;
        launch_bolt(b, t.bolt_looks);
        if (!t.ds_sound.empty()) sound(t.ds_sound, &muzzle);
        ++stats_.enemy_shots;
        return;
    }
    if ((t.ds_wait -= dt) <= 0 && target && std::abs(off) <= 30.0f) {
        play_state(t, "Attack", false);
        t.ds_fire_at = 0.321f / 0.5f;
    }
}

// CVehicleCarAI::Update: while it detects a target it selects one and fires its gun's weapon
// at it (the aim point half its height up), again after [0] ms (0 in the data, so it keeps
// firing; the weapon's Attack_AI clip paces the shots). CVehicleCar::UpdateTarget /
// UpdateWeaponLimits turn the gun to 0.85 of the target's height (100 m ahead without
// one), its pitch within 20 degrees of the chassis.
void Combat::update_car(Target& t, float dt) {
    if (!alive(t)) return;
    const LevelLogic::Object* o = logic_.object(t.id);
    size_t gun, shooter;
    if (!o || !logic_.vehicle_gun(t.id, gun, shooter)) return;
    t.pos = o->position - glm::vec3(0, 0, 0.9f), t.yaw = o->yaw;
    if (t.car_interval <= 0) {
        const std::string clip = logic_.state_clip(t.car_gun_type, "Attack_AI");
        const float len = clip.empty() ? -1 : scene_.clip_length(gun, clip);
        t.car_interval = len > 0.02f ? len / std::max(std::fabs(logic_.state_speed(t.car_gun_type, "Attack_AI")), 0.1f) : 0.134f;
    }
    if (!t.ranged) return;
    // What it fights, looked for a few times a second.
    if ((t.choose_timer -= dt) <= 0) t.choose_timer = 0.25f, t.enemy = choose_enemy(t);
    glm::vec3 feet, chest;
    const bool sees = !player_dead && t.enemy != kNoEnemy && enemy_points(t, t.enemy, feet, chest) && sees_point(t, chest);
    const glm::vec3 pivot(scene_.object_matrix(gun)[3]);
    const float y = glm::radians(t.yaw);
    const float tall = !sees ? 0 : t.enemy == -1 ? 2 * player_.radius.z : targets_[size_t(t.enemy)].height;
    const glm::vec3 look = sees ? feet + glm::vec3(0, 0, tall * 0.85f) : pivot + glm::vec3(-std::sin(y), std::cos(y), 0) * 100.0f;
    const glm::vec3 to = look - pivot;
    const float want_yaw = angle_diff(t.yaw, yaw_towards(pivot, look));
    const float want_pitch = std::clamp(glm::degrees(std::atan2(to.z, glm::length(glm::vec2(to)))), -20.0f, 20.0f);
    const float step = 120.0f * dt;
    t.car_yaw += std::clamp(angle_diff(t.car_yaw, want_yaw), -step, step);
    t.car_pitch += std::clamp(want_pitch - t.car_pitch, -step, step);
    logic_.aim_vehicle_gun(t.id, t.car_yaw, t.car_pitch);
    // Firing while it sees it (stopping half a second after it lost it), a shot per loop.
    t.car_spread = std::max(t.car_spread - t.car_spread_recover * dt, t.car_spread_min);
    if (sees) t.lost_sight = 0;
    else t.lost_sight += dt;
    if (sees && !t.car_firing) car_fire(t, true), t.car_shot = 0.016f;
    else if (!sees && t.car_firing && t.lost_sight > 0.5f) car_fire(t, false);
    if (t.car_muzzle_shown && scene_.clip_finished(t.muzzle_object)) scene_.set_object_visible(t.muzzle_object, false), t.car_muzzle_shown = false;
    if (!t.car_firing || (t.car_shot -= dt) > 0) return;
    t.car_shot += t.car_interval;
    glm::vec3 muzzle = pivot;
    const std::string node = t.muzzle_node.empty() ? std::string("muzzle") : t.muzzle_node;
    scene_.node_position(gun, node, muzzle);
    if (t.muzzle_type >= 0 && !t.muzzle_clips.empty()) {
        if (t.muzzle_object == size_t(-1)) t.muzzle_object = scene_.spawn_object(t.muzzle_type, -5000 - attachment_id_++, muzzle, 0);
        if (t.muzzle_object != size_t(-1)) {
            scene_.play_clip(t.muzzle_object, t.muzzle_clips[size_t(rng_()) % t.muzzle_clips.size()], false);
            scene_.set_object_transform(t.muzzle_object, scene_.object_matrix(gun, node));
            scene_.set_object_visible(t.muzzle_object, true);
            t.car_muzzle_shown = true;
        }
    }
    // At half its height (where it was last seen), turned by the weapon's spread.
    const glm::vec3 at = sees ? feet + glm::vec3(0, 0, tall * 0.5f) : look;
    glm::vec3 dir = glm::normalize(at - muzzle);
    std::uniform_real_distribution<float> u(-t.car_spread, t.car_spread);
    const glm::vec3 side = glm::normalize(glm::cross(dir, glm::vec3(0, 0, 1)) + glm::vec3(1e-6f, 0, 0)), up = glm::cross(side, dir);
    dir = glm::normalize(dir + side * std::tan(glm::radians(u(rng_))) + up * std::tan(glm::radians(u(rng_))));
    t.car_spread = std::min(t.car_spread + t.car_spread_fire, t.car_spread_max);
    Bolt b{muzzle, dir * t.bolt_speed, t.bolt_damage, 3.0f, glm::vec4(1, 0.8f, 0.4f, 1), int(&t - targets_.data())};
    b.splash = t.bolt_splash, b.radius = t.bolt_radius;
    launch_bolt(b, t.bolt_looks);
    ++stats_.enemy_shots;
}

void Combat::car_fire(Target& t, bool on) {
    t.car_firing = on;
    size_t gun, shooter;
    if (!logic_.vehicle_gun(t.id, gun, shooter)) return;
    const LevelLogic::Object* o = logic_.object(t.id);
    // CWeaponComponent::FireWeapon / FireStop: the gun's Attack_AI (or Idle) clip, the
    // firing loop and its end; CVehicleCar::StartShooterFire / StopShooterFire.
    const std::string clip = logic_.state_clip(t.car_gun_type, on ? "Attack_AI" : "Idle");
    if (!clip.empty()) scene_.play_clip(gun, clip, true);
    if (shooter != size_t(-1) && shooter < scene_.level().objects.size()) {
        const std::string pose = logic_.state_clip(scene_.level().objects[shooter].type, on ? "ShootAttack" : "ShootIdle");
        if (!pose.empty()) scene_.play_clip(shooter, pose, true);
    }
    if (t.car_loop >= 0 && logic_.audio) logic_.audio->stop(t.car_loop, 50);
    t.car_loop = -1;
    if (on && logic_.audio && !t.car_loop_sound.empty() && o) {
        Audio::Params p;
        p.position = &o->position, p.loop = 1;
        t.car_loop = logic_.audio->play(t.car_loop_sound, p);
    } else if (!on && !t.car_stop_sound.empty() && o) {
        sound(t.car_stop_sound, &o->position);
    }
}

void Combat::fire_bolt(Target& t, const glm::vec3& at) {
    glm::vec3 muzzle = t.pos + glm::vec3(0, 0, t.height * 0.7f);
    const float y = glm::radians(t.yaw);
    muzzle += glm::vec3(-std::sin(y), std::cos(y), 0) * (t.radius + 0.3f);
    // From its weapon's muzzle, which flashes (CNPCWeaponComponent::Fire).
    if (t.armed && !t.muzzle_node.empty() && scene_.node_position(t.attached[0].object, t.muzzle_node, muzzle) && t.muzzle_type >= 0 &&
        !t.muzzle_clips.empty()) {
        if (t.muzzle_object == size_t(-1)) t.muzzle_object = scene_.spawn_object(t.muzzle_type, -5000 - attachment_id_++, muzzle, 0);
        if (t.muzzle_object != size_t(-1)) {
            scene_.play_clip(t.muzzle_object, t.muzzle_clips[size_t(rng_()) % t.muzzle_clips.size()], false);
            scene_.set_object_transform(t.muzzle_object, scene_.object_matrix(t.attached[0].object, t.muzzle_node));
            scene_.set_object_visible(t.muzzle_object, true);
            t.muzzle_shown = true;
        }
    }
    // CNPCWeaponComponent::UpdateTargetPos / Fire: at the sweeping aim point, turned by the
    // spread (random yaw and pitch within it).
    const float a = glm::radians(t.sweep_a);
    const glm::vec3 point = at + (t.locked_on ? glm::vec3(0) : glm::vec3(-std::sin(a), std::cos(a), 0) * t.sweep_r);
    glm::vec3 aim = glm::normalize(point - muzzle);
    t.spread = std::min(t.spread + t.spread_step, (1 - accuracy(t.id)) * t.spread_max);
    if (t.spread > 0) {
        std::uniform_real_distribution<float> u(-t.spread, t.spread);
        const glm::vec3 side = glm::normalize(glm::cross(aim, glm::vec3(0, 0, 1)) + glm::vec3(1e-6f, 0, 0));
        const glm::vec3 up = glm::cross(side, aim);
        aim = glm::normalize(aim + side * std::tan(glm::radians(u(rng_))) + up * std::tan(glm::radians(u(rng_))));
    }
    Bolt b{muzzle, aim * t.bolt_speed, t.bolt_damage, 3.0f, t.bolt_color, int(&t - targets_.data())};
    b.splash = t.bolt_splash, b.radius = t.bolt_radius;
    launch_bolt(b, t.bolt_looks);
    if (t.shots_left == t.burst) sound(t.fire_sounds, &muzzle);   // the sounds are whole bursts
    if (t.mag_size > 0) t.mag = std::max(t.mag - t.per_shot, 0);
    ++stats_.enemy_shots;
}

void Combat::damage_player(float amount, const glm::vec3& from) {
    if (player_dead || player_invulnerable || protect_ > 0 || amount <= 0) return;
    if (logic_.in_mech()) amount *= 0.3f;   // the mech's armour (its health is 200 to the player's 50)
    player_health -= amount;
    stats_.damage_taken += amount;
    regen_delay_ = regen_pause_;
    // The angle from the view's heading to the hit's source (clockwise, 0..360; 0 without
    // one). The hit indicator points there (CLevel::StartInterfaceEffect: controls.bsprite
    // frame 60 turned by it, for a second) and the view jolts away from it.
    const glm::vec2 to(from.x - player_.center.x, from.y - player_.center.y);
    float angle = 0;
    if (glm::length(to) > 0.01f) {
        const float y = glm::radians(player_.yaw);
        const glm::vec2 ahead(-std::sin(y), std::cos(y)), dir = glm::normalize(to);
        angle = glm::degrees(std::acos(std::clamp(glm::dot(ahead, dir), -1.0f, 1.0f)));
        if (ahead.y * dir.x - dir.y * ahead.x < 0) angle = 360 - angle;
    }
    // The player's own reactions (CPlayerBaseComponent::ReceiveDamage: the indicator, the HUD
    // effects, the view's jolt, the hurt sound) only while it is the active player component,
    // not under limited control.
    if (!logic_.limited_active()) {
        hit_angle_ = angle, hit_left_ = 1.0f;
        // Over half the shield left (100 - PlayerBase [0] percent): its ripple; else blood.
        const float half = (100.0f - base_shield_percent_) * 0.01f * player_max_health;
        if (logic_.in_mech() || logic_.player_vehicle() >= 0) {   // not in the mech or the jeep
        } else if (player_health > half) {
            if (hud_damage_wait_ <= 0) hud_damage_ = true, hud_damage_wait_ = 3.0f;
        } else {
            hud_blood_ = player_health / std::max(player_max_health, 1.0f);
        }
        if (view_hurt_wait_ <= 0 && glm::length(to) > 0.01f) {
            view_hurt_wait_ = view_hurt_pause_;
            const char* side = angle >= 325 || angle < 35 ? "front" : angle < 135 ? "right" : angle < 225 ? "back" : "left";
            view_hurt_ = std::string("Hurt_") + side + (amount > view_hurt_heavy_ ? "_heavy" : "");
        }
        if (player_hurt_wait_ <= 0) {
            sound(player_hurt_sounds_, nullptr);
            player_hurt_wait_ = 1.5f;
        }
    }
    // Each fall below 30% of the shield counts against the end of level's shield bonus
    // (CPlayerBaseComponent::ReceiveDamage, CLevelStatistics +0x18).
    const bool critical = player_health < player_max_health * 0.3f;
    if (critical && !critical_ && player_health > 0) ++stats_.criticals;
    critical_ = critical;
    if (player_health <= 0) {
        player_health = 0;
        player_dead = true;
        dead_timer_ = 0;
        ++stats_.deaths;
        log("player died");
    }
}

void Combat::apply_damage(Target& t, float amount, bool by_player) {
    LevelLogic::Object* o = logic_.object(t.id);
    if (!o || o->dead || t.invulnerable || !o->enabled) return;
    if (t.actor && !t.aggr_locked) t.aggr = std::max(t.aggr - amount / t.max_health * t.af_hurt, 0.0f);   // AF_DamageTaken
    o->health -= amount;
    t.last_damage = amount;
    if (by_player) logic_.runtime_.send_event("TargetHit", t.id);
    if (t.actor) notice(t);
    if (o->health <= 0) {
        die(t);
        return;
    }
    if (!t.cloak_model.empty()) t.recent_damage.emplace_back(clock_, amount);
    {   // the hurt look spreading from where it was hit (its chest when that isn't known)
        const glm::vec3 chest = t.pos + glm::vec3(0, 0, t.height * 0.6f);
        hurt_look(t, damage_point_ ? damage_point_ : &chest);
    }
    if (t.hurt_sound_wait <= 0 && !t.hurt_sounds.empty()) {
        const glm::vec3 at = t.pos + glm::vec3(0, 0, t.height * 0.6f);
        sound(t.hurt_sounds, &at);
        t.hurt_sound_wait = 1.2f;
    }
    // Flinch now and then: ducking from a blast, else by where the shot struck (the head,
    // a leg by its side, the front).
    if (t.actor && !t.clip_hurt.empty() && t.hurt_cooldown <= 0 && t.state != Target::State::Melee &&
        std::uniform_real_distribution<float>(0, 1)(rng_) < 0.35f) {
        std::string clip;
        if (damage_cause_ == Cause::Explosion) {
            clip = state_variant(t, "DuckExplosion" + std::to_string(std::uniform_int_distribution<int>(1, 4)(rng_)));
        } else if (damage_point_) {
            const float up = (damage_point_->z - t.pos.z) / std::max(t.height, 0.1f);
            const float y = glm::radians(t.yaw);
            const float side = glm::dot(glm::vec2(*damage_point_ - t.pos), glm::vec2(std::cos(y), std::sin(y)));   // + its right
            if (t.headshots && up > 0.84f) clip = state_variant(t, "HurtHead");
            else if (up < 0.45f) clip = state_variant(t, side < 0 ? "HurtLeftLeg" : "HurtRightLeg");
        }
        if (clip.empty()) clip = state_variant(t, "HurtFront");
        if (clip.empty()) clip = t.clip_hurt;
        t.state = Target::State::Hurt;
        t.timer = 0;
        t.hurt_cooldown = 2.5f;
        play(t, clip, false);
    }
}

void Combat::sound(const std::vector<std::string>& names, const glm::vec3* at) {
    if (!logic_.audio || names.empty()) return;
    Audio::Params p;
    p.position = at;
    logic_.audio->play_one_of(names, p);
}

void Combat::impact(const Impacts& im, uint8_t material, const glm::vec3& at, const glm::vec3& normal, bool world) {
    // CProjectileComponent::Die on the world: its decal for the material, laid along the hit.
    int cell = -1;
    if (float size = 0; world && im.decals.find(material, cell, size)) scene_.decals().add(scene_.collision(), at, -normal, cell, size);
    auto fx = im.effects.find(material);
    if (fx == im.effects.end()) fx = im.effects.find(0);
    if (fx == im.effects.end() || !effect(fx->second, at + normal * 0.02f, normal)) sparks_.push_back({at, 0.1f, {1.0f, 0.8f, 0.4f, 1}});
    auto sn = im.sounds.find(material);
    if (sn == im.sounds.end()) sn = im.sounds.find(0);
    if (sn != im.sounds.end()) sound(sn->second, &at);
}

bool Combat::effect(const std::string& name, const glm::vec3& at, const glm::vec3& normal) {
    if (name.empty()) return false;
    const glm::vec3 z = glm::length(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0, 0, 1);
    glm::vec3 x = glm::cross(std::abs(z.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0), z);
    x = glm::normalize(x);
    const glm::mat4 m(glm::vec4(x, 0), glm::vec4(glm::cross(z, x), 0), glm::vec4(z, 0), glm::vec4(at, 1));
    return scene_.particles().start(name, m) >= 0;
}

std::string Combat::state_variant(const Target& t, const std::string& state) {
    const LevelLogic::Object* o = logic_.object(t.id);
    if (!o || o->index >= scene_.level().objects.size()) return {};
    std::vector<std::string> clips;
    for (const std::string& c : logic_.state_clips(scene_.level().objects[o->index].type, state))
        if (scene_.clip_length(t.object, c) > 0) clips.push_back(c);
    return clips.empty() ? std::string() : clips[std::uniform_int_distribution<size_t>(0, clips.size() - 1)(rng_)];
}

// CPhysicsRagdollComponent::OnEvent (death): the death the killing blow brings
// (CRagdoll::GetDeathType); unless it is an animated one, the body falls as a ragdoll,
// pushed by the blow (BulletHit / ExplosionHit / RepelHit).
bool Combat::start_ragdoll(Target& t) {
    if (!t.has_ragdoll) return false;
    const int type = killing_repel_ ? 13 : damage_cause_ == Cause::Explosion ? 8 : damage_cause_ == Cause::Fire ? 0
                                                                                   : ragdolls_.death_type(damage_source_);
    if (type == 0) return false;
    auto r = std::make_shared<Ragdoll>();
    if (!r->start(scene_, t.object, ragdolls_.shapes.at(t.ragdoll_kind))) return false;
    scene_.set_clip_speed(t.object, 0);   // the bones it does not move hold as they were
    // The original's pushes are Bullet impulses; here velocity changes of a like feel.
    const float shove = std::min(t.last_damage * t.ragdoll_hit, t.ragdoll_hit_max);
    const glm::vec3 chest = t.pos + glm::vec3(0, 0, t.height * 0.6f);
    if (type == 8) {   // thrown from the blast, and up
        const glm::vec3 away = glm::normalize(chest - blast_at_ + glm::vec3(0, 0, 0.5f));
        r->push(glm::normalize(away + glm::vec3(0, 0, 0.6f)) * std::min(t.ragdoll_blast * 0.2f, 8.0f));
    } else if (type == 13) {   // repelled
        r->push(glm::normalize(damage_dir_ + glm::vec3(0, 0, 0.3f)) * 7.0f);
    } else if (type != 1 && glm::dot(damage_dir_, damage_dir_) > 0) {   // the bullet's push at where it struck
        const glm::vec3 at = damage_point_ ? *damage_point_ : chest;
        r->hit(at, damage_dir_ * std::min(shove * 0.05f, 6.0f));
        r->push(damage_dir_ * std::min(shove * 0.01f, 1.5f));
    }
    t.ragdoll = r;
    if (logic_.trace) log(std::to_string(t.id) + " falls as a ragdoll (death type " + std::to_string(type) + ")");
    return true;
}

void Combat::die(Target& t) {
    if (t.gun_loop >= 0 && logic_.audio) logic_.audio->stop(t.gun_loop, 50);   // its gun falls silent
    t.gun_loop = -1, t.gun = -1;
    LevelLogic::Object* o = logic_.object(t.id);
    if (!o || o->dead) return;
    leave_cover(t);
    if (t.held >= 0) drop_held(t);   // CNPCComponent::OnEvent 0xe: RemoveTentacle, DropTrownObject
    o->dead = true;
    o->health = 0;
    t.timer = 0;
    t.token = false;
    ++stats_.kills;
    if (t.hostile) {
        if (killing_headshot_) ++stats_.headshot_kills;
        if (killing_repel_) ++stats_.repel_kills;
        else if (t.paralyzed > 0) ++stats_.paralyze_kills;
        else if (logic_.time_scale() < 1) ++stats_.slow_kills;
    }
    log("killed " + t.type + " #" + std::to_string(t.id));
    {
        const glm::vec3 at = t.pos + glm::vec3(0, 0, t.height * 0.6f);
        sound(t.death_sounds, &at);
    }
    // The death by its cause (the states set's DeathHeadShot, DeathExplosion, DeathFlames,
    // DeathWeak), else any death clip.
    std::string death = killing_headshot_ ? state_variant(t, "DeathHeadShot")
                      : damage_cause_ == Cause::Explosion ? state_variant(t, "DeathExplosion")
                      : damage_cause_ == Cause::Fire ? state_variant(t, "DeathFlames") : std::string();
    if (death.empty()) death = state_variant(t, "DeathWeak");
    if (death.empty() && !t.clip_deaths.empty()) death = t.clip_deaths[std::uniform_int_distribution<size_t>(0, t.clip_deaths.size() - 1)(rng_)];
    if (t.cloak != Target::Cloak::Visible) {   // dying shows it
        t.cloak = Target::Cloak::Visible, t.script_cloak = false;
        scene_.set_object_material(t.object, "", "");
    }
    if (t.actor && start_ragdoll(t)) {
        t.state = Target::State::Dying;
    } else if (t.actor && !death.empty()) {
        t.state = Target::State::Dying;
        play(t, death, false);
    } else if (t.car) {
        // CVehicleCar::Destroy: it stops and plays Death (the wreck, its explosion); the
        // wreck stays.
        if (t.car_firing) car_fire(t, false);
        if (t.car_muzzle_shown) scene_.set_object_visible(t.muzzle_object, false), t.car_muzzle_shown = false;
        logic_.destroy_vehicle(t.id);
        t.state = Target::State::Dead;
        play_state(t, "Death", false);
    } else {
        t.state = Target::State::Dead;
        if (!t.actor) logic_.show(*o, false);
    }
    const glm::vec3 death_at = glm::vec3(scene_.object_matrix(t.object, t.death_node)[3]);
    effect(t.death_effect, t.death_node.empty() ? t.pos + glm::vec3(0, 0, t.height * 0.5f) : death_at);
    if (t.explosive) {
        // Plasma pods: splash damage around them.
        const glm::vec3 c = t.pos + glm::vec3(0, 0, 0.5f);
        if (t.death_effect.empty() || !scene_.particles().has(t.death_effect))
            for (int k = 0; k < 24; ++k)
                sparks_.push_back({c + glm::vec3(std::cos(k * 0.8f), std::sin(k * 0.8f), (k % 5) * 0.3f) * (0.4f + 0.1f * (k % 4)),
                                   0.6f, {0.4f, 0.8f, 1.0f, 1}});
        for (Target& other : targets_) {
            if (&other == &t || !alive(other)) continue;
            const float d = glm::distance(other.pos, t.pos);
            if (d < 6.0f) damage_cause_ = Cause::Explosion, apply_damage(other, 40.0f * (1 - d / 6.0f), true), damage_cause_ = Cause::Shot;
        }
        const float d = glm::distance(player_center(), c);
        if (d < 6.0f) damage_player(40.0f * (1 - d / 6.0f), c);
    }
    logic_.runtime_.send_event("TargetableDied", t.id);
    logic_.runtime_.send_event("WaitForEnemiesDead", t.id);
    logic_.start_script("Obj" + std::to_string(t.id) + "_OnDead", t.id);
}

int Combat::ray_target(const glm::vec3& from, const glm::vec3& dir, float max_t, float& t_hit, glm::vec3& point) const {
    int best = -1;
    t_hit = max_t;
    for (size_t i = 0; i < targets_.size(); ++i) {
        const Target& t = targets_[i];
        if (!alive(t)) continue;
        // Vertical cylinder: solve in XY, then check the height.
        const glm::vec2 o = glm::vec2(from - t.pos), d = glm::vec2(dir);
        const float a = glm::dot(d, d), b = 2 * glm::dot(o, d), c = glm::dot(o, o) - t.radius * t.radius;
        if (a < 1e-8f) continue;
        const float disc = b * b - 4 * a * c;
        if (disc < 0) continue;
        const float s = std::sqrt(disc);
        for (float h : {(-b - s) / (2 * a), (-b + s) / (2 * a)}) {
            if (h < 0 || h >= t_hit) continue;
            const glm::vec3 p = from + dir * h;
            if (p.z < t.pos.z || p.z > t.pos.z + t.height) continue;
            t_hit = h, best = int(i), point = p;
            break;
        }
    }
    return best;
}

void Combat::fire_player(const glm::vec3& eye, const glm::vec3& aim) {
    const WeaponDef& w = weapon();
    since_shot_ = 0, ++shots_;
    --clip_[size_t(current_)];
    // Aim assist (the original's magnetic cursor): the enemy nearest the crosshair, within
    // three degrees and in sight, takes the shot.
    glm::vec3 aimed = aim;
    if (aim_assist) {
        float best = std::cos(glm::radians(3.0f));
        for (const Target& t : targets_) {
            if (!t.actor || !t.hostile || !alive(t)) continue;
            const glm::vec3 to = t.pos + glm::vec3(0, 0, t.height * 0.6f) - eye;
            const float d = glm::length(to);
            if (d < 1e-3f || d > 150.0f) continue;
            const float c = glm::dot(to / d, aim);
            if (c > best && line_clear(eye, eye + to * 0.98f)) best = c, aimed = to / d;
        }
    }
    const glm::vec3& aim_dir = aimed;
    ++stats_.shots;
    // CWeaponComponent::DoFire: the spread within its least and most; GetSpread: the shot
    // off by up to that much either way, across and up.
    const WeaponDef::Spread& sp = aiming_ ? w.aim : w.hip;
    spread_ = std::clamp(spread_, sp.min, std::max(sp.max, sp.min));
    std::uniform_real_distribution<float> n(-glm::radians(spread_), glm::radians(spread_));
    glm::vec3 right = glm::cross(aim_dir, glm::vec3(0, 0, 1));
    right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3(1, 0, 0);
    const glm::vec3 up = glm::normalize(glm::cross(right, aim_dir));
    const glm::vec3 dir = glm::normalize(aim_dir + right * std::tan(n(rng_)) + up * std::tan(n(rng_)));
    // CWeaponManager::UpdateWeaponFire: each shot opens it by its rate over the frame (the
    // original's 30 frames a second), up to the most.
    spread_ = std::min(spread_ + sp.fire / 30.0f, sp.max);

    // As far as the bullet flies in its life (the flamethrower's flame: 15 m/s for 1.3 s).
    const float range = w.bullet_speed >= 1 ? std::min(300.0f, w.bullet_speed * w.bullet_life) : 300.0f;   // under 1: the rail gun's instant beam
    float wall_t = 1;
    uint8_t material = 0;
    glm::vec3 normal(0, 0, 1);
    uint32_t wall_owner = 0;
    const bool wall = scene_.collision().raycast(eye, eye + dir * range, wall_t, &material, &normal, &wall_owner, true);
    float target_t = 0;
    glm::vec3 point;
    const int hit = ray_target(eye, dir, wall ? wall_t * range : range, target_t, point);
    const glm::vec3 end = hit >= 0 ? point : eye + dir * (wall ? wall_t * range : range);
    const glm::vec3 from = has_muzzle_ ? muzzle_ : eye + aim_dir * 0.6f + right * 0.18f - up * 0.16f;
    if (w.grenade >= 0) {   // a grenade out of the muzzle at what the crosshair is on
        auto [it, fresh] = fired_grenades_.try_emplace(w.grenade);
        if (fresh) it->second = read_grenade(gol_, w.grenade);
        launch_grenade(it->second, from, glm::normalize(end - from) * it->second.speed, false);
    } else if (w.flies) {   // a projectile from the muzzle at what the crosshair is on; it hits as it gets there
        Bolt b{from, glm::normalize(end - from) * w.bullet_speed, w.damage_near, w.bullet_life, glm::vec4(1, 0.6f, 0.3f, 1), -1};
        b.splash = w.splash_damage, b.radius = w.splash_radius, b.weapon = current_;
        launch_bolt(b, w.looks);
    } else {
        launch_trail(w, from, end);
        if (!w.beam_texture.empty()) beams_.push_back({from, end, w.beam_width, 0, w.beam_texture});
        if (hit >= 0) {
            Target& t = targets_[size_t(hit)];
            const float d = glm::distance(eye, point);
            float dmg = w.damage_near;
            if (d > w.far_dist) dmg = w.damage_far;
            else if (d > w.near_dist) dmg = w.damage_near + (d - w.near_dist) * (w.damage_far - w.damage_near) / (w.far_dist - w.near_dist);
            const bool head = t.headshots && point.z > t.pos.z + t.height * 0.84f;
            if (head) dmg *= 2;   // CTargetableComponent head multiplier
            ++stats_.hits;
            hit_marker_ = w.type == kLightningGunType ? 0.15f : 0.075f;
            if (!effect(t.hit_effect, point, -dir)) sparks_.push_back({point, 0.12f, {1.0f, 0.3f, 0.2f, 1}});
            killing_headshot_ = head;
            damage_point_ = &point;
            damage_source_ = w.type, damage_dir_ = dir;
            if (weapon_types_[size_t(current_)] == "FlameThrower_Player") damage_cause_ = Cause::Fire;
            apply_damage(t, dmg, true);
            killing_headshot_ = false, damage_point_ = nullptr, damage_cause_ = Cause::Shot, damage_source_ = -1;
        } else if (wall) {
            impact(w.impacts, material, end, normal, true);
        }
        shoot_breakables(eye, end, w.damage_near, hit < 0 && wall, wall_owner);
    }
    // Shot sounds: automatic weapons loop while the trigger is held.
    if (!w.loop_sound.empty()) {
        if (!logic_.audio || !logic_.audio->playing(fire_loop_)) {
            Audio::Params p;
            p.loop = 1;
            fire_loop_ = logic_.audio ? logic_.audio->play(w.loop_sound, p) : -1;
        }
    } else {
        sound(w.shot_sounds, nullptr);
    }
}

void Combat::update(float dt, const CombatInput& in, const glm::vec3& eye, const glm::vec3& aim) {
    last_eye_ = eye;
    last_aim_ = aim;
    update_spawners(dt);
    update_pickups(dt);
    update_damage_zones(dt);
    burst_cooldown_ = std::max(burst_cooldown_ - dt, 0.0f);
    player_hurt_wait_ = std::max(player_hurt_wait_ - dt, 0.0f);
    view_hurt_wait_ = std::max(view_hurt_wait_ - dt, 0.0f);
    hit_left_ = std::max(hit_left_ - dt, 0.0f);
    since_shot_ += dt;
    for (Target& t : targets_) t.hurt_sound_wait = std::max(t.hurt_sound_wait - dt, 0.0f);
    logic_.actor_positions.clear();
    for (const Target& t : targets_)
        if (t.actor && t.mobile && t.aware && alive(t)) logic_.actor_positions.push_back(t.pos + glm::vec3(0, 0, 1));
    // Spawned bodies go back to the pool after a while.
    for (Target& t : targets_) {
        if (!t.spawned || t.state != Target::State::Dead) continue;
        if ((t.timer += dt) > 8.0f)
            if (LevelLogic::Object* o = logic_.object(t.id); o && o->visible) logic_.show(*o, false);
    }
    if (grenade_ticket_ >= 0) grenade_ticket_ -= dt;   // CAIController::Update
    keep_enemies_on_player();
    for (Target& t : targets_) {
        LevelLogic::Object* o = logic_.object(t.id);
        if (!o || !o->enabled) continue;
        if (t.state != Target::State::Dying && t.state != Target::State::Dead && (o->dead || o->health <= 0)) {
            o->dead = false;
            die(t);   // SetHealth(id, 0) from a script
            continue;
        }
        if (t.actor) update_actor(t, dt), update_cloak(t, dt);
        else if (t.dropship) update_dropship(t, dt);
        else if (t.car) update_car(t, dt);
        else if (t.flight) update_flight(t, dt);
        else if (t.paralyzed > 0) update_paralysis(t, dt);
    }
    // Keep actors apart.
    for (size_t i = 0; i < targets_.size(); ++i) {
        Target& a = targets_[i];
        if (!a.actor || !a.mobile || !alive(a)) continue;
        for (size_t j = i + 1; j < targets_.size(); ++j) {
            Target& b = targets_[j];
            if (!b.actor || !b.mobile || !alive(b)) continue;
            glm::vec2 d = glm::vec2(b.pos - a.pos);
            const float len = glm::length(d), min = a.radius + b.radius;
            if (len >= min || std::abs(a.pos.z - b.pos.z) > 2) continue;
            d = len > 1e-4f ? d / len : glm::vec2(1, 0);
            const glm::vec2 push = d * (min - len) * 0.5f;
            a.pos -= glm::vec3(push, 0), b.pos += glm::vec3(push, 0);
            for (Target* x : {&a, &b}) {
                const bool crossing = x->path_next < x->path_off_mesh.size() && x->path_off_mesh[x->path_next];
                if (x->tri >= 0 && !crossing) stay_on_mesh(*x);
            }
        }
    }

    // Projectiles: move, hit the player or the world.
    for (Bolt& b : bolts_) {
        const glm::vec3 next = b.pos + b.vel * dt;
        b.life -= dt;
        float t = 1;
        uint8_t material = 0;
        glm::vec3 normal(0, 0, 1);
        uint32_t wall_owner = 0;
        const bool wall = scene_.collision().raycast(b.pos, next, t, &material, &normal, &wall_owner, true);
        const glm::vec3 end = wall ? b.pos + (next - b.pos) * t : next;
        // Closest approach to the player along this step.
        const glm::vec3 pc = player_center();
        const glm::vec3 seg = end - b.pos;
        const float s = glm::dot(seg, seg) > 0 ? glm::clamp(glm::dot(pc - b.pos, seg) / glm::dot(seg, seg), 0.0f, 1.0f) : 0.0f;
        const glm::vec3 c = b.pos + seg * s;
        const glm::vec3 off = c - pc;
        // Other targets the shooter fights (not its friends) in the way.
        float hit_t = 0;
        glm::vec3 hit_p;
        const float seg_len = glm::length(seg);
        const int hit = seg_len > 1e-4f ? ray_target(b.pos, seg / seg_len, seg_len, hit_t, hit_p) : -1;
        const bool players = b.owner < 0;   // the player's projectile: anything it meets, never the player
        const Target* shooter = b.owner >= 0 && size_t(b.owner) < targets_.size() ? &targets_[size_t(b.owner)] : nullptr;
        const bool hits_target = hit >= 0 && hit != b.owner &&
                                 (players || (shooter && ((shooter->attacks & (1u << targets_[size_t(hit)].team)) != 0 ||
                                                          targets_[size_t(hit)].id == shooter->locked)));
        const Impacts* impacts = players && b.weapon >= 0 && size_t(b.weapon) < weapons_.size() ? &weapons_[size_t(b.weapon)].impacts
                                 : shooter && !shooter->bolt_impacts.effects.empty()       ? &shooter->bolt_impacts
                                                                                            : nullptr;
        glm::vec3 stop = end;
        bool stopped = false;
        if (!player_dead && shooter && (shooter->attacks & kPlayerTeamBit) && glm::length(glm::vec2(off)) < player_.radius.x + 0.15f &&
            std::abs(off.z) < player_.radius.z) {
            damage_player(b.damage, b.pos);
            stop = c, stopped = true;
        } else if (hits_target) {
            Target& t = targets_[size_t(hit)];
            if (players) ++stats_.hits, hit_marker_ = b.weapon >= 0 && size_t(b.weapon) < weapons_.size() && weapons_[size_t(b.weapon)].type == kLightningGunType ? 0.15f : 0.075f;
            damage_source_ = players && b.weapon >= 0 && size_t(b.weapon) < weapons_.size() ? weapons_[size_t(b.weapon)].type : -1;
            damage_dir_ = seg / std::max(seg_len, 1e-4f), damage_point_ = &hit_p;
            if (b.damage > 0) apply_damage(t, b.damage, players);
            damage_source_ = -1, damage_point_ = nullptr;
            if (!t.hit_effect.empty()) effect(t.hit_effect, hit_p, -seg);
            else if (impacts) impact(*impacts, 0, hit_p, -seg / std::max(seg_len, 1e-4f));
            stop = hit_p, stopped = true;
        } else if (wall) {
            if (impacts) impact(*impacts, material, end, normal, true);
            else if (!effect("impact_plasma", end, normal)) sparks_.push_back({end, 0.15f, b.color});
            stopped = true;
        }
        if (b.damage > 0) shoot_breakables(b.pos, stop, b.damage, wall && !hits_target && stop == end, wall_owner);
        // An explosive one blows up where it stops, or at the end of its flight
        // (CProjectileComponent::Die: [6] damage within [7] metres).
        if ((stopped || b.life <= 0) && b.splash > 0 && b.radius > 0) splash_damage(stop, b.splash, b.radius, players);
        if (stopped) b.life = 0;
        b.pos = stop;
        const glm::vec3 dir = glm::normalize(b.vel);
        if (!b.fx.empty() || b.body != size_t(-1)) {
            const glm::mat4 at = bolt_matrix(b.pos, dir);
            for (const int h : b.fx) b.life > 0 ? scene_.particles().move(h, at) : scene_.particles().stop(h);
            if (b.body != size_t(-1)) {
                if (b.life > 0) {
                    scene_.set_object_transform(b.body, at);
                } else {
                    scene_.set_object_visible(b.body, false);
                    trail_pool_[b.body_type].push_back(b.body);
                }
            }
        }
        if (b.trail == size_t(-1)) continue;
        if (b.life > 0) {
            pose_trail(b.trail, b.from, b.pos, dir);
        } else {   // it stops here
            if (Tracer t{b.trail, b.trail_type, b.from, b.pos, dir, b.pos, 0}; end_trail(t)) tracers_.push_back(t);
            b.trail = size_t(-1);
        }
    }
    bolts_.erase(std::remove_if(bolts_.begin(), bolts_.end(), [](const Bolt& b) { return b.life <= 0; }), bolts_.end());
    update_trails(dt);
    update_hurt_looks(dt);
    clock_ += dt;
    for (Beam& b : beams_) b.age += dt;
    std::erase_if(beams_, [](const Beam& b) { return b.age >= 1.0f; });
    for (Spark& s : sparks_) s.life -= dt;
    sparks_.erase(std::remove_if(sparks_.begin(), sparks_.end(), [](const Spark& s) { return s.life <= 0; }), sparks_.end());

    // The player: death and revival, regeneration.
    hit_marker_ = std::max(hit_marker_ - dt, 0.0f);
    stats_.play_time += dt;
    if (player_dead) {
        // The screen fades, then the player is back at the last checkpoint (or the start).
        const float before = dead_timer_;
        dead_timer_ += dt;
        if (death_screen) return;   // its Retry calls respawn()
        if (before < 1.5f && dead_timer_ >= 1.5f) logic_.start_fade(false, 1200, glm::vec3(0));
        if (dead_timer_ > 3.0f) respawn();
        return;
    }
    protect_ = std::max(protect_ - dt, 0.0f);
    // Falling: a hard landing hurts (Player_EnableFallDamage).
    if (player_.landing_speed > 0) {
        const float v = std::exchange(player_.landing_speed, 0.0f);
        if (logic_.fall_damage && v > fall_damage_from_) damage_player((v - fall_damage_from_) * fall_damage_per_, player_.center);
        // The weapon's landing clip (PlayerComponent::Update -> CWeaponManager::StartLanding).
        if (v > landing_speeds_[0]) landing_ = v >= landing_speeds_[2] ? 2 : v >= landing_speeds_[1] ? 1 : 0;
    }
    if (core >= 0 && player_health <= player_max_health) core = -1;   // the core's charge is spent
    regen_delay_ -= dt;
    hud_damage_wait_ = std::max(hud_damage_wait_ - dt, 0.0f);
    if (regen_delay_ < 0 && player_health < player_max_health) blood_fade_ = true;   // RegenerateHealth
    if (regen_delay_ <= 0 && player_health < player_max_health)
        player_health = std::min(player_health + regen_rate_ * dt, player_max_health);

    // Grenades: the throw animation starts, the grenade leaves the hand a moment later.
    grenade_cooldown_ = std::max(grenade_cooldown_ - dt, 0.0f);
    if (in.switch_throwable && mines_enabled) throw_mines_ = !throw_mines_, ++switches_;
    if (!mines_enabled) throw_mines_ = false;
    if (in.detonate) detonate();
    int& ammo = throw_mines_ ? mines : grenades;
    if (in.grenade && (throw_mines_ || grenades_enabled) && ammo > 0 && grenade_cooldown_ <= 0 && throw_delay_ < 0 &&
        (throw_mines_ ? mine_def_ : grenade_def_).type >= 0) {
        --ammo;
        throwing_mine_ = throw_mines_, ++throws_;
        throw_delay_ = 0.35f;
        grenade_cooldown_ = 1.0f;
        throw_started_ = true;
    }
    if (throw_delay_ >= 0 && (throw_delay_ -= dt) < 0) throw_grenade(eye, aim);
    power_cooldown_ = std::max(power_cooldown_ - dt, 0.0f);
    if (in.power && power_cooldown_ <= 0) use_selected_power(eye, aim);
    update_grenades(dt);

    // Weapon.
    if (in.select >= 0 && in.select < int(weapons_.size()) && in.select != current_) {
        current_ = in.select;
        reload_left_ = 0;
        fire_cooldown_ = 0.4f;
    }
    fire_cooldown_ = std::max(fire_cooldown_ - dt, 0.0f);
    {   // CWeaponManager::UpdateWeaponFire, off the trigger: walking (not just after a shot)
        // it opens to half its most, else it closes to its least.
        aiming_ = in.aiming;
        const WeaponDef::Spread& sp = aiming_ ? weapon().aim : weapon().hip;
        const glm::vec3 now = player_.center;
        const bool walking = dt > 0 && glm::length(glm::vec2(now - last_center_)) / dt > 0.5f && since_shot_ >= 0.3f;
        last_center_ = now;
        if (!(trigger_held_ && firing())) {
            if (walking && spread_ < sp.max * 0.5f) spread_ = std::min(spread_ + dt * sp.walk, sp.max * 0.5f);
            else if (!walking) spread_ = std::max(spread_ - dt * sp.recover, sp.min);
        }
        // DrawTargetCrossHair: it fades in over a quarter second while it may show (not
        // aiming, unless the weapon keeps it; not reloading), out otherwise; the fire kick
        // (Update: 0.015 a ms) draws its arms in by up to 3 pixels.
        const bool show = (!aiming_ || weapon().cross_aiming) && reload_left_ <= 0;
        cross_alpha_ = std::clamp(cross_alpha_ + (show ? dt : -dt) * 1000.0f / 255.0f, 0.0f, 1.0f);
        cross_kick_ = std::clamp(cross_kick_ + (trigger_held_ && firing() ? dt : -dt) * 15.0f, 0.0f, 1.0f);
    }
    int& clip = clip_[size_t(current_)];
    int& reserve = reserve_[size_t(current_)];
    const WeaponDef& w = weapon();
    if (reload_left_ > 0) {
        reload_left_ -= dt;
        if (reload_left_ <= 0) {
            const int want = w.clip - clip;
            const int take = reserve < 0 ? want : std::min(want, reserve);
            clip += take;
            if (reserve >= 0) reserve -= take;
        }
    } else if (auto_switch && clip <= 0 && reserve == 0 && !special_weapon()) {
        // Out of ammunition: the next weapon that has some (Auto-Switch).
        for (int k = 1; k < int(weapons_.size()); ++k) {
            const int i = (current_ + k) % int(weapons_.size());
            if (owned_[size_t(i)] && (clip_[size_t(i)] > 0 || reserve_[size_t(i)] != 0)) {
                current_ = i, fire_cooldown_ = 0.4f;
                break;
            }
        }
    } else if ((in.reload && clip < w.clip) || (clip <= 0 && in.fire)) {
        if (reserve != 0) {
            reload_left_ = w.reload_time;
            sound(w.reload_sound, nullptr);
        }
    } else if (in.fire && shoot_enabled && fire_cooldown_ <= 0 && clip > 0 && (w.automatic || !trigger_held_)) {
        fire_player(eye, aim);
        fire_cooldown_ = w.interval;
    }
    // The fire loop ends when the weapon stops firing (a short gap keeps it going).
    if (fire_loop_ >= 0 && logic_.audio && (fire_cooldown_ <= 0 || reload_left_ > 0) && !(in.fire && clip > 0 && shoot_enabled)) {
        logic_.audio->stop(fire_loop_, 30);
        fire_loop_ = -1;
        sound(w.end_sound, nullptr);
    }
    trigger_held_ = in.fire;
    float t_hit;
    glm::vec3 p;
    float wall_t = 1;
    const bool wall = scene_.collision().raycast(eye, eye + aim * 200.0f, wall_t);
    const int under = ray_target(eye, aim, wall ? wall_t * 200.0f : 200.0f, t_hit, p);
    aim_on_enemy_ = under >= 0 && targets_[size_t(under)].hostile;
    aim_distance_ = under >= 0 ? t_hit : wall ? wall_t * 200.0f : -1.0f;
}

Combat::Crosshair Combat::crosshair() const {
    const WeaponDef& w = weapon();
    Crosshair c;
    c.sprite = w.sprite, c.plus = w.cross_plus, c.scope = w.scope;
    const WeaponDef::Spread& sp = aiming_ ? w.aim : w.hip;
    c.arms = -3.0f * cross_kick_ + float(w.cross_px) * (sp.max > 0 ? spread_ / sp.max : 0.0f);
    c.color = aim_on_enemy_ ? glm::vec4(1, 0, 0, 1) : glm::vec4(1);
    c.alpha = w.scope ? 1.0f : cross_alpha_;
    c.distance = aim_distance_;
    return c;
}

void Combat::push_player_out() {
    for (const Target& t : targets_) {
        if (!t.actor || !alive(t)) continue;
        const glm::vec3 feet = player_.center - glm::vec3(0, 0, player_.radius.z);
        if (feet.z > t.pos.z + t.height || feet.z + 2 * player_.radius.z < t.pos.z) continue;
        glm::vec2 d = glm::vec2(player_.center - t.pos);
        const float len = glm::length(d), min = t.radius + player_.radius.x;
        if (len >= min) continue;
        d = len > 1e-4f ? d / len : glm::vec2(0, 1);
        player_.center += glm::vec3(d * (min - len), 0);
    }
}

void Combat::launch_trail(const WeaponDef& w, const glm::vec3& from, const glm::vec3& to) {
    const float len = glm::distance(from, to);
    if (w.looks.trail < 0 || w.bullet_speed <= 0 || len < 0.5f) return;
    const size_t object = take_trail(w.looks.trail, from);
    if (object == size_t(-1)) return;
    scene_.play_clip(object, "start", false);
    scene_.set_object_visible(object, true);
    tracers_.push_back({object, w.looks.trail, from, from, (to - from) / len, to, w.bullet_speed});
}

void Combat::update_trails(float dt) {
    for (Tracer& t : tracers_) {
        if (!t.ending) {
            t.pos += t.dir * t.speed * dt;
            if (glm::dot(t.end - t.pos, t.dir) <= 0) {
                t.pos = t.end;
                if (!end_trail(t)) t.object = size_t(-1);
                continue;
            }
        } else if (scene_.clip_finished(t.object)) {
            scene_.set_object_visible(t.object, false);
            trail_pool_[t.type].push_back(t.object);
            t.object = size_t(-1);
            continue;
        }
        pose_trail(t.object, t.from, t.pos, t.dir, t.ending ? t.length : -1);
    }
    std::erase_if(tracers_, [](const Tracer& t) { return t.object == size_t(-1); });
}

bool Combat::end_trail(Tracer& t) {
    const float flown = glm::distance(t.from, t.pos);
    if (flown < 3) {
        scene_.set_object_visible(t.object, false);
        trail_pool_[t.type].push_back(t.object);
        return false;
    }
    t.ending = true, t.length = flown < 7.7f ? flown * 0.05f : 1.0f;
    scene_.play_clip(t.object, "end", false);
    pose_trail(t.object, t.from, t.pos, t.dir, t.length);
    return true;
}

size_t Combat::take_trail(int type, const glm::vec3& at) {
    std::vector<size_t>& pool = trail_pool_[type];
    if (pool.empty()) return scene_.spawn_object(type, -4000 - int(tracers_.size() + bolts_.size()), at, 0);
    const size_t object = pool.back();
    pool.pop_back();
    return object;
}

void Combat::pose_trail(size_t object, const glm::vec3& from, const glm::vec3& head, const glm::vec3& dir, float length) {
    // The streak lies along its -Y from the head, flat in its XY plane: turned about the
    // flight to face the eye, and in flight no longer than the way flown (it grows to 7 m).
    glm::vec3 side = glm::cross(dir, last_eye_ - head);
    side = glm::length(side) > 1e-4f ? glm::normalize(side) : glm::vec3(1, 0, 0);
    glm::mat4 m(1);
    m[0] = glm::vec4(side, 0);
    m[1] = glm::vec4(dir * (length >= 0 ? length : std::min(1.0f, glm::distance(from, head) / 7.0f)), 0);
    m[2] = glm::vec4(glm::cross(side, dir), 0);
    m[3] = glm::vec4(head, 1);
    scene_.set_object_transform(object, m);
}

void Combat::draw(Overlay& o, const glm::vec3& eye, int width, int height) const {
    for (const Bolt& b : bolts_) {
        if (b.trail_type >= 0 || !b.fx.empty() || b.body_type >= 0) continue;   // it has its own looks
        const glm::vec3 tail = b.pos - glm::normalize(b.vel) * 1.2f;
        beam(o.world, tail, b.pos, 0.12f, glm::vec4(glm::vec3(b.color), 0), b.color, eye);
        glow(o.world, b.pos, 0.18f, b.color, eye);
    }
    for (const Spark& s : sparks_) glow(o.world, s.pos, 0.25f, glm::vec4(glm::vec3(s.color), std::min(s.life * 8, 1.0f)), eye);

    // (The crosshair, the hit marker, health and ammo are the HUD's: app/game.cpp, game/hud.cpp.)
    (void)width, (void)height;
}

int Combat::count_teams(uint32_t team_mask) const {
    int n = 0;
    for (const Target& t : targets_) n += t.actor && ((1u << t.team) & team_mask) && alive(t);
    return n;
}

void Combat::spawn(SpawnManager& m, size_t slot, const SpawnManager::Entry& e) {
    const int wp = slot < m.points.size() ? m.points[slot] : -1;
    const LevelLogic::Object* point = logic_.object(wp);
    if (!point || e.type < 0) return;
    const int id = next_spawn_id_++;
    const float yaw = yaw_towards(point->position, player_.center);
    const size_t index = scene_.spawn_object(e.type, id, point->position, yaw);
    const LevelObject& lo = scene_.level().objects[index];
    LevelLogic::Object& o = logic_.add_object(id, index, lo.name, point->position, yaw);
    Target t;
    t.id = id;
    t.object = index;
    t.type = lo.name;
    t.pos = point->position;
    t.yaw = yaw;
    t.spawned = true;
    if (const Value* health = component(lo, kCompHealth)) {
        o.health = num(*health, 0, 10);
        if (health->size() > 9) t.hurt_sounds = sound_list((*health)[6]), t.death_sounds = sound_list((*health)[9]);
    }
    setup_actor(t, lo);
    t.aware = t.hostile;   // they come to fight
    targets_.push_back(std::move(t));
    logic_.enable(o, true);
    ++stats_.actors;
    // The waypoint's spawn script (CWayPointObject::RunSpawnScript), if any.
    if (e.waypoint >= 0) logic_.start_script("Obj" + std::to_string(e.waypoint) + "_OnSpawnStart", id);
}

void Combat::update_spawners(float dt) {
    for (SpawnManager& m : spawners_) {
        LevelLogic::Object* self = logic_.object(m.id);
        if (m.ended || !self || !self->enabled || m.waves.empty()) continue;
        if (m.wave < 0) {
            m.wave = 0, m.time = 0;
            m.spawned.assign(m.waves[0].entries.size(), 0);
        }
        m.time += dt;
        const SpawnManager::Wave& w = m.waves[size_t(m.wave)];
        bool complete = true;
        for (size_t i = 0; i < w.entries.size(); ++i) {
            if (m.spawned[i] || w.entries[i].type < 0) continue;
            if (m.time * 1000.0f >= float(w.entries[i].delay_ms)) {
                m.spawned[i] = 1;
                spawn(m, i, w.entries[i]);
            } else {
                complete = false;
            }
        }
        if (!complete) continue;
        if (m.wave + 1 >= int(m.waves.size())) {
            if (m.loop) {
                m.wave = -1;
                continue;
            }
            m.ended = true;
            logic_.enable(*self, false);
            logic_.runtime_.send_event("SpawnEnd_" + std::to_string(m.id));
            log("spawn manager " + std::to_string(m.id) + " ended");
        } else if (count_teams(m.teams) < m.waves[size_t(m.wave) + 1].threshold) {
            ++m.wave;
            m.time = 0;
            m.spawned.assign(m.waves[size_t(m.wave)].entries.size(), 0);
        }
    }
}

bool Combat::spawner_pending(int id) const {
    return std::any_of(spawners_.begin(), spawners_.end(), [&](const SpawnManager& m) { return m.id == id && !m.ended; });
}

std::vector<int> Combat::enemy_ids() const {
    std::vector<int> out;
    for (const Target& t : targets_)
        if (t.actor && ((1u << t.team) & ~kPlayerTeams) && alive(t)) out.push_back(t.id);
    return out;
}

bool Combat::damage(int id, float amount) {
    for (Target& t : targets_)
        if (t.id == id) {
            apply_damage(t, amount, false);
            return true;
        }
    return false;
}

void Combat::kill_all_enemies() {
    for (Target& t : targets_)
        if (t.actor && t.hostile && alive(t)) die(t);
}

void Combat::set_invulnerable(int id, bool on) {
    for (Target& t : targets_)
        if (t.id == id) t.invulnerable = on;
}

std::vector<std::string> Combat::describe() const {
    std::vector<std::string> out;
    for (const Target& t : targets_) {
        const LevelLogic::Object* o = logic_.object(t.id);
        char line[256];
        static const char* kStates[] = {"idle", "chase", "attack", "melee", "hurt", "dying", "dead"};
        std::snprintf(line, sizeof line, "%7d %-22s (%7.1f %7.1f %6.1f) %s%s%s%s team %d hp %.0f tri %d run %.1f m/s %s %s%s path %zu/%zu",
                      t.id, t.type.c_str(), t.pos.x, t.pos.y, t.pos.z, t.actor ? "actor " : "", t.hostile ? "hostile " : "",
                      t.ranged ? "ranged " : "", o && o->enabled ? "enabled" : "disabled", t.team, o ? o->health : 0.0f, t.tri,
                      t.run_speed, t.clip_run.c_str(), kStates[int(t.state)], t.aware ? " aware" : "", t.path_next, t.path.size());
        std::string l = line;
        if (!t.playing.empty()) l += " clip " + t.playing;
        for (const glm::vec3& p : t.path) {
            char pt[64];
            std::snprintf(pt, sizeof pt, " (%.1f %.1f %.1f)", p.x, p.y, p.z);
            l += pt;
        }
        out.push_back(l);
    }
    return out;
}

bool Combat::nearest_enemy(const glm::vec3& eye, glm::vec3& out) const {
    float best = std::numeric_limits<float>::max();
    for (const Target& t : targets_) {
        if (!t.actor || !t.hostile || !alive(t)) continue;
        const glm::vec3 c = t.pos + glm::vec3(0, 0, t.height * 0.6f);
        const float d = glm::distance(eye, c);
        if (d < best && d < 60 && line_clear(eye, c)) best = d, out = c;
    }
    return best < std::numeric_limits<float>::max();
}

}  // namespace nova
