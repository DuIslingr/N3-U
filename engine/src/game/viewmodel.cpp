#include "game/viewmodel.h"

#include <cmath>
#include <cstdlib>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompStatesSet = 0x0e;
constexpr int kCompWeapon = 0x47;     // [1]: the rig's WEAPON_* node for this weapon

const GolType* type_named(const Gol& gol, const std::string& name, int* id = nullptr) {
    for (const auto& [i, t] : gol)
        if (t.name == name) {
            if (id) *id = i;
            return &t;
        }
    return nullptr;
}

}  // namespace

const ViewModel::Clips& ViewModel::clips(const std::string& weapon) {
    if (auto it = clips_.find(weapon); it != clips_.end()) return it->second;
    Clips& c = clips_[weapon];
    const GolType* t = type_named(gol_, weapon);
    if (!t) return c;
    if (const Value* w = resolved_template(*t, gol_, kCompWeapon); w && w->size() > 1 && (*w)[1].type == Value::String) c.node = (*w)[1].s;
    // Each state: [0] name, [1] type, [2] loops, [6] the next state's index, [11] clips
    // ({name, speed, ...}).
    if (const Value* v = resolved_template(*t, gol_, kCompStatesSet); v && v->size() > 1)
        for (const Value& st : (*v)[1].list) {
            State s;
            if (st.size() > 0 && st[0].type == Value::String) s.name = st[0].s;
            if (st.size() > 6) s.type = int(st[1].as_float()), s.loop = st[2].as_float() != 0, s.next = int(st[6].as_float());
            if (st.size() > 11)
                for (const Value& clip : st[11].list)
                    if (clip.size() > 1 && clip[0].type == Value::String) s.clips.emplace_back(clip[0].s, clip[1].as_float());
            c.index.emplace(s.name, int(c.states.size()));
            c.states.push_back(std::move(s));
        }
    return c;
}

void ViewModel::start() {
    int type = -1;
    if (!type_named(gol_, "AssaultRifle_Player", &type)) return;
    object_ = scene_.spawn_object(type, -1000, glm::vec3(0), 0);
    scene_.set_object_viewmodel(object_);
    cockpit_ = size_t(-1), cockpit_visible_ = false, cockpit_playing_.clear();
    if (type_named(gol_, "Player_Mech_Cockpit", &type)) {
        cockpit_ = scene_.spawn_object(type, -1001, glm::vec3(0), 0);
        scene_.set_object_viewmodel(cockpit_);
        scene_.set_object_visible(cockpit_, false);
    }
    weapon_.clear(), base_.clear(), requested_.clear(), queued_.clear(), movement_.clear();
    camera_request_.clear(), camera_clip_.clear(), camera_offset_ = glm::mat4(1);
    visible_ = false, lowered_ = false, was_reloading_ = false;
    current_ = -1, left_ = 0;
}

void ViewModel::show_weapon(const std::string& node) {
    for (const std::string& n : scene_.node_names(object_))
        if (n.rfind("WEAPON_", 0) == 0 && n.find("_PIVOT") == std::string::npos) scene_.set_node_visible(object_, n, n == node);
}

void ViewModel::set_state(const std::string& state) { requested_ = state; }

bool ViewModel::play(const Clips& c, const std::string& state) {
    auto it = c.index.find(state);
    if (it == c.index.end()) return false;
    const State& s = c.states[size_t(it->second)];
    if (s.clips.empty()) return false;
    const auto& [clip, speed] = s.clips[s.clips.size() > 1 ? size_t(std::rand()) % s.clips.size() : 0];
    const float rate = speed == 0 ? 1.0f : std::fabs(speed);   // 0: the postures, posed by the view's turn
    if (!scene_.play_clip(object_, clip, s.loop, speed < 0)) return false;
    scene_.set_clip_speed(object_, rate);
    current_ = it->second;
    Scene::ClipState cs;
    left_ = s.loop ? 0.0f : (scene_.clip_state(object_, cs) ? cs.length / 1000.0f : 0.5f) / rate;
    return true;
}

void ViewModel::update(float dt, const glm::vec3& eye, float yaw_deg, float pitch_deg, const Input& in) {
    if (object_ == size_t(-1)) return;
    // The mech's cockpit: its own rig and states.
    const bool cockpit = in.visible && in.cockpit && cockpit_ != size_t(-1);
    if (cockpit_ != size_t(-1) && cockpit != cockpit_visible_) scene_.set_object_visible(cockpit_, cockpit), cockpit_visible_ = cockpit;
    if (cockpit) {
        if (visible_) scene_.set_object_visible(object_, false), visible_ = false;
        const Clips& c = clips("Player_Mech_Cockpit");
        std::string want = in.firing ? c.get("Attack") : in.moving ? c.get("CockpitWalk") : c.get("CockpitIdle");
        if (want.empty()) want = c.get("CockpitIdle");
        if (!want.empty() && want != cockpit_playing_) scene_.play_clip(cockpit_, want, true), cockpit_playing_ = want;
        place(cockpit_, eye, yaw_deg, pitch_deg);
        camera_request_.clear(), camera_clip_.clear(), camera_offset_ = glm::mat4(1);
        return;
    }
    if (in.visible != visible_) scene_.set_object_visible(object_, in.visible), visible_ = in.visible;
    if (!in.visible) {
        camera_request_.clear(), camera_clip_.clear(), camera_offset_ = glm::mat4(1);
        return;
    }
    const Clips& c = clips(in.weapon);
    update_camera_layer(dt, c);
    // What is playing: a state that does not loop holds the rest off, except the moves
    // between the input's own states (aiming, sprinting, the fire's start and end, jumps,
    // landings), which the next of those cut short.
    const State* cur = current_ >= 0 && size_t(current_) < c.states.size() ? &c.states[size_t(current_)] : nullptr;
    left_ = std::max(left_ - dt, 0.0f);
    auto soft = [](const State* s) {
        if (!s || s->loop) return true;
        switch (s->type) {
            case 3: case 4: case 10: case 11: case 24: case 26: case 37: case 38: case 42: return true;
            default: return s->name == "Attack";
        }
    };
    if (in.weapon != weapon_) {   // switched: the new weapon comes up
        weapon_ = in.weapon;
        show_weapon(c.node);
        lowered_ = false, queued_.clear();
        if (!play(c, "GetUp")) play(c, "GetWeapon");
    }
    if (!requested_.empty()) {
        const std::string r = std::exchange(requested_, {});
        if (r == "GetDown") {
            lowered_ = play(c, "GetDown");   // then Hidden: held down
        } else if (r == "ThrowGrenade") {   // the throw, then back to the weapon
            lowered_ = false;
            if (play(c, "ThrowGrenade")) queued_ = "ThrowGrenade_End";
        } else if (r == "Idle") {
            lowered_ = false, left_ = 0;
        } else {
            lowered_ = false;
            if (!play(c, r) && r == "UnAim") left_ = 0;
        }
    }
    if (in.reloading && !was_reloading_) play(c, "Reload");
    was_reloading_ = in.reloading;
    cur = current_ >= 0 && size_t(current_) < c.states.size() ? &c.states[size_t(current_)] : nullptr;
    const bool busy = left_ > 0 && !soft(cur);
    if (!movement_.empty()) {
        if (!busy && !lowered_ && !in.reloading) play(c, movement_);
        movement_.clear();
    }

    // The looping state the input asks for, and the move into it from the last one.
    std::string base;
    if (in.aiming) base = in.firing ? "AimAttack" : in.moving ? "AimWalk" : "AimIdle";
    else if (in.firing) base = "Attack_Loop";
    else if (in.moving && in.running && c.has("RushWalk")) base = "RushWalk";
    else base = in.moving ? "Walk" : "Idle";
    if (!c.has(base)) base = in.aiming && c.has("AimIdle") ? "AimIdle" : "Idle";
    if (base != base_ && !base_.empty() && !lowered_ && !busy) {
        const bool was_aim = base_.rfind("Aim", 0) == 0, aim = base.rfind("Aim", 0) == 0;
        std::string move;
        if (aim && !was_aim) move = "Aim";
        else if (was_aim && !aim) move = base == "RushWalk" ? "AimToRush" : "UnAim";
        else if (base == "Attack_Loop") move = "Attack";
        else if (base == "RushWalk") move = "StartRush";
        else if (base_ == "RushWalk") move = "EndRush";
        else if (base_ == "Attack_Loop") move = "Attack_End";
        if (!move.empty()) play(c, move);
    }
    base_ = base;
    cur = current_ >= 0 && size_t(current_) < c.states.size() ? &c.states[size_t(current_)] : nullptr;
    if (lowered_ || (cur && !cur->loop && left_ > 0)) {
        place(object_, eye, yaw_deg, pitch_deg);
        return;
    }
    // A state that ended: a queued one, its next (a chain such as Attack -> Attack_Loop,
    // when that is still wanted), or the input's.
    if (!queued_.empty()) {
        const std::string q = std::exchange(queued_, {});
        if (play(c, q)) {
            place(object_, eye, yaw_deg, pitch_deg);
            return;
        }
    }
    if (cur && !cur->loop && cur->next > 1 && size_t(cur->next) < c.states.size()) {
        const State& n = c.states[size_t(cur->next)];
        if (n.clips.empty()) {   // Hidden
            lowered_ = true;
            place(object_, eye, yaw_deg, pitch_deg);
            return;
        }
        if ((!n.loop || n.name == base) && play(c, n.name)) {
            place(object_, eye, yaw_deg, pitch_deg);
            return;
        }
    }
    if (!cur || cur->name != base || (!cur->loop && left_ <= 0)) play(c, base);
    place(object_, eye, yaw_deg, pitch_deg);
}

void ViewModel::update_camera_layer(float dt, const Clips& c) {
    if (!camera_request_.empty()) {
        const State* s = c.find(camera_request_);
        camera_request_.clear();
        if (s && !s->clips.empty()) camera_clip_ = s->clips[size_t(std::rand()) % s->clips.size()].first, camera_ms_ = 0;
    }
    camera_offset_ = glm::mat4(1);
    if (camera_clip_.empty()) return;
    camera_ms_ += dt * 1000.0f;
    glm::mat4 from, now;
    float length = 0;
    if (!scene_.sample_node(object_, "Camera01", camera_clip_, 0, from, &length) ||
        !scene_.sample_node(object_, "Camera01", camera_clip_, camera_ms_, now) || camera_ms_ >= length) {
        camera_clip_.clear();
        return;
    }
    // In the camera's frame (looking down -Z, +Y up), then turned into the view's.
    const glm::mat4 turn = glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(1, 0, 0));
    camera_offset_ = glm::inverse(turn) * glm::inverse(from) * now * turn;
}

void ViewModel::place(size_t rig, const glm::vec3& eye, float yaw_deg, float pitch_deg) {
    // The rig's camera (Collada: looking down -Z) turned to look along +Y, in rig space.
    glm::mat4 placement = scene_.object_matrix(rig);
    const glm::mat4 cam_world = scene_.object_matrix(rig, "Camera01");
    const glm::mat4 cam_rig = glm::inverse(placement) * cam_world * glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(1, 0, 0));
    const glm::mat4 view = glm::rotate(glm::rotate(glm::translate(glm::mat4(1), eye), glm::radians(yaw_deg), glm::vec3(0, 0, 1)),
                                       glm::radians(pitch_deg), glm::vec3(1, 0, 0));
    scene_.set_object_transform(rig, view * glm::inverse(cam_rig));
}

}  // namespace nova
