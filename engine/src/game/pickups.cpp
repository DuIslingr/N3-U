// Combat's pick-ups: CPowerUpComponent::Update and ConsumePowerUp, CLevel::RespawnPowerUps.
// See combat.h.
#include <algorithm>

#include "game/combat.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr float kPlayerRadius = 0.5f;   // the player's CActorsCollisionComponent
constexpr int kMaxPowerAmmo = 300;      // PowerManager: ThrowGrenade's and ThrowMine's maximum
constexpr float kInfoTime = 2.0f;       // ShowPowerUpInfo: 2000 ms

int integer(const Value& v, size_t i, int fallback) { return i < v.size() ? int(v[i].as_float()) : fallback; }

}  // namespace

void Combat::load_pickup(int id, const LevelObject& o, const Value& v) {
    (void)o;
    if (v.size() < 10) return;
    Pickup p;
    p.id = id;
    if (const int gol_id = integer(v, 0, -1); gol_id >= 0)
        if (auto g = gol_.find(gol_id); g != gol_.end()) p.weapon = find_weapon(g->second.name);
    p.power = integer(v, 1, -1);
    if (p.power != 1 && p.power != 2) p.power = -1;
    if (p.weapon < 0 && p.power < 0) return;   // a weapon not in the arsenal (ShotGunNOVA)
    p.unlocks = integer(v, 3, 0) == 0;
    p.amount = integer(v, 4, 0);
    p.radius = v[5].as_float();
    p.regenerate = v[6].as_float();
    p.sound = v[7].s;
    p.switch_to = integer(v, 9, 0) != 0;
    pickups_.push_back(p);
    if (logic_.trace)
        log("pick-up " + std::to_string(id) + ": " + (p.weapon >= 0 ? weapon_types_[size_t(p.weapon)] : p.power == 1 ? "grenades" : "mines") +
            " x" + std::to_string(p.amount));
}

bool Combat::take_pickup(const Pickup& p) {
    // CPowerUpComponent::ConsumePowerUp: the power's ammunition, then the weapon's; taken if
    // either gave something.
    bool taken = false;
    if (p.power >= 0 && (p.power == 1 ? grenades_enabled : mines_enabled)) {
        int& have = p.power == 1 ? grenades : mines;
        const int before = have;
        have = std::min(have + p.amount, kMaxPowerAmmo);
        if (have > before) {
            pickup_info_ = {p.power == 1 ? "grenades.bsprite" : "mines.bsprite", have - before, kInfoTime};
            taken = true;
        }
    }
    if (p.weapon >= 0) {
        const size_t i = size_t(p.weapon);
        const bool had = owned_[i] != 0;
        const int max = weapons_[i].reserve;
        // A weapon held full is left (unless the power part was taken).
        if (!(had && max > 0 && reserve_[i] >= max)) {
            const bool empty = had && clip_[i] <= 0 && reserve_[i] == 0;
            int gave = 0;
            if (!had && p.unlocks) {
                unlock_weapon(weapon_types_[i]);
                gave = std::max(max, 0);
                // To it if it is new (or was empty) and further along the weapon slots.
                if (p.switch_to && p.weapon > current_ && !special_weapon())
                    current_ = p.weapon, reload_left_ = 0, fire_cooldown_ = 0.4f;
            } else if (had) {
                const int before = reserve_[i];
                if (reserve_[i] >= 0) reserve_[i] = max >= 0 ? std::min(reserve_[i] + p.amount, max) : reserve_[i] + p.amount;
                gave = reserve_[i] - before;
                if (empty && gave > 0 && p.switch_to && p.weapon > current_ && !special_weapon())
                    current_ = p.weapon, reload_left_ = 0, fire_cooldown_ = 0.4f;
            }
            if (owned_[i] && (gave > 0 || !had)) {
                if (!taken) pickup_info_ = {weapons_[i].sprite, gave, kInfoTime};
                taken = true;
            }
        }
    }
    if (!taken) return false;
    if (logic_.trace) log("picked up " + std::to_string(p.id));
    sound(p.sound, nullptr);
    if (LevelLogic::Object* o = logic_.object(p.id)) logic_.enable(*o, false);
    return true;
}

void Combat::update_pickups(float dt) {
    pickup_info_.time = std::max(pickup_info_.time - dt, 0.0f);
    const glm::vec3 feet = player_.center - glm::vec3(0, 0, player_.radius.z);
    for (Pickup& p : pickups_) {
        LevelLogic::Object* o = logic_.object(p.id);
        if (!o) continue;
        if (p.gone >= 0) {   // CLevel::RespawnPowerUps: back after its time
            if ((p.gone += dt) > p.regenerate) p.gone = -1, logic_.enable(*o, true);
            continue;
        }
        if (!o->enabled || player_dead) continue;
        const float reach = p.radius + kPlayerRadius;
        const glm::vec3 d = o->position - feet;
        if (glm::dot(d, d) >= reach * reach) continue;
        if (take_pickup(p) && p.regenerate >= 0) p.gone = 0;
    }
}

}  // namespace nova
