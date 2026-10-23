// Level logic: the level's game objects as scripts see them (ids, enabled state,
// triggers, doors, interactive objects, spawn points), the Lua level scripts, and the
// game's script API (Enable, Wait, LockDoor, TeleportPlayerToZone, ...).
//
// Scripts are the Lua chunks stored in the level: the level properties' Obj<id>_OnEnter
// runs when the level starts, the start spawn point's Obj<id>_Default when the player
// spawns, triggers run Obj<id>_OnEnter/OnExit/WhileIn/WhileOut (CTriggerComponent),
// interactive objects Obj<id>_OnEnter when used.
#pragma once

#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"
#include "assets/strings.h"
#include "render/renderer.h"
#include "game/scene.h"
#include "script/lua_runtime.h"

namespace nova {

class Scene;
class Player;
class Combat;
class Audio;

class LevelLogic {
public:
    LevelLogic(Scene& scene, Player& player, const Gol& gol);
    ~LevelLogic();

    // Load the scripts and objects of the scene's current level and run its start
    // scripts. Call after Scene::load_level and spawning the player. `keep_player`:
    // a section reload, where the player stays put and the start spawn's script does not run.
    void start(bool keep_player = false);
    // Advance scripts, triggers and doors by `dt` seconds.
    void update(float dt);
    // The player pressed "use": interact with the nearest usable object, if any.
    bool use();
    // Run a Lua snippet as a script (with the game API), e.g. for testing.
    bool exec(const std::string& code);

    // Script messages (ShowMessage, print, errors), for the HUD/console.
    struct Message {
        std::string id, text;        // the script's id, and its text (or the id when untranslated)
        float seconds_left = 0;
    };
    StringTable strings;             // MAIN and the level's table (English)
    const std::deque<Message>& messages() const { return messages_; }
    std::function<void(const std::string&)> log = [](const std::string&) {};
    bool trace = false;              // log every started script function
    bool level_ended = false;        // EndLevel() was called
    bool fog_locked = false;         // Fog_SetLock: zone changes keep the current fog
    // LoadLevelSections(n): the level should reload with section bit n (0: none pending).
    // The caller reloads the scene and calls start(true).
    uint32_t pending_section = 0;


    struct Stats {
        size_t chunks = 0, chunk_errors = 0, objects = 0, triggers = 0, doors = 0, interactive = 0;
    };
    const Stats& stats() const { return stats_; }
    const ScriptRuntime& runtime() const { return runtime_; }

    // State used by the script API (script/game_api.cpp).
    struct Trigger {
        glm::vec3 min{0}, max{0};    // box, or the cylinder's bounding box
        bool cylinder = false;
        glm::vec2 center{0};
        float radius = 0;
        int runs_left = -1;          // -1: unlimited
        bool player_target = true;
        bool inside = false;
    };
    struct Door {
        enum class State { Closed, Opening, Open, Closing } state = State::Closed;
        float radius = 10;
        bool locked = false, keep_unlocked = false, forced_open = false;
        std::string unlock_light, lock_light;   // nodes shown while unlocked / locked (CDoorAIComponent::SetDoorLocked)
    };
    struct Object {
        int id = 0;
        size_t index = std::numeric_limits<size_t>::max();   // into Level::objects
        std::string type;            // object type name, or the level-structure kind
        glm::vec3 position{0};
        float yaw = 0;               // degrees
        bool enabled = true, visible = true, dead = false;
        bool interactive = false, interact_enabled = true;
        float health = 100;
        std::string state;
        int trigger = -1, door = -1;
        // Particle effects: CEffectBuiltInComponent's one effect, or CEffectsComponent's
        // list (effect, node it is attached to, started with the object).
        struct EffectSlot {
            std::string effect, node;
            bool autostart = false;
            int handle = -1;
        };
        std::vector<EffectSlot> effects;
    };
    Object* object(int id);
    // Register an object created at runtime (spawned enemies); it starts disabled.
    Object& add_object(int id, size_t index, const std::string& type, const glm::vec3& position, float yaw);
    void enable(Object& o, bool on);
    void show(Object& o, bool on);
    void set_door(Object& o, bool open_now);
    // StartEffect / StopEffect: the object's effect `index` (its built-in effect: any index).
    bool start_effect(Object& o, int index);
    void stop_effect(Object& o, int index);
    void message(const std::string& text, float seconds);
    // ShowDialogMessage: voice-over lines in order (each shown while its voice plays);
    // "MessageEnd" is sent after the last.
    void dialog(const std::vector<std::string>& lines);
    void teleport_player(const Object& spawn);
    void start_script(const std::string& function, int owner);
    void send_event_later(const std::string& event, float seconds, int id = -1);
    void fade_fog(int what, glm::vec4 target, float seconds);   // 0 colour, 1 near, 2 far, 3 density

    Scene& scene;
    Player& player;
    Combat* combat = nullptr;        // enemies and damage, if the game runs combat
    Audio* audio = nullptr;          // sound, when there is a device
    std::vector<glm::vec3> actor_positions;   // moving actors (doors open for them too)
    // Screen fade (StartFadeIn/StartFadeOut: CLevel +0x29c..0x2a8) and camera shake
    // (CameraShake -> CGameCamera::ShakeCamera).
    void start_fade(bool in, float ms, glm::vec3 color) { fade_in_ = in, fade_total_ = fade_left_ = std::max(ms, 1.0f) / 1000.0f, fade_color = color; fading_ = true; }
    float fade_alpha() const;
    glm::vec3 fade_color{0};
    void shake(float ms, float amount) { shake_total_ = shake_left_ = ms / 1000.0f, shake_amount_ = amount; }
    glm::vec2 shake_offset() const;   // yaw, pitch (degrees) to add to the view
    // Objects moving on paths (CMotionOnPathComponent: elevators, platforms), started by
    // GameObject_MotionOnPathStart. False if the object has no path.
    bool motion_start(int id);
    void motion_stop(int id, int at_waypoint = -1);
    void motion_max_speed(int id, float speed);
    // Cinematics (CCinematicComponent 0x41): StartCinematicObject plays the object's clips
    // and runs its timed script events (Obj<id>_<track>_<n>_OnScriptEvent); a camera in its
    // model takes over the view meanwhile. StartCinematic/EndCinematic take the player's
    // control away and give it back.
    bool start_cinematic(int id);
    void end_cinematic(int id);
    bool cinematic_view(Scene::CameraView& out) const;
    bool player_locked() const;      // no player control (a cinematic is running)
    bool cinematic_mode = false;     // StartCinematic .. EndCinematic
    // PlayerLookTo(waypoint, ms): turn the player's view to a point.
    void player_look_to(const glm::vec3& at, float seconds);
    bool look_target(float& yaw, float& pitch) const;   // the view while turning (degrees)
    // Checkpoints (SaveGame(spawn point) -> PlayerComponent::SaveGame): the game saves and
    // reloads there when the player dies. Here the world stays as it is: the player comes
    // back at the checkpoint's spawn point, whose Obj<id>_Default script runs again.
    void save_checkpoint(int spawn_id);
    void respawn_player();
    int checkpoint() const { return checkpoint_; }
    // TeleportToWaypoint: move an object (actors through combat) to a position.
    void teleport_object(Object& o, const glm::vec3& position);
    // Post effects the scripts control (PostEffect_Enable / SetVar / SetParam).
    PostParams post;
    // A post-effect variable ("DOF"/"HEIGHTFOG" and fFocus, fRange, fBlurStep, FogTop, ...),
    // reaching `to` in `seconds`; false if unknown.
    bool set_post_var(const std::string& effect, const std::string& var, float to, float seconds, const float* from = nullptr);
    bool door_passable(int id) {     // for actors' paths: doors that are not locked
        const Object* o = object(id);
        return !o || o->door < 0 || !doors_[size_t(o->door)].locked || doors_[size_t(o->door)].keep_unlocked;
    }
    const Gol& gol;
    ScriptRuntime runtime_;
    std::unordered_map<int, Object> objects_;
    std::vector<Trigger> triggers_;
    std::vector<Door> doors_;
    std::unordered_set<std::string> warned_;   // unimplemented API functions already reported

private:
    void load_objects();
    void read_effects(Object& obj, const LevelObject& lo);
    void update_triggers();
    void update_doors(float dt);
    void update_fog(float dt);

    struct DelayedEvent {
        std::string event;
        int id;
        float seconds;
    };
    std::vector<DelayedEvent> delayed_;
    struct PostFade {
        float* value;
        float from, to, elapsed, duration;
    };
    std::vector<PostFade> post_fades_;
    struct FogFade {
        glm::vec4 from{0}, to{0};
        float elapsed = 0, duration = 0;
        bool active = false;
    } fog_[4];
    std::deque<Message> messages_;
    int properties_id_ = -1, start_spawn_id_ = -1;
    // Background music (level properties [41], an event of tracks), with a pause between
    // tracks; it gives way while scripts play action music.
    std::string music_event_;
    float music_gap_min_ = 12, music_gap_max_ = 22, music_wait_ = 0;
    int music_ = -1;
    void update_music(float dt);
    std::deque<std::string> dialog_;
    bool dialog_active_ = false;
    int dialog_voice_ = -1;
    float dialog_wait_ = 0;
    void update_dialog(float dt);
    // Paths: waypoint chains (each waypoint's next) and CPathComponent objects (id -> first waypoint).
    std::unordered_map<int, int> waypoint_next_, path_first_;
    struct Mover {
        int id = 0, type = 0, path = -1;
        size_t index = 0;
        float max_speed = 0, accel = 0, brake = 0;
        bool loop = false, moving = false;
        std::vector<glm::vec3> points;   // the route of the current motion
        std::vector<int> point_ids;      // its waypoints (-1: where it started)
        size_t segment = 0;
        float along = 0, speed = 0;      // metres into the segment, metres per second
        int stop_at = -1;                // waypoint to stop at (GameObject_MotionOnPathStopAtWaypoint)
        glm::vec3 position{0};
        std::vector<int> sounds;         // started with the motion (loops stop with it)
    };
    std::vector<Mover> movers_;
    int checkpoint_ = -1;
    struct Cinematic {
        int id = 0;
        size_t index = 0;
        float time = 0, duration = 0;   // ms
        std::vector<std::pair<float, std::string>> events;   // start ms, script function
        size_t next = 0;
        bool was_visible = true;
    };
    std::vector<Cinematic> cinematics_;
    void update_cinematics(float dt);
    const Value* object_component(const Object& o, int comp) const;
    glm::vec3 look_at_{0};
    float look_left_ = 0, look_total_ = 0;
    float look_from_yaw_ = 0, look_from_pitch_ = 0;
    void update_movers(float dt);
    void mover_state(Mover& m, const std::string& state);
    bool fading_ = false, fade_in_ = true;
    float fade_total_ = 1, fade_left_ = 0;
    float shake_total_ = 0, shake_left_ = 0, shake_amount_ = 0, shake_clock_ = 0;
    std::unique_ptr<class AnimSounds> anim_sounds_;
    int fog_zone_ = -2;              // zone whose fog is applied (-2: none yet)
    glm::vec4 level_fog_color_{0};
    float level_fog_start_ = 0, level_fog_end_ = 0, level_fog_density_ = 0;
    void update_zone_fog();
    Stats stats_;
};

// Register the game's script functions with the runtime (game/game_api.cpp).
void register_game_api(LevelLogic& logic);

}  // namespace nova
