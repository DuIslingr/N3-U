// Combat's NPC tactics: aggressiveness and its bands, cover points and the fight from cover
// (CAIComponent::Update/UpdateLowCover/UpdateHiCover, CNPCComponent::ChooseCoverInRange).
// See combat.h.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/combat.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompBuiltinCover = 0xb3;

float field(const Value& v, size_t i, float fallback) {
    return i < v.size() && (v[i].type == Value::Float || v[i].type == Value::Int) ? v[i].as_float() : fallback;
}

}  // namespace

void Combat::load_covers() {
    covers_.clear();
    const Level& level = scene_.level();
    for (size_t i = 0; i < level.objects.size(); ++i) {
        const LevelObject& o = level.objects[i];
        auto c = o.components.find(kCompBuiltinCover);
        if (c == o.components.end() || c->second.size() == 0) continue;
        // CCoverPoint::InitFrom: the entry's offset placed with the object; it faces +Y turned
        // by the entry's angle, then by the object.
        const glm::mat4 m = scene_.object_matrix(i);
        for (const Value& e : c->second[0].list) {
            if (e.size() < 10) continue;
            CoverPoint p;
            p.pos = glm::vec3(m * glm::vec4(field(e, 0, 0), field(e, 1, 0), field(e, 2, 0), 1));
            const float a = glm::radians(field(e, 3, 0));
            p.dir = glm::normalize(glm::vec3(m * glm::vec4(-std::sin(a), std::cos(a), 0, 0)));
            p.low = e[6].i != 0;
            glm::vec3 on;
            if (nav_.nearest(p.pos + glm::vec3(0, 0, 0.3f), 1.5f, on) >= 0) p.pos = on;   // standing on the walkable area
            covers_.push_back(p);
        }
    }
}

void Combat::read_ai(Target& t, const Value& ai) {
    // CComponentAI: [6] uses covers; [21]/[22] attack time (ms) min/max; [25]/[26] time
    // hidden in low cover; [51]-[65] the aggressiveness factors; [66] the bands.
    t.use_covers = ai.size() > 6 && ai[6].i != 0;
    t.attack_min = field(ai, 21, 1000) / 1000.0f, t.attack_max = field(ai, 22, 3000) / 1000.0f;
    t.idle_in_cover_min = field(ai, 25, 1000) / 1000.0f, t.idle_in_cover_max = field(ai, 26, 2000) / 1000.0f;
    t.af_hurt = field(ai, 51, 100), t.af_unhurt = field(ai, 52, 0.8f), t.af_near = field(ai, 53, 15), t.af_near_dist = field(ai, 54, 10);
    t.af_unseen = field(ai, 55, 0), t.af_behind = field(ai, 56, 0), t.af_in_cover = field(ai, 59, 0), t.af_out_of_cover = field(ai, 60, 0);
    t.af_team_alive = field(ai, 63, 0), t.af_team_dead = field(ai, 64, 0), t.af_last = field(ai, 65, 0);
    t.bands.clear();
    if (ai.size() > 66)
        for (const Value& b : ai[66].list) {
            if (b.size() < 24) continue;
            Target::Band band;
            band.threshold = field(b, 1, 0);
            band.range_min = field(b, 2, 15), band.range_max = field(b, 3, 25);
            band.pause_min = field(b, 22, 1000) / 1000.0f, band.pause_max = field(b, 23, 1000) / 1000.0f;
            t.bands.push_back(band);
        }
}

const Combat::Target::Band* Combat::band(const Target& t) const {
    // GetBehaviourAttack: the first band (from the most aggressive) its aggressiveness reaches.
    for (const Target::Band& b : t.bands)
        if (t.aggr >= b.threshold) return &b;
    return t.bands.empty() ? nullptr : &t.bands.back();
}

bool Combat::lock_aggressiveness(int npc, float value) {
    Target* t = find_target(npc);
    if (!t || !t->actor) return false;
    t->aggr = value, t->aggr_locked = true;
    return true;
}

void Combat::update_aggressiveness(Target& t, float dt, float dist, bool sees, bool in_cover) {
    if (t.aggr_locked) return;
    t.aggr += dt * t.af_unhurt;
    if (dist < t.af_near_dist) t.aggr += dt * t.af_near * (1 - dist / t.af_near_dist);
    if (t.enemy == -1) {   // where the player looks: unseen by them, or behind them
        const glm::vec3 to = glm::normalize(t.pos - player_center() + glm::vec3(0, 0, 1e-4f));
        const float facing = glm::dot(to, player_.view_forward());
        if (facing < 0.5f) t.aggr += dt * t.af_unseen;
        if (facing < 0) t.aggr += dt * t.af_behind;
    }
    t.aggr += in_cover ? dt * t.af_in_cover : -dt * t.af_out_of_cover;
    // Its team: each member fighting raises it; members lost lower it, the last one more.
    int team = 0;
    for (const Target& o : targets_)
        if (&o != &t && o.actor && o.team == t.team && o.aware && alive(o)) ++team;
    t.aggr += dt * t.af_team_alive * float(team);
    if (t.team_seen > team) t.aggr -= float(t.team_seen - team) * t.af_team_dead;
    if (t.team_seen > 0 && team == 0) t.aggr -= t.af_last;
    t.team_seen = team;
    t.aggr = std::clamp(t.aggr, 0.0f, 100.0f);
    (void)sees;
}

bool Combat::cover_protects(const CoverPoint& c, const glm::vec3& from) const {
    // Hidden there (crouched behind a low cover, standing behind a high one): the line from
    // the enemy's eye is blocked.
    return !line_clear(from, c.pos + glm::vec3(0, 0, c.low ? 0.8f : 1.5f));
}

int Combat::choose_cover(const Target& t, const glm::vec3& enemy_feet, const glm::vec3& enemy_eye) const {
    const Target::Band* b = band(t);
    const float lo = b ? b->range_min : 10.0f, hi = b ? b->range_max : 25.0f;
    const int self = int(&t - targets_.data());
    // ChooseCoverInRange: the nearest free cover at the band's distance from the enemy that
    // hides from it and can be reached.
    std::vector<std::pair<float, int>> order;
    for (size_t i = 0; i < covers_.size(); ++i) {
        const CoverPoint& c = covers_[i];
        if (c.holder >= 0 && c.holder != self) continue;
        const float d = glm::distance(c.pos, t.pos);
        if (d > 30.0f || std::abs(c.pos.z - t.pos.z) > 6.0f) continue;
        order.emplace_back(d, int(i));
    }
    std::sort(order.begin(), order.end());
    for (const auto& [d, i] : order) {
        const CoverPoint& c = covers_[size_t(i)];
        const float to_enemy = glm::distance(glm::vec2(c.pos), glm::vec2(enemy_feet));
        if (to_enemy < std::min(lo, 6.0f) * 0.5f || to_enemy > hi * 1.5f) continue;
        if (!c.low && glm::dot(c.dir, glm::normalize(c.pos - enemy_feet + glm::vec3(0, 0, 1e-4f))) < 0) continue;
        if (!cover_protects(c, enemy_eye)) continue;
        // Not across the enemy's way: the cover is not nearer to it than to the actor... unless
        // it is ahead of the actor towards the band's distance.
        glm::vec3 on;
        const int tri = nav_.nearest(c.pos + glm::vec3(0, 0, 0.3f), 1.5f, on);
        std::vector<glm::vec3> path;
        if (tri < 0 || t.tri < 0 || !nav_.find_path(t.tri, t.pos, tri, on, path, [this](int door) { return logic_.door_passable(door); }))
            continue;
        return i;
    }
    return -1;
}

void Combat::set_crouched(Target& t, bool on) {
    if (t.crouched == on) return;
    t.crouched = on;
    t.height = on ? t.stand_height * 0.6f : t.stand_height;
}

void Combat::leave_cover(Target& t) {
    if (t.cover >= 0 && size_t(t.cover) < covers_.size()) covers_[size_t(t.cover)].holder = -1;
    t.cover = -1;
    set_crouched(t, false);
    if (t.token) t.token = false;
}

bool Combat::fight_from_cover(Target& t, float dt, const glm::vec3& target, const glm::vec3& chest, bool sees, bool on_player) {
    const float dist = glm::distance(glm::vec2(t.pos), glm::vec2(target));
    const bool in_cover = t.cover >= 0 && t.cover_phase != Target::CoverPhase::Moving;
    update_aggressiveness(t, dt, dist, sees, in_cover);
    if (!t.use_covers || !t.ranged || !t.mobile || t.hold || t.has_region || nav_.empty() || t.tri < 0 || covers_.empty()) return false;
    const glm::vec3 enemy_eye = chest + glm::vec3(0, 0, 0.3f);
    // The cover still hides it? Flanked, or the enemy upon it: out (CheckPositionIsSafe).
    if (t.cover >= 0) {
        const CoverPoint& c = covers_[size_t(t.cover)];
        if (glm::distance(glm::vec2(c.pos), glm::vec2(target)) < 4.0f || !cover_protects(c, enemy_eye)) leave_cover(t), t.cover_eval = 0;
    }
    // Looking for a cover every 3-5 s while it has none (timer 9).
    if (t.cover < 0) {
        if ((t.cover_eval -= dt) > 0) return false;
        t.cover_eval = std::uniform_real_distribution<float>(3.0f, 5.0f)(rng_);
        const int c = choose_cover(t, target, enemy_eye);
        if (c < 0) return false;
        t.cover = c, covers_[size_t(c)].holder = int(&t - targets_.data());
        if (logic_.trace) {
            char line[160];
            const glm::vec3 p = covers_[size_t(c)].pos;
            std::snprintf(line, sizeof line, "%d takes %s cover %d at (%.1f %.1f %.1f), aggressiveness %.0f", t.id,
                          covers_[size_t(c)].low ? "low" : "high", c, p.x, p.y, p.z, t.aggr);
            log(line);
        }
        t.cover_phase = Target::CoverPhase::Moving;
        t.state = Target::State::Chase;
    }
    const CoverPoint& c = covers_[size_t(t.cover)];
    const Target::Band* b = band(t);
    auto clip = [&](const char* state, const std::string& fallback) {
        const std::string s = logic_.state_clip(scene_.level().objects[t.object].type, state);
        return s.empty() ? fallback : s;
    };
    switch (t.cover_phase) {
        case Target::CoverPhase::Moving:
            if (glm::distance(glm::vec2(t.pos), glm::vec2(c.pos)) > 0.6f) {
                go_towards(t, c.pos, 0.4f, dt);
                if (t.path_next >= t.path.size() && glm::distance(glm::vec2(t.pos), glm::vec2(c.pos)) > 2.0f) {   // no way there
                    leave_cover(t);
                    return false;
                }
                return true;
            }
            t.cover_phase = Target::CoverPhase::Hidden;
            t.cover_timer = std::uniform_real_distribution<float>(t.idle_in_cover_min, std::max(t.idle_in_cover_max, t.idle_in_cover_min))(rng_);
            [[fallthrough]];
        case Target::CoverPhase::Hidden:
            face(t, target, dt);
            if (c.low) set_crouched(t, true), play(t, clip("CrouchIdle", t.clip_alert), true);
            else play(t, clip("HighCoverIdle", t.clip_alert), true);
            if ((t.cover_timer -= dt) > 0) return true;
            {
                // Up to fire needs one of the attack tokens at the player (else wait a moment).
                int shooters = 0;
                for (const Target& o : targets_) shooters += o.token && &o != &t && alive(o);
                if (on_player && shooters >= max_shooters_) {
                    t.cover_timer = 0.4f;
                    return true;
                }
            }
            // UpdateLowCover state 2: attack for attack min + (max - min) x aggressiveness/100.
            t.cover_phase = Target::CoverPhase::Firing;
            t.cover_timer = t.attack_min + (t.attack_max - t.attack_min) * t.aggr * 0.01f;
            set_crouched(t, false);
            t.fire_timer = std::min(t.fire_timer, 0.3f);
            return true;
        case Target::CoverPhase::Firing:
            face(t, target, dt);
            play(t, c.low ? clip("LowCoverFire", t.clip_fire) : clip("HighCoverLeanLeftAttack", t.clip_fire), true);
            if (b) t.burst_pause = std::uniform_real_distribution<float>(b->pause_min, std::max(b->pause_max, b->pause_min))(rng_);
            attack_fire(t, chest, on_player, sees, dt);
            if ((t.cover_timer -= dt) <= 0 && t.shots_left <= 0) {   // back down
                t.token = false;
                t.cover_phase = Target::CoverPhase::Hidden;
                t.cover_timer = std::uniform_real_distribution<float>(t.idle_in_cover_min, std::max(t.idle_in_cover_max, t.idle_in_cover_min))(rng_);
            }
            return true;
    }
    return true;
}

}  // namespace nova
