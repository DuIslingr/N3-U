#include "game/viewmodel.h"

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
            const std::string& name = st[0].s;
            const std::string& clip = st[11][0][0].s;
            if (name == "Idle") c.idle = clip;
            else if (name == "Walk") c.walk = clip;
            else if (name == "Attack_Loop") c.fire = clip;
            else if (name == "Reload") c.reload = clip;
            else if (name == "GetWeapon") c.show = clip;
        }
    return c;
}

void ViewModel::start() {
    int type = -1;
    if (!type_named(gol_, "AssaultRifle_Player", &type)) return;
    object_ = scene_.spawn_object(type, -1000, glm::vec3(0), 0);
    scene_.set_object_viewmodel(object_);
    weapon_.clear(), playing_.clear();
    visible_ = false;
}

void ViewModel::show_weapon(const std::string& node) {
    for (const std::string& n : scene_.node_names(object_))
        if (n.rfind("WEAPON_", 0) == 0 && n.find("_PIVOT") == std::string::npos) scene_.set_node_visible(object_, n, n == node);
}

void ViewModel::update(float dt, const glm::vec3& eye, float yaw_deg, float pitch_deg, const Input& in) {
    if (object_ == size_t(-1)) return;
    if (in.visible != visible_) scene_.set_object_visible(object_, in.visible), visible_ = in.visible;
    if (!in.visible) return;
    const Clips& c = clips(in.weapon);
    if (in.weapon != weapon_) {
        weapon_ = in.weapon;
        show_weapon(c.node);
        switch_time_ = 0.6f;
        if (!c.show.empty()) scene_.play_clip(object_, c.show, false), playing_ = c.show;
    }
    switch_time_ = std::max(switch_time_ - dt, 0.0f);
    std::string want = c.idle;
    if (in.reloading && !c.reload.empty()) want = c.reload;
    else if (in.firing && !c.fire.empty()) want = c.fire;
    else if (switch_time_ > 0 && !c.show.empty()) want = c.show;
    else if (in.moving && !c.walk.empty()) want = c.walk;
    if (!want.empty() && want != playing_) {
        scene_.play_clip(object_, want, want != c.reload && want != c.show);
        playing_ = want;
    }
    // The rig's camera (Collada: looking down -Z) turned to look along +Y, in rig space.
    glm::mat4 placement = scene_.object_matrix(object_);
    const glm::mat4 cam_world = scene_.object_matrix(object_, "Camera01");
    const glm::mat4 cam_rig = glm::inverse(placement) * cam_world * glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(1, 0, 0));
    const glm::mat4 view = glm::rotate(glm::rotate(glm::translate(glm::mat4(1), eye), glm::radians(yaw_deg), glm::vec3(0, 0, 1)),
                                       glm::radians(pitch_deg), glm::vec3(1, 0, 0));
    scene_.set_object_transform(object_, view * glm::inverse(cam_rig));
}

}  // namespace nova
