// Combat's NPC tactics: aggressiveness and its bands, cover points and the fight from cover
// (CAIComponent::Update/UpdateLowCover/UpdateHiCover, CNPCComponent::ChooseCoverInRange).
// See combat.h.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include "game/combat.h"
#include "audio/audio.h"
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
            p.left = e[8].i != 0, p.right = e[9].i != 0;
            p.jumpable = e[7].i != 0;
            p.jump_dist = glm::length(glm::vec3(m * glm::vec4(-std::sin(a), std::cos(a), 0, 0)));
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
    t.can_dash = ai.size() > 9 && ai[9].i != 0;
    t.can_jump = ai.size() > 11 && ai[11].i != 0;
    t.can_throw = ai.size() > 12 && ai[12].i != 0;
    t.throw_wait_min = field(ai, 39, 5000) / 1000.0f, t.throw_wait_max = field(ai, 40, 7000) / 1000.0f;
    t.throw_reach = field(ai, 46, 20), t.throw_enemy_min = field(ai, 47, 5), t.throw_object_min = field(ai, 48, 5);
    t.throw_drag = field(ai, 50, 2.5f);
    t.throw_wait = t.throw_wait_min;
    t.jump_wait_min = field(ai, 37, 6000) / 1000.0f, t.jump_wait_max = field(ai, 38, 12000) / 1000.0f;
    t.jump_wait = t.jump_wait_min;
    t.dash_wait_min = field(ai, 31, 5000) / 1000.0f, t.dash_wait_max = field(ai, 32, 10000) / 1000.0f;
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
            band.fire = field(b, 13, 100), band.lean = field(b, 14, 0), band.blind = field(b, 15, 0);
            band.peek = field(b, 16, 0), band.roll = field(b, 17, 0), band.grenade = field(b, 18, 0);
            band.crouch_min = field(b, 6, 0) / 1000.0f, band.crouch_max = field(b, 7, 0) / 1000.0f;
            band.jump = field(b, 11, 0), band.throw_chance = field(b, 12, 0);
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

void Combat::start_root_motion(Target& t, const std::string& clip, bool moves, bool reverse, float scale) {
    if (scene_.play_clip(t.object, clip, false, reverse)) t.playing = clip;
    t.root_scale = scale, t.root_vault = false;
    t.root_clip = t.playing == clip ? clip : std::string();
    t.root_moves = moves && !reverse;
    glm::mat4 local;
    if (!t.root_clip.empty() && scene_.sample_node(t.object, "Bip01", clip, 0, local)) t.root_prev = glm::vec3(local[3]);
}

bool Combat::update_root_motion(Target& t) {
    if (t.root_clip.empty()) return false;
    Scene::ClipState cs;
    glm::mat4 local;
    const bool on = t.playing == t.root_clip && scene_.clip_state(t.object, cs);
    if (on && t.root_moves && scene_.sample_node(t.object, "Bip01", t.root_clip, cs.ms, local)) {
        // The root's sideways travel since the last step, through the actor's placement (the
        // scene holds the root itself in place).
        const glm::vec3 now(local[3]);
        const glm::vec3 d = glm::mat3(scene_.object_matrix(t.object)) * glm::vec3(now.x - t.root_prev.x, now.y - t.root_prev.y, 0) * t.root_scale;
        t.root_prev = now;
        t.pos += glm::vec3(d.x, d.y, 0);
        if (!t.root_vault) stay_on_mesh(t);
        scene_.set_object_pose(t.object, t.pos, t.yaw);
    }
    if (on && !scene_.clip_finished(t.object)) return true;
    const bool rolled = t.root_moves && !t.root_vault;
    t.root_clip.clear();
    if (std::exchange(t.root_vault, false)) {   // landed beyond the cover: back on the ground, fighting
        stay_on_mesh(t);
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        if (logic_.trace) log(std::to_string(t.id) + " landed at (" + std::to_string(t.pos.x) + " " + std::to_string(t.pos.y) + ")");
        return false;
    }
    if (const int into = std::exchange(t.root_cover, -1); into >= 0 && t.cover == into) {   // in it, crouched
        if (logic_.trace) {
            char line[128];
            std::snprintf(line, sizeof line, "%d rolled into cover %d, %.1f m from its point", t.id, into,
                          glm::distance(glm::vec2(t.pos), glm::vec2(covers_[size_t(into)].pos)));
            log(line);
        }
        t.cover_phase = Target::CoverPhase::Hidden;
        t.cover_timer = std::uniform_real_distribution<float>(t.idle_in_cover_min, std::max(t.idle_in_cover_max, t.idle_in_cover_min))(rng_);
        set_crouched(t, true);
        play(t, logic_.state_clip(scene_.level().objects[t.object].type, t.cover_left ? "IdleCoverLeft" : "IdleCoverRight"), true);
        return false;
    }
    if (rolled) {
        if (logic_.trace) {
            char line[96];
            std::snprintf(line, sizeof line, "%d rolled to (%.1f %.1f %.1f)", t.id, t.pos.x, t.pos.y, t.pos.z);
            log(line);
        }
        // It ends crouched: there for its band's crouch time, firing (UpdateCrouch), or up at
        // once without one (CrouchExit: CrouchEnter's clip backwards).
        if (const Target::Band* b = band(t); b && b->crouch_max > 0) {
            t.crouch_left = std::uniform_real_distribution<float>(b->crouch_min, std::max(b->crouch_max, b->crouch_min))(rng_);
            if (logic_.trace) log(std::to_string(t.id) + " stays crouched for " + std::to_string(t.crouch_left) + " s");
            set_crouched(t, true);
            return false;
        }
        if (const std::string c = logic_.state_clip(scene_.level().objects[t.object].type, "CrouchExit"); !c.empty()) {
            start_root_motion(t, c, false, true);
            return true;
        }
    }
    return false;
}

bool Combat::roll_into_cover(Target& t, int cover, const glm::vec3& enemy_eye) {
    const CoverPoint& c = covers_[size_t(cover)];
    if (!c.low || t.crouched) return false;
    // The side: the clip that travels towards it (RollLowCoverLeft goes to the actor's
    // right and ends in the left cover posture).
    const glm::vec2 to(c.pos - t.pos);
    const float y = glm::radians(t.yaw);
    const bool to_right = glm::dot(to, glm::vec2(std::cos(y), std::sin(y))) > 0;
    const std::string state = to_right ? "RollLowCoverLeft" : "RollLowCoverRight";
    const std::string clip = logic_.state_clip(scene_.level().objects[t.object].type, state);
    float length = 0;
    glm::mat4 a, b;
    if (clip.empty() || !scene_.sample_node(t.object, "Bip01", clip, 0, a, &length) || !scene_.sample_node(t.object, "Bip01", clip, length, b))
        return false;
    const glm::vec2 travel = glm::vec2(b[3]) - glm::vec2(a[3]);
    const float l = glm::length(travel), d = glm::length(to);
    // About a roll away (0.8 to 1.2 of it), a clear way there, still hidden from the enemy.
    if (l < 1 || d < 0.8f * l || d > 1.2f * l) return false;
    if (!line_clear(t.pos + glm::vec3(0, 0, 0.5f), c.pos + glm::vec3(0, 0, 0.5f)) || !cover_protects(c, enemy_eye)) return false;
    // Turned so that the travel lands on the cover.
    t.yaw = glm::degrees(std::atan2(to.y, to.x) - std::atan2(travel.y, travel.x));
    scene_.set_object_pose(t.object, t.pos, t.yaw);
    t.cover_left = to_right;
    t.cover_phase = Target::CoverPhase::Hidden;
    start_root_motion(t, clip);
    t.root_cover = cover;
    if (logic_.trace) log(std::to_string(t.id) + " rolls into cover " + std::to_string(cover));
    return true;
}

bool Combat::try_dash(Target& t, const glm::vec3& to, bool forced, const glm::vec3* enemy_chest) {
    if (!t.can_dash || t.dashing || !t.mobile || nav_.empty() || (!forced && t.dash_wait > 0)) return false;
    glm::vec3 on;
    if (nav_.nearest(to + glm::vec3(0, 0, 0.3f), 2.0f, on) < 0) return false;
    if (!forced && glm::distance(t.pos, on) < t.dash_min) return false;
    if (!line_clear(t.pos + glm::vec3(0, 0, 0.6f), on + glm::vec3(0, 0, 0.6f))) return false;
    for (const Grenade& g : live_grenades_)   // CAIController::IsPositionSafe
        if (g.live && glm::distance(g.pos, on) < 5.0f) return false;
    if (enemy_chest && !line_clear(on + glm::vec3(0, 0, t.height * 0.85f), *enemy_chest)) return false;
    // StartSuperSpeed: the shift clip, the distortion material, out of harm's way.
    t.dashing = true, t.dash_from = t.pos, t.dash_to = on;
    t.dash_was_invulnerable = t.invulnerable, t.invulnerable = true;
    const glm::vec2 way(on - t.pos);
    t.yaw = glm::degrees(std::atan2(-way.x, way.y));
    const std::string clip = logic_.state_clip(scene_.level().objects[t.object].type, "SuperSpeedStart");
    if (!clip.empty()) play(t, clip, true);
    const bool blurred = scene_.set_object_material(t.object, "Executor_grunt_mat_distort.bdae", "Marine_distort");
    t.dash_blend = -0.3f, t.dash_blend_ms = 200;   // MatsPush, then MatsChangeParam(0, 0.1, 200)
    scene_.set_object_param(t.object, "BlendFactor", glm::vec4(t.dash_blend), 1);
    t.state = Target::State::Chase;
    if (logic_.trace) {
        char line[128];
        std::snprintf(line, sizeof line, "%d dashes to (%.1f %.1f %.1f)%s", t.id, on.x, on.y, on.z, blurred ? "" : " (no blur material)");
        log(line);
    }
    return true;
}

bool Combat::try_dash_aside(Target& t, const glm::vec3& enemy_feet, const glm::vec3& enemy_chest) {
    // TrySSRandom: dash_side to the left or the right of the way to the enemy.
    if (!t.can_dash || t.dash_wait > 0) return false;
    const glm::vec2 d = glm::normalize(glm::vec2(enemy_feet - t.pos) + glm::vec2(1e-4f, 0));
    const float side = std::uniform_int_distribution<int>(0, 1)(rng_) ? 1.0f : -1.0f;
    const glm::vec3 to = t.pos + glm::vec3(d.y, -d.x, 0) * t.dash_side * side;
    return try_dash(t, to, false, &enemy_chest);
}

void Combat::update_dash(Target& t, float dt) {
    // UpdateSuperSpeed_: straight there at dash_speed, then the cooldown.
    if (t.dash_blend_ms > 0) {   // towards 0.1 by a thousandth of the gap per ms, for 200 ms
        t.dash_blend += (0.1f - t.dash_blend) * std::min(dt, 1.0f);
        t.dash_blend_ms -= dt * 1000.0f;
        scene_.set_object_param(t.object, "BlendFactor", glm::vec4(t.dash_blend), 1);
    }
    const glm::vec3 d = t.dash_to - t.pos;
    const float step = t.dash_speed * dt, len = glm::length(d);
    if (len > step) {
        t.pos += d / len * step;
        scene_.set_object_pose(t.object, t.pos, t.yaw);
        return;
    }
    t.pos = t.dash_to, t.tri = -1;
    t.dashing = false, t.invulnerable = t.dash_was_invulnerable;
    t.dash_wait = std::uniform_real_distribution<float>(t.dash_wait_min, std::max(t.dash_wait_max, t.dash_wait_min))(rng_);
    scene_.set_object_material(t.object, "", "");
    const std::string clip = logic_.state_clip(scene_.level().objects[t.object].type, "SuperSpeedStop");
    play(t, clip.empty() ? t.clip_alert : clip, false);
    scene_.set_object_pose(t.object, t.pos, t.yaw);
}

void Combat::load_jump_points() {
    jump_points_.clear();
    std::unordered_map<int, size_t> by_id;
    std::vector<int> next;
    for (const LevelObject& o : scene_.level().objects) {
        if (o.kind != "waypoint" || o.fields.size() < 10 || o.fields[9].i == 0) continue;
        by_id[o.fields[0].i] = jump_points_.size();
        jump_points_.push_back({o.fields[0].i, -1, {o.fields[2].as_float(), o.fields[3].as_float(), o.fields[4].as_float()}});
        next.push_back(o.fields[5].i);
    }
    for (size_t i = 0; i < jump_points_.size(); ++i)
        if (auto it = by_id.find(next[i]); it != by_id.end()) jump_points_[i].landing = int(it->second);
}

bool Combat::try_jump(Target& t, const glm::vec3& enemy_feet, const glm::vec3& enemy_chest) {
    // Every few seconds, by the band's chance: the nearest take-off point within reach whose
    // landing is in the band's range of the enemy, safe, and shows it; walked to if need be,
    // then the leap (StartJumpToWaypoint).
    if (!t.can_jump || jump_points_.empty() || t.jump_wait > 0) return false;
    t.jump_wait = std::uniform_real_distribution<float>(t.jump_wait_min, std::max(t.jump_wait_max, t.jump_wait_min))(rng_);
    const Target::Band* b = band(t);
    if (!b || std::uniform_real_distribution<float>(0.0f, 100.0f)(rng_) >= b->jump) return false;
    const JumpPoint* from = nullptr;
    const JumpPoint* to = nullptr;
    float best = t.jump_reach;
    for (const JumpPoint& p : jump_points_) {
        if (p.landing < 0) continue;
        const float d = glm::distance(t.pos, p.pos);
        if (d > best || std::abs(p.pos.z - t.pos.z) > 2.0f || !line_clear(t.pos + glm::vec3(0, 0, 0.5f), p.pos + glm::vec3(0, 0, 0.5f))) continue;
        const JumpPoint& l = jump_points_[size_t(p.landing)];
        const float r = glm::distance(l.pos, enemy_feet);
        if (r < b->range_min || r > b->range_max) continue;
        bool safe = true;
        for (const Grenade& g : live_grenades_) safe = safe && !(g.live && glm::distance(g.pos, l.pos) < 5.0f);
        if (!safe || !line_clear(l.pos + glm::vec3(0, 0, t.height * 0.85f), enemy_chest)) continue;
        best = d, from = &p, to = &l;
    }
    if (!from) return false;
    if (best > t.jump_walk) {
        NpcGoal walk;
        walk.kind = NpcGoal::Kind::Move, walk.pos = from->pos, walk.id = -1;
        t.goals.push_back(walk);
    }
    NpcGoal leap;
    leap.kind = NpcGoal::Kind::Jump, leap.pos = to->pos, leap.id = -1;
    t.goals.push_back(leap);
    if (logic_.trace) log(std::to_string(t.id) + " jumps from jump point " + std::to_string(from->id) + " to " + std::to_string(to->id));
    return true;
}

bool Combat::try_throw(Target& t, const glm::vec3& enemy_feet) {
    if (!t.can_throw || t.held >= 0 || t.throw_wait > 0) return false;
    t.throw_wait = std::uniform_real_distribution<float>(t.throw_wait_min, std::max(t.throw_wait_max, t.throw_wait_min))(rng_);
    const Target::Band* b = band(t);
    if (!b || std::uniform_real_distribution<float>(0.0f, 100.0f)(rng_) >= b->throw_chance) return false;
    if (glm::distance(t.pos, enemy_feet) < t.throw_enemy_min) return false;
    // CNPCComponent::CanThrowObject: an object in its zone, within reach and roughly ahead
    // (cosine 0.408), far enough from the enemy, whose way from where it would be held (the
    // drag distance ahead, raised) to the enemy is clear.
    const glm::vec3 ahead(-std::sin(glm::radians(t.yaw)), std::cos(glm::radians(t.yaw)), 0);
    const int zone = scene_.zones().zone_at(t.pos + glm::vec3(0, 0, 1));
    std::vector<int> choices;   // a random one of those that fit
    for (size_t i = 0; i < targets_.size(); ++i) {
        const Target& o = targets_[i];
        if (!o.throwable || o.flight || !alive(o)) continue;
        if (const LevelLogic::Object* lo = logic_.object(o.id); !lo || !lo->enabled || !lo->visible) continue;
        if (scene_.zones().zone_at(o.pos + glm::vec3(0, 0, 0.5f)) != zone) continue;
        const glm::vec3 to = o.pos - t.pos;
        if (glm::length(to) > t.throw_reach || glm::distance(o.pos, enemy_feet) < t.throw_object_min) continue;
        if (glm::dot(glm::normalize(to), ahead) < 0.408f) continue;
        if (!line_clear(t.pos + ahead * t.throw_drag + glm::vec3(0, 0, o.throw_lift + t.height * 0.5f), enemy_feet + glm::vec3(0, 0, 1)))
            continue;
        choices.push_back(int(i));
    }
    if (choices.empty()) return false;
    const int pick = choices[size_t(std::uniform_int_distribution<int>(0, int(choices.size()) - 1)(rng_))];
    Target& o = targets_[size_t(pick)];
    t.held = pick, t.throw_stage = 0, t.throw_timer = 0;
    o.flight = 4, o.flung = false, o.thrower = int(&t - targets_.data());
    // ThrowAttackStart; its GameEvent 35 (at once) puts out the tentacle with the telekinesis loop.
    play_state(t, "ThrowAttackStart", false);
    t.tentacle = true, t.tentacle_len = 0;
    if (logic_.audio && !t.telekinesis_sound.empty()) {
        Audio::Params p;
        p.position = &t.pos;
        t.tk_voice = logic_.audio->play(t.telekinesis_sound, p);
    }
    if (logic_.trace) log(std::to_string(t.id) + " lifts " + std::to_string(o.id));
    return true;
}

void Combat::drop_held(Target& t) {   // DropTrownObject: it falls where it was
    if (t.held >= 0 && size_t(t.held) < targets_.size()) {
        Target& o = targets_[size_t(t.held)];
        if (o.flight == 4) o.flight = 0;
        else if (o.flight == 1 || o.flight == 2) o.flight = 3, o.flung = false, o.flight_vel = glm::vec3(0);
    }
    remove_tentacle(t);
    t.held = -1;
}

void Combat::remove_tentacle(Target& t) {   // RemoveTentacle
    for (size_t part : t.tentacle_parts) scene_.set_object_visible(part, false);
    t.tentacle = false;
    if (t.tk_voice >= 0 && logic_.audio) logic_.audio->stop(t.tk_voice, 200);
    t.tk_voice = -1;
}

// CTentacleAIComponent::Update: links laid end to end from the grab node towards `to`, the
// reach growing at the tentacle's speed up to the distance; the last link cut short.
void Combat::update_tentacle(Target& t, float dt, const glm::vec3& to) {
    if (tentacle_part_type_ == -2) {
        tentacle_part_type_ = -1;
        for (const auto& [id, type] : gol_)
            if (type.name == "TentaclePart") tentacle_part_type_ = id;
    }
    if (tentacle_part_type_ < 0) return;
    const glm::vec3 from(scene_.object_matrix(t.object, t.grab_node)[3]);
    const float dist = glm::distance(from, to);
    float speed = 60;
    for (const auto& [id, type] : gol_)
        if (type.name == "Tentacle")
            if (const Value* v = resolved_template(type, gol_, 0x42); v && v->size() > 0) speed = (*v)[0].as_float();
    t.tentacle_len = std::min(t.tentacle_len + speed * dt, dist);
    auto part = [&](size_t n) {
        while (t.tentacle_parts.size() <= n) {
            const size_t obj = scene_.spawn_object(tentacle_part_type_, -7000 - tentacle_ids_++, from, 0);
            scene_.play_clip(obj, "AUTO_SceneAnims_Range", true);
            if (tentacle_link_ <= 0)
                tentacle_link_ = std::max(glm::distance(glm::vec3(scene_.object_matrix(obj, "start")[3]), glm::vec3(scene_.object_matrix(obj, "end")[3])), 0.1f);
            t.tentacle_parts.push_back(obj);
        }
        return t.tentacle_parts[n];
    };
    part(0);
    const float link = tentacle_link_;
    const size_t n = size_t(std::max(1.0f, std::ceil(t.tentacle_len / link)));
    // The links point down -Y from their start: turned onto the way to the object.
    const glm::vec3 dir = dist > 1e-3f ? (to - from) / dist : glm::vec3(0, 1, 0);
    const glm::mat4 turn = glm::mat4_cast(glm::rotation(glm::vec3(0, -1, 0), dir));
    for (size_t i = 0; i < std::max(n, t.tentacle_parts.size()); ++i) {
        const size_t obj = part(i);
        if (i >= n) {
            scene_.set_object_visible(obj, false);
            continue;
        }
        const float length = std::min(link, t.tentacle_len - float(i) * link);
        scene_.set_object_visible(obj, length > 0.01f);
        scene_.set_object_transform(obj, glm::translate(glm::mat4(1), from + dir * (float(i) * link)) * turn *
                                             glm::scale(glm::mat4(1), glm::vec3(1, std::max(length / link, 0.01f), 1)));
    }
    if (t.tk_voice >= 0 && logic_.audio) logic_.audio->set_position(t.tk_voice, t.pos);
}

void Combat::update_throw(Target& t, float dt, const glm::vec3& enemy_chest) {
    // Reached for and pulled in (ThrowAttackStart, then ThrowAttackIdle), then ThrowAttack
    // lets go at its GameEvent 10 (309 ms), then ThrowAttackEnd.
    Target& o = targets_[size_t(t.held)];
    if (!alive(o) || o.flight == 0) {
        remove_tentacle(t);
        t.held = -1;
        return;
    }
    face(t, enemy_chest, dt);
    t.throw_timer += dt;
    const glm::vec3 grab(scene_.object_matrix(t.object, t.grab_node)[3]);
    if (t.tentacle) update_tentacle(t, dt, o.pos + glm::vec3(0, 0, 0.5f));
    switch (t.throw_stage) {
        case 0:   // reaching, then pulling (UpdateUpperBodyThrow)
            if (scene_.clip_finished(t.object)) play_state(t, "ThrowAttackIdle", true);
            if (o.flight == 4 && t.tentacle_len >= glm::distance(grab, o.pos + glm::vec3(0, 0, 0.5f)) - 1e-3f) {
                // The tentacle is there: CPhysicsObjectComponent::Grab pulls it in.
                o.flight = 1, o.flight_from = o.pos, o.flight_t = 0;
                o.flight_len = 0.3f + glm::distance(o.pos, grab) * 0.05f;   // pulled at about 20 m/s
            }
            if (o.flight == 2) {
                play_state(t, "ThrowAttack", false);
                t.throw_stage = 1, t.throw_timer = 0;
            }
            break;
        case 1:   // throwing
            if (t.throw_timer >= 0.309f) {
                // CPhysicsObjectComponent::Throw: across at its speed, up enough to land on the target.
                const glm::vec3 d = enemy_chest - o.pos;
                const float across = std::max(glm::length(glm::vec2(d)), 0.1f), time = across / o.throw_speed;
                o.flight_vel = glm::vec3(glm::vec2(d) / across * o.throw_speed, d.z / time + 0.5f * 9.8f * time);
                o.flight = 3, o.flung = true, o.flight_t = 0;
                remove_tentacle(t);
                sound(t.telekinesis_end_sound, &t.pos);
                t.throw_stage = 2;
                if (logic_.trace) log(std::to_string(t.id) + " throws " + std::to_string(o.id));
            }
            break;
        default:
            if (scene_.clip_finished(t.object)) {
                play_state(t, "ThrowAttackEnd", false);
                t.held = -1;
            }
            break;
    }
}

void Combat::update_flight(Target& o, float dt) {
    o.flight_t += dt;
    const Target* t = o.thrower >= 0 && size_t(o.thrower) < targets_.size() ? &targets_[size_t(o.thrower)] : nullptr;
    const bool still_held = t && alive(*t) && t->held == int(&o - targets_.data());
    if (o.flight == 4) {   // the tentacle reaching for it: where it is
        if (!still_held) o.flight = 0;
        return;
    }
    if (o.flight == 1 || o.flight == 2) {   // pulled to the thrower's grab node, held there
        if (!still_held) {
            o.flight = 3, o.flung = false, o.flight_vel = glm::vec3(0);
            return;
        }
        const glm::vec3 grab(scene_.object_matrix(t->object, t->grab_node)[3]);
        const float k = std::min(o.flight_t / o.flight_len, 1.0f);
        o.pos = glm::mix(o.flight_from, grab - glm::vec3(0, 0, 0.5f), k * k * (3 - 2 * k));
        if (k >= 1) o.flight = 2;
    } else {   // flying: under gravity until it meets something, then it goes off
        const glm::vec3 from = o.pos;
        o.flight_vel.z -= 9.8f * dt;
        o.pos += o.flight_vel * dt;
        const glm::vec3 c = o.pos + glm::vec3(0, 0, 0.5f);
        const bool landed = !line_clear(from + glm::vec3(0, 0, 0.5f), c) ||
                            glm::distance(c, player_center()) < player_.radius.z + 0.5f || o.flight_t > 30;
        if (landed && o.flung) {
            o.flight = 0;
            if (LevelLogic::Object* lo = logic_.object(o.id)) lo->health = 0;
            apply_damage(o, 1e6f, false);
        } else if (landed) {   // dropped: it comes to rest where it met the ground
            o.flight = 0, o.pos = from;
        }
    }
    scene_.set_object_pose(o.object, o.pos, scene_.level().objects[o.object].base ? scene_.level().objects[o.object].base->rotation.z : 0.0f);
    if (LevelLogic::Object* lo = logic_.object(o.id)) lo->position = o.pos, logic_.move_effects(*lo);
}

// CNPCComponent::ExitLowCoverStand / CheckJumpOverCover: out of a cover one can jump, over
// it (JumpOverCoverFromCoverLeft/Right), its root's travel scaled to the cover's depth.
bool Combat::try_vault(Target& t, const CoverPoint& c) {
    if (!c.low || !c.jumpable || t.cover_phase == Target::CoverPhase::Moving) return false;
    const std::string clip = logic_.state_clip(scene_.level().objects[t.object].type,
                                               t.cover_left ? "JumpOverCoverFromCoverLeft" : "JumpOverCoverFromCoverRight");
    glm::mat4 a, b;
    float length = 0;
    if (clip.empty() || !scene_.sample_node(t.object, "Bip01", clip, 0, a, &length) || !scene_.sample_node(t.object, "Bip01", clip, length, b))
        return false;
    const float travel = glm::length(glm::vec2(b[3]) - glm::vec2(a[3]));
    if (travel < 0.1f) return false;
    leave_cover(t);
    t.cover_eval = std::uniform_real_distribution<float>(3.0f, 5.0f)(rng_);
    t.yaw = glm::degrees(std::atan2(-c.dir.x, c.dir.y));
    start_root_motion(t, clip, true, false, c.jump_dist / travel);
    t.root_vault = true;
    if (logic_.trace) log(std::to_string(t.id) + " vaults over its cover (" + clip + ")");
    return true;
}

void Combat::leave_cover(Target& t) {
    if (t.cover >= 0 && size_t(t.cover) < covers_.size()) covers_[size_t(t.cover)].holder = -1;
    t.cover = -1;
    t.cover_next.clear();
    t.root_cover = -1;
    set_crouched(t, false);
    if (t.token) t.token = false;
}

bool Combat::fight_from_cover(Target& t, float dt, const glm::vec3& target, const glm::vec3& chest, bool sees, bool on_player) {
    const float dist = glm::distance(glm::vec2(t.pos), glm::vec2(target));
    const bool in_cover = t.cover >= 0 && t.cover_phase != Target::CoverPhase::Moving;
    update_aggressiveness(t, dt, dist, sees, in_cover);
    if (update_root_motion(t)) return true;   // rolling out of cover
    if (t.crouch_left > 0) {   // crouched in the open: firing from there, then up
        face(t, target, dt);
        if (t.reload_left <= 0) play(t, logic_.state_clip(scene_.level().objects[t.object].type, "CrouchAttack"), true);
        attack_fire(t, chest, on_player, sees, dt);
        if ((t.crouch_left -= dt) <= 0 && t.reload_left <= 0) {
            set_crouched(t, false);
            if (const std::string c = logic_.state_clip(scene_.level().objects[t.object].type, "CrouchExit"); !c.empty())
                start_root_motion(t, c, false, true);
        }
        return true;
    }
    if (!t.use_covers || !t.ranged || !t.mobile || t.hold || t.has_region || nav_.empty() || t.tri < 0 || covers_.empty()) return false;
    const glm::vec3 enemy_eye = chest + glm::vec3(0, 0, 0.3f);
    // The cover still hides it? Flanked, or the enemy upon it: out (CheckPositionIsSafe).
    if (t.cover >= 0) {
        const CoverPoint& c = covers_[size_t(t.cover)];
        if (glm::distance(glm::vec2(c.pos), glm::vec2(target)) < 4.0f || !cover_protects(c, enemy_eye)) {
            if (try_vault(t, c)) return true;
            leave_cover(t), t.cover_eval = 0;
        }
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
        if (roll_into_cover(t, c, enemy_eye)) return true;
        if (try_dash(t, covers_[size_t(c)].pos, false)) return true;   // CNPCComponent::TakeCover
    }
    const CoverPoint& c = covers_[size_t(t.cover)];
    const Target::Band* b = band(t);
    const int type = scene_.level().objects[t.object].type;
    auto clip = [&](const std::string& state) { return logic_.state_clip(type, state); };
    const std::string side = t.cover_left ? "Left" : "Right";
    // A transition clip, then the one it leads to (CNPCComponent::StartTransition); without
    // the first, straight to the second.
    auto transition = [&](const std::string& from, const std::string& to, bool loop) {
        const std::string first = clip(from), then = clip(to);
        if (first.empty()) {
            play(t, then.empty() ? t.clip_alert : then, loop);
            t.cover_next.clear();
            return;
        }
        play(t, first, false);
        t.cover_from = first, t.cover_next = then.empty() ? t.clip_alert : then, t.cover_next_loop = loop;
    };
    if (!t.cover_next.empty() && (t.playing != t.cover_from || scene_.clip_finished(t.object)))   // done (or cut short)
        play(t, t.cover_next, t.cover_next_loop), t.cover_next.clear();
    const bool settling = !t.cover_next.empty();
    const std::string idle = c.low ? "IdleCover" + side : "HighCoverIdle";
    auto hide_for = [&]() {
        return std::uniform_real_distribution<float>(t.idle_in_cover_min, std::max(t.idle_in_cover_max, t.idle_in_cover_min))(rng_);
    };
    switch (t.cover_phase) {
        case Target::CoverPhase::Moving: {
            if (glm::distance(glm::vec2(t.pos), glm::vec2(c.pos)) > 0.6f) {
                go_towards(t, c.pos, 0.4f, dt);
                if (t.path_next >= t.path.size() && glm::distance(glm::vec2(t.pos), glm::vec2(c.pos)) > 2.0f) {   // no way there
                    leave_cover(t);
                    return false;
                }
                return true;
            }
            // CheckLowCoverOrientation: the side towards the enemy (beyond 15 degrees either
            // way of the cover's facing), else whichever side the cover opens on.
            const glm::vec2 to(target - c.pos);
            const float ang = glm::degrees(std::atan2(c.dir.x * to.y - c.dir.y * to.x, c.dir.x * to.x + c.dir.y * to.y));
            if (ang < -15.0f) t.cover_left = false;   // the enemy to its right
            else if (ang > 15.0f) t.cover_left = true;
            else {   // ahead: a side the cover opens on, the left before the right
                if (ang < 5.0f && c.right) t.cover_left = false;
                if (ang > -5.0f && c.left) t.cover_left = true;
            }
            t.cover_phase = Target::CoverPhase::Hidden;
            t.cover_timer = hide_for();
            if (c.low) set_crouched(t, true);
            transition(c.low ? "EnterCover" + std::string(t.cover_left ? "Left" : "Right") : "HighCoverEnter",
                       c.low ? "IdleCover" + std::string(t.cover_left ? "Left" : "Right") : "HighCoverIdle", true);
            return true;
        }
        case Target::CoverPhase::Hidden: {
            face(t, target, dt);
            if (t.throw_at >= 0) {   // a grenade on its way out of the hand (its clip playing)
                if ((t.throw_at -= dt) < 0) npc_throw_grenade(t, target);
                return true;
            }
            if (settling || (t.cover_timer -= dt) > 0) return true;
            // ReloadInLowCover: under five shots left, reloaded hidden (its state has no clip).
            if (c.low && need_reload(t, 5)) {
                refill(t);
                if (logic_.trace) log(std::to_string(t.id) + " reloads in cover");
            }
            // UpdateLowCover state 0 (CFuzzyActions): fire (rising), lean out, blind fire
            // (the enemy in front of the cover), peek, or throw a grenade (when the AI
            // controller's grenade ticket is free, within its throw range, if it has any), by
            // the band's weights.
            const float d = glm::distance(t.pos, target);
            const bool front = glm::dot(c.dir, glm::normalize(target - c.pos + glm::vec3(0, 0, 1e-4f))) > 0.5f;
            // Rolling out (CheckExitLowCoverRoll): from a low cover, out of a side it opens on.
            const bool can_roll = c.low && (t.cover_left ? c.left : c.right);
            const float w[6] = {b ? b->fire : 100.0f, b ? b->lean : 0.0f, b && front ? b->blind : 0.0f, b ? b->peek : 0.0f,
                                b && t.grenades > 0 && grenade_ticket_ < 0 && d >= t.grenade_min && d <= t.grenade_max ? b->grenade : 0.0f,
                                b && can_roll ? b->roll : 0.0f};
            float total = 0;
            for (float x : w) total += x;
            float roll = std::uniform_real_distribution<float>(0.0f, std::max(total, 1e-3f))(rng_);
            int act = 0;
            while (act < 5 && roll >= w[act]) roll -= w[act], ++act;
            if (act == 5) {   // ExitLowCoverRoll: out of the cover, then fighting in the open
                const std::string out = clip("LowCoverRoll" + side);
                if (!out.empty()) {
                    leave_cover(t);
                    t.cover_eval = std::uniform_real_distribution<float>(3.0f, 5.0f)(rng_);
                    start_root_motion(t, out);
                    if (logic_.trace) {
                        char line[160];
                        std::snprintf(line, sizeof line, "%d rolls out of cover (%s) from (%.1f %.1f %.1f), the enemy at (%.1f %.1f)", t.id,
                                      side.c_str(), t.pos.x, t.pos.y, t.pos.z, target.x, target.y);
                        log(line);
                    }
                    return true;
                }
                act = 0;
            }
            if (act == 4) {   // the grenade (ThrowGrenadeFromCover takes the ticket)
                grenade_ticket_ = kGrenadeTicketWait;
                const std::string from_cover = c.low ? clip("LowCover" + side + "ThrowGrenade") : std::string();
                transition(from_cover.empty() ? "AttackGrenade" : "LowCover" + side + "ThrowGrenade", idle, true);
                t.throw_at = 0.6f;
                t.cover_timer = hide_for();
                return true;
            }
            if (act == 3) {   // a look over the top, then down again
                transition(c.low ? "LowCover" + side + "Peek" : "HighCoverLean" + side + "Peek", idle, true);
                t.cover_timer = std::uniform_real_distribution<float>(0.5f, 1.5f)(rng_);
                return true;
            }
            // Out to fire needs one of the attack tokens at the player (else wait a moment).
            int shooters = 0;
            for (const Target& o : targets_) shooters += o.token && &o != &t && alive(o);
            if (on_player && shooters >= max_shooters_) {
                t.cover_timer = 0.4f;
                return true;
            }
            t.cover_act = act == 1 ? Target::CoverAct::Lean : act == 2 ? Target::CoverAct::Blind : Target::CoverAct::Fire;
            // UpdateLowCover state 2: attack for attack min + (max - min) x aggressiveness/100.
            t.cover_phase = Target::CoverPhase::Acting;
            t.cover_timer = t.attack_min + (t.attack_max - t.attack_min) * t.aggr * 0.01f;
            t.fire_timer = std::min(t.fire_timer, 0.3f);
            if (!c.low) transition("HighCoverLean" + side + "ToFire", "HighCoverLean" + side + "Attack", true);
            else if (t.cover_act == Target::CoverAct::Lean) transition("EnterLowCoverLean" + side, "LowCoverLean" + side + "Fire", true);
            else if (t.cover_act == Target::CoverAct::Blind) transition("EnterLowCoverBlindFire" + side, "LowCoverBlindFire" + side, true);
            else transition("EnterLowCoverFire" + side, "LowCoverFire", true);
            set_crouched(t, c.low && t.cover_act == Target::CoverAct::Blind);   // blind: still behind it
            return true;
        }
        case Target::CoverPhase::Acting: {
            face(t, target, dt);
            if (settling) return true;   // coming out
            if (b) t.burst_pause = std::uniform_real_distribution<float>(b->pause_min, std::max(b->pause_max, b->pause_min))(rng_);
            if (t.cover_act == Target::CoverAct::Blind) {   // over the top unseen: anywhere near
                std::uniform_real_distribution<float> u(-1.5f, 1.5f);
                attack_fire(t, chest + glm::vec3(u(rng_), u(rng_), u(rng_) * 0.5f), on_player, false, dt);
            } else {
                attack_fire(t, chest, on_player, sees, dt);
            }
            if (need_reload(t, 1) && t.reload_left <= 0) t.cover_timer = 0, t.shots_left = 0;   // out of rounds: down to reload
            if ((t.cover_timer -= dt) <= 0 && t.shots_left <= 0) {   // back down
                t.token = false;
                t.cover_phase = Target::CoverPhase::Hidden;
                t.cover_timer = hide_for();
                if (c.low) set_crouched(t, true);
                if (!c.low) transition("HighCoverLean" + side + "Back", idle, true);
                else if (t.cover_act == Target::CoverAct::Lean) transition("ExitLowCoverLean" + side, idle, true);
                else if (t.cover_act == Target::CoverAct::Blind) transition("ExitLowCoverBlindFire" + side, idle, true);
                else transition("ExitLowCoverFire" + side, idle, true);
            }
            return true;
        }
    }
    return true;
}

}  // namespace nova
