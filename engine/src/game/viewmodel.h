// The first-person hands and weapon (hands_weapons_mesh.bdae, one rig holding every
// weapon): placed so the rig's own camera (Camera01, looking along +Y with +Z up as the
// game's camera does) sits at the player's eye, with only the current weapon's WEAPON_*
// node shown, playing the weapon's state-set clips (idle, run, fire, reload, switch).
#pragma once

#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

#include "assets/level.h"

namespace nova {

class Scene;

class ViewModel {
public:
    ViewModel(Scene& scene, const Gol& gol) : scene_(scene), gol_(gol) {}

    struct Input {
        std::string weapon;          // gol type, e.g. AssaultRifle_Player
        bool visible = true;
        bool moving = false, firing = false, reloading = false;
    };
    // After the level is loaded (it spawns the rig as a runtime object).
    void start();
    void update(float dt, const glm::vec3& eye, float yaw_deg, float pitch_deg, const Input& in);

private:
    struct Clips {
        std::string idle, walk, fire, reload, show, node;   // node: WEAPON_* shown for it
    };
    const Clips& clips(const std::string& weapon);
    void show_weapon(const std::string& node);

    Scene& scene_;
    const Gol& gol_;
    size_t object_ = size_t(-1);
    std::unordered_map<std::string, Clips> clips_;
    std::string weapon_, playing_;
    bool visible_ = false;
    float switch_time_ = 0;
};

}  // namespace nova
