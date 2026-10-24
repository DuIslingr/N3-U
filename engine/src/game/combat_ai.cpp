// Combat's NPC tactics: aggressiveness and its bands, cover points and the fight from cover
// (CAIComponent::Update/UpdateLowCover/UpdateHiCover, CNPCComponent::ChooseCoverInRange).
// See combat.h.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

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
            p.left = e[8].i != 0, p.right = e[9].i != 0;
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
            band.fire = field(b, 13, 100), band.lean = field(b, 14, 0), band.blind = field(b, 15, 0);
            band.peek = field(b, 16, 0), band.roll = field(b, 17, 0), band.grenade = field(b, 18, 0);
            band.crouch_min = field(b, 6, 0) / 1000.0f, band.crouch_max = field(b, 7, 0) / 1000.0f;
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

void Combat::start_root_motion(Target& t, const std::string& clip, bool moves, bool reverse) {
    if (scene_.play_clip(t.object, clip, false, reverse)) t.playing = clip;
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
        const glm::vec3 d = glm::mat3(scene_.object_matrix(t.object)) * glm::vec3(now.x - t.root_prev.x, now.y - t.root_prev.y, 0);
        t.root_prev = now;
        t.pos += glm::vec3(d.x, d.y, 0);
        stay_on_mesh(t);
        scene_.set_object_pose(t.object, t.pos, t.yaw);
    }
    if (on && !scene_.clip_finished(t.object)) return true;
    const bool rolled = t.root_moves;
    t.root_clip.clear();
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
        if (roll_into_cover(t, c, enemy_eye)) return true;
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
