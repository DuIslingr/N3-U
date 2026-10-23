#include "game/combat.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "audio/audio.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompHealth = 0x11;
constexpr int kCompAirNavigation = 0x1b;
constexpr int kCompGroundNavigation = 0x1d;
constexpr int kCompAwareness = 0x1f;
constexpr int kCompProjectile = 0x28;
constexpr int kCompWeapon = 0x47;
constexpr int kCompNpcWeapon = 0x48;
constexpr int kCompNpc = 0x4b;
constexpr int kCompAi = 0x4e;
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

// CProjectileComponent [18] material -> effect, [20] material -> sounds.
Impacts read_impacts(const Value& pr) {
    Impacts im;
    if (pr.size() > 18)
        for (const Value& e : pr[18].list)
            if (e.size() >= 2 && e[1].type == Value::String) im.effects[e[0].i] = e[1].s;
    if (pr.size() > 20)
        for (const Value& e : pr[20].list)
            if (e.size() >= 2) im.sounds[e[0].i] = sound_list(e[1]);
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

void rect(std::vector<ColorVertex>& out, float x, float y, float w, float h, const glm::vec4& c) {
    const uint32_t p = pack_rgba(c);
    out.insert(out.end(), {{{x, y, 0}, p}, {{x + w, y, 0}, p}, {{x + w, y + h, 0}, p},
                           {{x, y, 0}, p}, {{x + w, y + h, 0}, p}, {{x, y + h, 0}, p}});
}

}  // namespace

Combat::Combat(Scene& scene, Player& player, LevelLogic& logic, const Gol& gol)
    : scene_(scene), player_(player), logic_(logic), gol_(gol) {
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
        if (const Value* b = resolved_template(type, gol_, 0x29)) {
            if (b->size() > 5) player_hurt_sounds_ = sound_list((*b)[5]);
            regen_rate_ = num(*b, 1, 20);
            regen_pause_ = num(*b, 2, 3000) / 1000.0f;
        }
    }
    player_health = player_max_health;
    load_grenade();
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
        if (!str(*g, 10).empty()) d.effect = str(*g, 10);
        d.bounce_sound = str(*g, 15);
        d.explode_sounds = sound_list((*g)[18]);
    }
    if (const Value* fx = resolved_template(it->second, gol, 0x92); fx && fx->size() > 0 && (*fx)[0].size() > 0)
        d.trail = str((*fx)[0][0], 0);
    return d;
}

void Combat::load_grenade() {
    for (const auto& [id, type] : gol_) {
        if (type.name == "Grenade_Player") grenade_def_ = read_grenade(gol_, id);
        if (type.name == "NPC_Grenade") npc_grenade_def_ = read_grenade(gol_, id);
    }
}

void Combat::throw_grenade(const glm::vec3& eye, const glm::vec3& aim) {
    // Out of the right hand, along the view and a little up.
    const glm::vec3 right = glm::normalize(glm::cross(aim, glm::vec3(0, 0, 1)) + glm::vec3(1e-4f));
    glm::vec3 at = eye + aim * 0.4f + right * 0.2f - glm::vec3(0, 0, 0.15f);
    float t = 1;
    if (scene_.collision().raycast(eye, at, t)) at = eye;   // too close to a wall
    launch_grenade(grenade_def_, at, aim * (grenade_def_.speed * 0.6f) + glm::vec3(0, 0, 3.5f), false);
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
    g->bounce_wait = 0;
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
        if (!g.live) continue;
        g.bounce_wait = std::max(g.bounce_wait - dt, 0.0f);
        g.vel.z -= kGravity * dt;
        const glm::vec3 to = g.pos + g.vel * dt;
        float t = 1;
        glm::vec3 n(0, 0, 1);
        if (glm::length(g.vel) > 1e-3f && scene_.collision().raycast(g.pos, to, t, nullptr, &n)) {
            g.pos = glm::mix(g.pos, to, t) + n * 0.05f;
            const float speed = glm::length(g.vel);
            g.vel = glm::reflect(g.vel, n) * 0.4f;
            if (n.z > 0.7f && glm::length(g.vel) < 1.0f) g.vel = glm::vec3(0);   // at rest
            if (speed > 2.0f && g.bounce_wait <= 0) sound(g.def->bounce_sound, &g.pos), g.bounce_wait = 0.15f;
        } else {
            g.pos = to;
        }
        g.spin += glm::length(g.vel) * dt * 4.0f;
        scene_.set_object_transform(g.object, glm::rotate(glm::translate(glm::mat4(1), g.pos), g.spin, glm::vec3(1, 0, 0)));
        if (g.trail >= 0) scene_.particles().move(g.trail, glm::translate(glm::mat4(1), g.pos));
        if ((g.fuse -= dt) <= 0) {
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
        if (!t.actor || t.hostile) t.paralyzed = t.paralyze_time;
        if (paralyze_damage > 0 && t.actor && t.hostile) apply_damage(t, paralyze_damage, true);
    }
    for (Target& t : targets_) {
        if (!t.actor || !t.hostile || !alive(t)) continue;
        const glm::vec3 c = t.pos + glm::vec3(0, 0, t.height * 0.5f);
        const glm::vec3 to = c - eye;
        const float d = glm::length(to);
        if (d > 40.0f || d < 1e-3f || glm::dot(to / d, aim) < 0.85f || !line_clear(eye, c)) continue;
        t.paralyzed = t.paralyze_time;
    }
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
        killing_repel_ = true;
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
        apply_damage(t, damage * (1 - d / r), by_player);
    }
    const float d = glm::distance(at, player_center());
    if (d < r && line_clear(at + glm::vec3(0, 0, 0.2f), player_.eye())) {
        damage_player(damage * (by_player ? 0.5f : 1.0f) * (1 - d / r), at);   // the player's own: half
    }
    if (d < r * 3) logic_.shake(400, 1.5f * (1 - d / (r * 3)));
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
    s.grenades = grenades;
    s.powers = powers_, s.power_index = power_index_;
    for (const Target& t : targets_) {
        if (!t.actor || t.spawned) continue;
        const LevelLogic::Object* o = logic_.object(t.id);
        s.actors.push_back({t.id, o && o->dead, t.pos, t.yaw});
    }
    s.stats = stats_;
    return s;
}

void Combat::restore(const Saved& s) {
    if (s.owned.size() == owned_.size()) owned_ = s.owned, clip_ = s.clip, reserve_ = s.reserve;
    if (s.current >= 0 && size_t(s.current) < owned_.size()) current_ = s.current;
    reload_left_ = 0, fire_cooldown_ = 0, spread_ = 0;
    grenades = s.grenades;
    if (!s.powers.empty()) powers_ = s.powers, power_index_ = std::clamp(s.power_index, 0, int(powers_.size()) - 1);
    power_cooldown_ = 0;
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
        w.name = f[0].s;
        // CComponentWeapon::Load: ..., max ammo (+0x4c), clip size (+0x50), ...
        w.reserve = f[16].i;
        w.clip = std::max(f[17].i, 1);
        w.spread_step = f[27].as_float();
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
            if (auto p = gol_.find(mode[1].i); p != gol_.end()) {
                if (const Value* pr = resolved_template(p->second, gol_, kCompProjectile); pr && pr->size() > 4) {
                    w.damage_far = num(*pr, 1, 1), w.damage_near = num(*pr, 2, 2);
                    w.far_dist = num(*pr, 3, 30), w.near_dist = num(*pr, 4, 30);
                    w.impacts = read_impacts(*pr);
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
    throw_delay_ = -1;
    targets_.clear();
    spawners_.clear();
    bolts_.clear();
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
        if (t.explosive) t.radius = 0.5f, t.height = 1.0f;
        const bool ai = component(o, kCompAi) || component(o, 0x55) || component(o, 0x21) || component(o, 0x57) ||
                        component(o, 0x59) || component(o, 0x23);
        if (ai && (component(o, kCompGroundNavigation) || component(o, kCompAirNavigation))) setup_actor(t, o);
        targets_.push_back(std::move(t));
        ++stats_.targets;
        if (targets_.back().actor) ++stats_.actors;
        if (targets_.back().hostile) ++stats_.hostile;
    }
    load_covers();
    log("combat: " + std::to_string(stats_.targets) + " targets, " + std::to_string(stats_.actors) + " actors (" +
        std::to_string(stats_.hostile) + " hostile), " + std::to_string(spawners_.size()) + " spawn managers, nav mesh " +
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
    // Ranged weapon: CNPCComponent weapon list -> CNPCWeaponComponent -> its projectile.
    if (const Value* npc = component(o, kCompNpc); npc && npc->size() > 1 && (*npc)[1].size() > 0) {
        const int weapon = (*npc)[1][0][0].i;
        if (auto w = gol_.find(weapon); w != gol_.end()) {
            if (const Value* nw = resolved_template(w->second, gol_, kCompNpcWeapon); nw && nw->size() > 0) {
                if (nw->size() > 22) t.fire_sounds = sound_list((*nw)[22]);
                set_projectile(t, (*nw)[0].i);
            }
        }
    }
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
    if (scene_.play_clip(t.object, clip, loop)) t.playing = clip;
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
        g.kind = NpcGoal::Kind::Teleport, g.pos = wp->position, g.id = -1;
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
        // Behind the gun, holding there, firing longer bursts.
        const LevelLogic::Object* gun = logic_.object(arg);
        if (!gun) return false;
        const float y = glm::radians(gun->yaw);
        t->pos = gun->position + glm::vec3(std::sin(y), -std::cos(y), 0) * 1.0f;
        t->yaw = gun->yaw, t->hold = true, t->leaping = false;
        t->burst = std::max(t->burst, 12), t->burst_pause = 0.8f, t->fire_range = std::max(t->fire_range, 60.0f);
        scene_.set_object_pose(t->object, t->pos, t->yaw);
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
            if (first) g.length = std::clamp(glm::distance(t.pos, g.pos) / 8.0f, 0.4f, 1.2f), t.leap_from = t.pos;
            const float k = std::min(g.timer / g.length, 1.0f);
            t.pos = glm::mix(t.leap_from, g.pos, k) + glm::vec3(0, 0, 4 * k * (1 - k) * 1.5f);
            face(t, g.pos, dt);
            play(t, t.clip_jump.empty() ? t.clip_run : t.clip_jump, true);
            if (k >= 1) {
                glm::vec3 on;
                t.tri = nav_.nearest(t.pos + glm::vec3(0, 0, 0.3f), 2.0f, on);
                done = true;
            }
            break;
        }
        case NpcGoal::Kind::Turn:
            face(t, g.pos, dt);
            play(t, t.clip_idle, true);
            done = g.timer > 0.6f;
            break;
        case NpcGoal::Kind::Teleport:
            t.pos = g.pos;
            t.tri = -1;
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
        if (scene_.clip_finished(t.object)) t.state = S::Dead;
        return;
    }
    if (t.state == S::Dead) return;
    if (!t.goals.empty()) {
        run_goal(t, dt);
        return;
    }
    if (t.paralyzed > 0) {   // the Paralyze power: frozen in place
        t.paralyzed -= dt;
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
        play(t, t.clip_fire, true);
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
void Combat::attack_fire(Target& t, const glm::vec3& chest, bool on_player, bool sees, float dt) {
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

void Combat::fire_bolt(Target& t, const glm::vec3& at) {
    glm::vec3 muzzle = t.pos + glm::vec3(0, 0, t.height * 0.7f);
    const float y = glm::radians(t.yaw);
    muzzle += glm::vec3(-std::sin(y), std::cos(y), 0) * (t.radius + 0.3f);
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
    bolts_.push_back({muzzle, aim * t.bolt_speed, t.bolt_damage, 3.0f, t.bolt_color, int(&t - targets_.data())});
    if (t.shots_left == t.burst) sound(t.fire_sounds, &muzzle);   // the sounds are whole bursts
    ++stats_.enemy_shots;
}

void Combat::damage_player(float amount, const glm::vec3& from) {
    (void)from;
    if (player_dead || player_invulnerable || protect_ > 0 || amount <= 0) return;
    if (logic_.in_mech()) amount *= 0.3f;   // the mech's armour (its health is 200 to the player's 50)
    player_health -= amount;
    stats_.damage_taken += amount;
    regen_delay_ = regen_pause_;
    damage_flash_ = std::min(damage_flash_ + amount / 30.0f + 0.15f, 1.0f);
    if (player_hurt_wait_ <= 0) {
        sound(player_hurt_sounds_, nullptr);
        player_hurt_wait_ = 1.5f;
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
    if (by_player) logic_.runtime_.send_event("TargetHit", t.id);
    if (t.actor) notice(t);
    if (o->health <= 0) {
        die(t);
        return;
    }
    if (t.hurt_sound_wait <= 0 && !t.hurt_sounds.empty()) {
        const glm::vec3 at = t.pos + glm::vec3(0, 0, t.height * 0.6f);
        sound(t.hurt_sounds, &at);
        t.hurt_sound_wait = 1.2f;
    }
    // Flinch now and then.
    if (t.actor && !t.clip_hurt.empty() && t.hurt_cooldown <= 0 && t.state != Target::State::Melee &&
        std::uniform_real_distribution<float>(0, 1)(rng_) < 0.35f) {
        t.state = Target::State::Hurt;
        t.timer = 0;
        t.hurt_cooldown = 2.5f;
        play(t, t.clip_hurt, false);
    }
}

void Combat::sound(const std::vector<std::string>& names, const glm::vec3* at) {
    if (!logic_.audio || names.empty()) return;
    Audio::Params p;
    p.position = at;
    logic_.audio->play_one_of(names, p);
}

void Combat::impact(const Impacts& im, uint8_t material, const glm::vec3& at, const glm::vec3& normal) {
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

void Combat::die(Target& t) {
    LevelLogic::Object* o = logic_.object(t.id);
    if (!o || o->dead) return;
    leave_cover(t);
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
    if (t.actor && !t.clip_deaths.empty()) {
        t.state = Target::State::Dying;
        play(t, t.clip_deaths[std::uniform_int_distribution<size_t>(0, t.clip_deaths.size() - 1)(rng_)], false);
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
            if (d < 6.0f) apply_damage(other, 40.0f * (1 - d / 6.0f), true);
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
    since_shot_ = 0;
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
    // Spread: a cone that widens while firing.
    std::normal_distribution<float> n(0.0f, glm::radians(0.3f + spread_) * 0.5f);
    glm::vec3 right = glm::cross(aim_dir, glm::vec3(0, 0, 1));
    right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3(1, 0, 0);
    const glm::vec3 up = glm::normalize(glm::cross(right, aim_dir));
    const glm::vec3 dir = glm::normalize(aim_dir + right * n(rng_) + up * n(rng_));
    spread_ = std::min(spread_ + w.spread_step * 0.35f, 4.0f);

    const float range = 300;
    float wall_t = 1;
    uint8_t material = 0;
    glm::vec3 normal(0, 0, 1);
    const bool wall = scene_.collision().raycast(eye, eye + dir * range, wall_t, &material, &normal);
    float target_t = 0;
    glm::vec3 point;
    const int hit = ray_target(eye, dir, wall ? wall_t * range : range, target_t, point);
    const glm::vec3 end = hit >= 0 ? point : eye + dir * (wall ? wall_t * range : range);
    const glm::vec3 muzzle = eye + aim_dir * 0.6f + right * 0.18f - up * 0.16f;
    tracers_.push_back({muzzle, end, 0.06f});
    if (hit >= 0) {
        Target& t = targets_[size_t(hit)];
        const float d = glm::distance(eye, point);
        float dmg = w.damage_near;
        if (d > w.far_dist) dmg = w.damage_far;
        else if (d > w.near_dist) dmg = w.damage_near + (d - w.near_dist) * (w.damage_far - w.damage_near) / (w.far_dist - w.near_dist);
        const bool head = t.headshots && point.z > t.pos.z + t.height * 0.84f;
        if (head) dmg *= 2;   // CTargetableComponent head multiplier
        ++stats_.hits;
        hit_marker_ = 0.15f;
        if (!effect(t.hit_effect, point, -dir)) sparks_.push_back({point, 0.12f, {1.0f, 0.3f, 0.2f, 1}});
        killing_headshot_ = head;
        apply_damage(t, dmg, true);
        killing_headshot_ = false;
    } else if (wall) {
        impact(w.impacts, material, end, normal);
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
    burst_cooldown_ = std::max(burst_cooldown_ - dt, 0.0f);
    player_hurt_wait_ = std::max(player_hurt_wait_ - dt, 0.0f);
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
    for (Target& t : targets_) {
        LevelLogic::Object* o = logic_.object(t.id);
        if (!o || !o->enabled) continue;
        if (t.state != Target::State::Dying && t.state != Target::State::Dead && (o->dead || o->health <= 0)) {
            o->dead = false;
            die(t);   // SetHealth(id, 0) from a script
            continue;
        }
        if (t.actor) update_actor(t, dt);
        else if (t.paralyzed > 0) t.paralyzed -= dt;
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
        const bool wall = scene_.collision().raycast(b.pos, next, t, &material, &normal);
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
        const Target* shooter = b.owner >= 0 && size_t(b.owner) < targets_.size() ? &targets_[size_t(b.owner)] : nullptr;
        const bool hits_target = hit >= 0 && hit != b.owner && shooter &&
                                 ((shooter->attacks & (1u << targets_[size_t(hit)].team)) != 0 || targets_[size_t(hit)].id == shooter->locked);
        if (!player_dead && shooter && (shooter->attacks & kPlayerTeamBit) && glm::length(glm::vec2(off)) < player_.radius.x + 0.15f &&
            std::abs(off.z) < player_.radius.z) {
            damage_player(b.damage, b.pos);
            b.life = 0;
        } else if (hits_target) {
            apply_damage(targets_[size_t(hit)], b.damage, false);
            if (!targets_[size_t(hit)].hit_effect.empty()) effect(targets_[size_t(hit)].hit_effect, hit_p, -seg);
            b.life = 0;
        } else if (wall) {
            if (b.owner >= 0 && size_t(b.owner) < targets_.size() && !targets_[size_t(b.owner)].bolt_impacts.effects.empty())
                impact(targets_[size_t(b.owner)].bolt_impacts, material, end, normal);
            else if (!effect("impact_plasma", end, normal))
                sparks_.push_back({end, 0.15f, b.color});
            b.life = 0;
        }
        b.pos = end;
    }
    bolts_.erase(std::remove_if(bolts_.begin(), bolts_.end(), [](const Bolt& b) { return b.life <= 0; }), bolts_.end());
    for (Tracer& t : tracers_) t.life -= dt;
    tracers_.erase(std::remove_if(tracers_.begin(), tracers_.end(), [](const Tracer& t) { return t.life <= 0; }), tracers_.end());
    for (Spark& s : sparks_) s.life -= dt;
    sparks_.erase(std::remove_if(sparks_.begin(), sparks_.end(), [](const Spark& s) { return s.life <= 0; }), sparks_.end());

    // The player: death and revival, regeneration.
    damage_flash_ = std::max(damage_flash_ - dt * 1.5f, 0.0f);
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
        if (logic_.fall_damage && v > 13.0f) damage_player((v - 13.0f) * 8.0f, player_.center);
    }
    if (core >= 0 && player_health <= player_max_health) core = -1;   // the core's charge is spent
    regen_delay_ -= dt;
    if (regen_delay_ <= 0 && player_health < player_max_health)
        player_health = std::min(player_health + regen_rate_ * dt, player_max_health);

    // Grenades: the throw animation starts, the grenade leaves the hand a moment later.
    grenade_cooldown_ = std::max(grenade_cooldown_ - dt, 0.0f);
    if (in.grenade && grenades_enabled && grenades > 0 && grenade_cooldown_ <= 0 && throw_delay_ < 0) {
        --grenades;
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
    spread_ = std::max(spread_ - dt * 6.0f, 0.0f);
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

void Combat::draw(Overlay& o, const glm::vec3& eye, int width, int height) const {
    for (const Tracer& t : tracers_) {
        const float a = t.life / 0.06f;
        beam(o.world, t.from, t.to, 0.035f, {1.0f, 0.85f, 0.5f, 0.9f * a}, {1.0f, 0.7f, 0.3f, 0.2f * a}, eye);
    }
    for (const Bolt& b : bolts_) {
        const glm::vec3 tail = b.pos - glm::normalize(b.vel) * 1.2f;
        beam(o.world, tail, b.pos, 0.12f, glm::vec4(glm::vec3(b.color), 0), b.color, eye);
        glow(o.world, b.pos, 0.18f, b.color, eye);
    }
    for (const Spark& s : sparks_) glow(o.world, s.pos, 0.25f, glm::vec4(glm::vec3(s.color), std::min(s.life * 8, 1.0f)), eye);

    // HUD, in pixels.
    const float w = float(width), h = float(height), u = h / 720.0f;
    const float cx = w * 0.5f, cy = h * 0.5f;
    if (damage_flash_ > 0 || player_dead) {
        const float a = player_dead ? 0.55f : damage_flash_ * 0.45f;
        const float edge = 90 * u;
        const glm::vec4 red(0.7f, 0.0f, 0.0f, a), clear(0.7f, 0.0f, 0.0f, 0);
        auto grad = [&](float x0, float y0, float x1, float y1, bool horizontal, bool flip) {
            const uint32_t pa = pack_rgba(flip ? clear : red), pb = pack_rgba(flip ? red : clear);
            if (horizontal)
                o.screen.insert(o.screen.end(), {{{x0, y0, 0}, pa}, {{x1, y0, 0}, pb}, {{x1, y1, 0}, pb},
                                                 {{x0, y0, 0}, pa}, {{x1, y1, 0}, pb}, {{x0, y1, 0}, pa}});
            else
                o.screen.insert(o.screen.end(), {{{x0, y0, 0}, pa}, {{x1, y0, 0}, pa}, {{x1, y1, 0}, pb},
                                                 {{x0, y0, 0}, pa}, {{x1, y1, 0}, pb}, {{x0, y1, 0}, pb}});
        };
        grad(0, 0, edge, h, true, false);
        grad(w - edge, 0, w, h, true, true);
        grad(0, 0, w, edge, false, false);
        grad(0, h - edge, w, h, false, true);
        if (player_dead) rect(o.screen, 0, 0, w, h, {0.25f, 0, 0, 0.35f});
    }
    // Crosshair: red over an enemy, wider while the spread is open.
    const glm::vec4 cc = aim_on_enemy_ ? glm::vec4(1, 0.25f, 0.2f, 0.9f) : glm::vec4(1, 1, 1, 0.75f);
    const float gap = (6 + spread_ * 4) * u, len = 9 * u, th = std::max(2 * u, 1.0f);
    rect(o.screen, cx - gap - len, cy - th / 2, len, th, cc);
    rect(o.screen, cx + gap, cy - th / 2, len, th, cc);
    rect(o.screen, cx - th / 2, cy - gap - len, th, len, cc);
    rect(o.screen, cx - th / 2, cy + gap, th, len, cc);
    if (hit_marker_ > 0) {
        const float s = 7 * u, a = hit_marker_ / 0.15f;
        for (int k = 0; k < 4; ++k) {
            const float dx = (k & 1) ? 1.0f : -1.0f, dy = (k & 2) ? 1.0f : -1.0f;
            rect(o.screen, cx + dx * (s + 2 * u) - th / 2, cy + dy * (s + 2 * u) - th / 2, th * 2, th * 2, {1, 1, 1, a});
        }
    }
    // (Health shows as the damage flash, ammo on the HUD's weapon panel: game/hud.cpp.)
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
