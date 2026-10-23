#include "game/viewmodel.h"

#include <cstdlib>

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
    if (const Value* v = resolved_template(*t, gol_, kCompStatesSet); v && v->size() > 1)
        for (const Value& st : (*v)[1].list) {
            if (st.size() < 12 || st[11].size() == 0 || st[11][0].size() == 0) continue;
            c.states[st[0].s] = st[11][0][0].s;
            for (const Value& clip : st[11].list)
                if (clip.size() > 0 && clip[0].type == Value::String) c.variants[st[0].s].push_back(clip[0].s);
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
    weapon_.clear(), playing_.clear(), requested_.clear(), queued_.clear();
    camera_request_.clear(), camera_clip_.clear(), camera_offset_ = glm::mat4(1);
    visible_ = false, lowered_ = false, was_reloading_ = false;
    once_left_ = 0;
}

void ViewModel::show_weapon(const std::string& node) {
    for (const std::string& n : scene_.node_names(object_))
        if (n.rfind("WEAPON_", 0) == 0 && n.find("_PIVOT") == std::string::npos) scene_.set_node_visible(object_, n, n == node);
}

void ViewModel::set_state(const std::string& state) { requested_ = state; }

void ViewModel::play_once(const std::string& clip, bool reverse) {
    if (clip.empty() || !scene_.play_clip(object_, clip, false, reverse)) return;
    playing_ = clip;
    Scene::ClipState cs;
    once_left_ = scene_.clip_state(object_, cs) ? cs.length / 1000.0f : 0.5f;
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
    once_left_ = std::max(once_left_ - dt, 0.0f);
    if (in.weapon != weapon_) {   // switched: the new weapon comes up
        weapon_ = in.weapon;
        show_weapon(c.node);
        lowered_ = false;
        play_once(c.get("GetUp").empty() ? c.get("GetWeapon") : c.get("GetUp"));
    }
    if (!requested_.empty()) {
        const std::string r = requested_;
        requested_.clear();
        if (r == "GetDown") {
            play_once(c.get("GetDown"), true);   // the switch clip backwards: the weapon goes down
            lowered_ = true;
        } else if (r == "ThrowGrenade") {   // the throw, then back to the weapon
            lowered_ = false;
            play_once(c.get("ThrowGrenade"));
            queued_ = c.get("ThrowGrenade_End");
        } else if (r == "Idle" || r == "UnAim") {
            lowered_ = false, once_left_ = 0;
            if (r == "UnAim") play_once(c.get("UnAim"));
        } else {
            lowered_ = false;
            play_once(c.get(r));
        }
    }
    if (once_left_ <= 0 && !queued_.empty()) play_once(queued_), queued_.clear();
    if (in.reloading && !was_reloading_) play_once(c.get("Reload"));
    was_reloading_ = in.reloading;
    if (once_left_ <= 0 && !lowered_) {
        std::string want;
        if (in.aiming) want = in.firing ? c.get("AimAttack") : in.moving ? c.get("AimWalk") : c.get("AimIdle");
        else if (in.firing) want = c.get("Attack_Loop");
        else if (in.moving) want = in.running && !c.get("RushWalk").empty() ? c.get("RushWalk") : c.get("Walk");
        if (want.empty()) want = c.get("Idle");
        if (!want.empty() && want != playing_) {
            scene_.play_clip(object_, want, true);
            playing_ = want;
        }
    }
    place(object_, eye, yaw_deg, pitch_deg);
}

void ViewModel::update_camera_layer(float dt, const Clips& c) {
    if (!camera_request_.empty()) {
        auto it = c.variants.find(camera_request_);
        camera_request_.clear();
        if (it != c.variants.end() && !it->second.empty()) camera_clip_ = it->second[size_t(std::rand()) % it->second.size()], camera_ms_ = 0;
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
