// The first-person hands and weapon (hands_weapons_mesh.bdae, one rig holding every
// weapon): placed so the rig's own camera (Camera01, looking along +Y with +Z up as the
// game's camera does) sits at the player's eye, with only the current weapon's WEAPON_*
// node shown, playing the weapon's state-set clips (idle, walk, sprint, fire, reload,
// switch, and their ironsight versions while aiming; scripts ask for others by name).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

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
        bool aiming = false, running = false;
        bool cockpit = false;        // piloting the mech: its cockpit (Player_Mech_Cockpit) instead
    };
    // SetCrtWeaponState: play a weapon state once (GetDown lowers the weapon until GetUp,
    // Idle and UnAim go back to normal).
    void set_state(const std::string& state);
    // StartJump / StartLanding: a state played once only over the plain idle, walk or aim
    // clips (not over a reload, a switch or a throw).
    void movement_state(const std::string& state) { movement_ = state; }
    // After the level is loaded (it spawns the rig as a runtime object).
    void start();
    void update(float dt, const glm::vec3& eye, float yaw_deg, float pitch_deg, const Input& in);
    // SetStateOnCameraLayer: a state whose clip moves only the rig's camera (the jolts of
    // CPlayerBaseComponent::ReceiveDamage: Hurt_front, IronSight_Hurt_left_heavy, ...),
    // one of its clips at random, over whatever the hands are playing.
    void camera_state(const std::string& state) { camera_request_ = state; }
    // That clip's move of the camera from where it starts, in the view's frame (x right,
    // y forward, z up); identity when none plays. The world is seen through it, the hands
    // stay where the rig's unmoved camera puts them.
    const glm::mat4& camera_offset() const { return camera_offset_; }

private:
    // The weapon's states set (CStatesSetComponent): each state's type, whether it loops,
    // the state that follows it, and its clips with their speeds (negative: backwards,
    // e.g. GetUp is the switch clip reversed).
    struct State {
        std::string name;
        int type = 0;
        bool loop = false;
        int next = -1;
        std::vector<std::pair<std::string, float>> clips;
    };
    struct Clips {
        std::vector<State> states;
        std::unordered_map<std::string, int> index;
        std::string node;                                       // the WEAPON_* node shown for it
        const State* find(const std::string& name) const {
            auto it = index.find(name);
            return it == index.end() ? nullptr : &states[size_t(it->second)];
        }
        bool has(const std::string& name) const {
            const State* s = find(name);
            return s && !s->clips.empty();
        }
        std::string get(const std::string& name) const {   // the first clip
            const State* s = find(name);
            return s && !s->clips.empty() ? s->clips[0].first : std::string();
        }
    };
    const Clips& clips(const std::string& weapon);
    // Play a state (one of its clips at random); false if it has none. One that does not
    // loop holds the others off until it ends (left_), then its next state follows.
    bool play(const Clips& c, const std::string& state);
    void show_weapon(const std::string& node);

    Scene& scene_;
    const Gol& gol_;
    size_t object_ = size_t(-1), cockpit_ = size_t(-1);
    bool cockpit_visible_ = false;
    std::string cockpit_playing_;
    void place(size_t rig, const glm::vec3& eye, float yaw_deg, float pitch_deg);
    std::unordered_map<std::string, Clips> clips_;
    std::string weapon_;
    int current_ = -1;             // the state playing
    float left_ = 0;               // seconds left of a state that does not loop
    std::string base_;             // the looping state the input asks for (Idle, AimWalk, RushWalk, ...)
    bool visible_ = false;
    bool lowered_ = false;         // GetDown: held lowered until GetUp
    bool was_reloading_ = false;
    std::string requested_, queued_;   // queued_: a state to play after the current one
    std::string movement_;
    std::string camera_request_, camera_clip_;
    float camera_ms_ = 0;
    glm::mat4 camera_offset_{1};
    void update_camera_layer(float dt, const Clips& c);
};

}  // namespace nova
