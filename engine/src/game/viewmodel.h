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
        bool shot = false;           // a shot went off since the last update
        bool aiming = false, running = false;
        bool cockpit = false;        // piloting the mech: its cockpit (Player_Mech_Cockpit) instead
        float mech_shield = 1;       // the mech's health, 0..1 (HUDShield_Mech in the cockpit)
    };
    // SetCrtWeaponState: play a weapon state once (GetDown lowers the weapon until GetUp,
    // Idle and UnAim go back to normal).
    void set_state(const std::string& state);
    // The weapon state playing now (CWeaponComponent::GetWeaponState), e.g. Interact.
    const std::string& state() const { return playing_; }
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
    // The view's move from where the rig's camera rests (in the weapon's idle clip), in the
    // view's frame (x right, y forward, z up): the hands' own clips move the camera too (the
    // ironsight fire's kick back and up), the hands staying put in front of it; and that
    // clip's move of it from where it starts, the world seen through it while the hands
    // stay where the rig's unmoved camera puts them.
    const glm::mat4& camera_offset() const { return camera_offset_; }
    // The current weapon's field of view aiming (game units), and what the view model's adds
    // to the view's (CWeaponComponent::GetWeaponFovDiff: across Aim and UnAim, from one to the
    // other).
    float aim_fov();
    float model_fov_add();
    // The mech's view is its cockpit camera's (CPlayerMechComponent: 1.29 rad to the hands'
    // 0.87): how much wider than the hands' camera, 1 without the rigs.
    float cockpit_fov_factor() const;
    // The current weapon's zoom range aiming (CLevelInterfaceNormal::EnableZoomSlider); false
    // if it has none.
    bool zoom_range(float& widest, float& narrowest);
    // Looking through the weapon's scope (its Aim clip has hidden it): the HUD draws the
    // scope's crosshair instead.
    bool scoped() const { return weapon_hidden_ && aiming_; }
    // PlayerComponent::distort_push / distort_pop / distort_change_blend: the rig drawn with
    // hand_mesh_new_distort.bdae's armsDistort (the screen behind shows through it where its
    // noise texture plus BlendFactor is under 0.5), BlendFactor from -0.3 eased to `target`
    // over `ms` (a quarter of the way each 30 fps frame, then set).
    void arms_distort(bool on);
    void arms_distort_blend(float target, float ms);
    // A shot (CWeaponComponent::ActivateMuzzleObjects): the weapon's muzzle object (its
    // ironsight one while aiming) shown at the weapon's muzzle node for one of its clips.
    void flash(bool aiming);
    // Where the current weapon's muzzle is in the world (the bullets leave from it); false
    // while no weapon is up.
    bool muzzle(glm::vec3& out) const;

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
        // CComponentWeapon [45]: the view's field of view aiming; [70]/[71]: the view
        // model's, normal and aiming (game units; drawn at the view's plus theirs less 70).
        float aim_fov = 35, model_fov = 70, model_aim_fov = 70;
        // [46]: a zoom slider while aiming (the sniper rifle's), from [47] to [48].
        bool zoom = false;
        float zoom_from = 30, zoom_to = 10;
        // The fire mode's muzzle objects (gol types; -1: none) and the node under `node`
        // they sit at.
        int muzzle_type = -1, aim_muzzle_type = -1;
        std::string muzzle_node;
        // The weapon's effects (CEffectsComponent): name, node under `node` ("-1": the
        // weapon node), whether it runs while the weapon is up.
        struct Fx {
            std::string effect, node;
            bool autostart = false;
        };
        std::vector<Fx> fx;
        // Per clip, its GameEvent track: {ms, event id, argument}.
        struct Event {
            float ms;
            int id, arg;
        };
        std::unordered_map<std::string, std::vector<Event>> events;
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
    bool play(const Clips& c, const std::string& state, float fade_ms = 0);
    void show_weapon(const std::string& node);
    // GameEvent 12 / 13 (the sniper rifle's Aim clip): the weapon out of sight while the
    // view looks through its scope, back on UnAim.
    void set_weapon_hidden(const Clips& c, bool hidden);
    bool weapon_hidden_ = false;

    Scene& scene_;
    const Gol& gol_;
    size_t object_ = size_t(-1), cockpit_ = size_t(-1), mech_shield_ = size_t(-1);
    float mech_shield_shown_ = -1;
    bool cockpit_visible_ = false;
    std::string cockpit_playing_;
    void place(size_t rig, const glm::vec3& eye, float yaw_deg, float pitch_deg);
    std::unordered_map<std::string, Clips> clips_;
    std::string weapon_;
    int current_ = -1;             // the state playing
    std::string playing_;          // its name
    float left_ = 0;               // seconds left of a state that does not loop
    std::string base_;             // the looping state the input asks for (Idle, AimWalk, RushWalk, ...)
    bool visible_ = false;
    bool lowered_ = false;         // GetDown: held lowered until GetUp
    bool aiming_ = false;
    bool was_reloading_ = false, was_firing_ = false;
    std::string requested_, queued_;   // queued_: a state to play after the current one
    std::string movement_;
    std::string camera_request_, camera_clip_;
    float camera_ms_ = 0;
    glm::mat4 camera_offset_{1};
    bool distort_ = false;
    float distort_blend_ = 0, distort_target_ = 0, distort_ms_ = 0;
    void update_camera_layer(float dt, const Clips& c);
    // The muzzle objects, spawned when first fired: per gol type, its object and its clips.
    struct Muzzle {
        size_t object = size_t(-1);
        std::vector<std::string> clips;
    };
    std::unordered_map<int, Muzzle> muzzles_;
    int flashing_ = -1;            // the muzzle type shown
    void place_muzzle();
    // The weapon's running effects (handles per Clips::fx index, -1: off), and how far the
    // clip's events have been played.
    std::vector<int> fx_;
    std::string fx_weapon_;
    uint32_t events_serial_ = 0;
    float events_ms_ = 0;
    void update_events(const Clips& c);
    // The weapon trailing the view (CWeaponManager::UpdateWeaponRotation): how far behind
    // it is (radians: x yaw, positive to the left; y pitch, positive up), where its catch-up
    // started and how much of it is left, shown by the posture clips' turn of the arms.
    glm::vec2 sway_{0}, sway_start_{0};
    float sway_left_ = 0, last_yaw_ = 0, last_pitch_ = 0;
    bool sway_view_ = false, sway_layer_ = false;
    void update_sway(float dt, float yaw_deg, float pitch_deg, bool aiming, const Clips& c);
    void start_fx(const Clips& c, int index);
    void stop_fx(int index);
    void stop_all_fx();
    glm::mat4 fx_matrix(const Clips& c, int index) const;
};

}  // namespace nova
