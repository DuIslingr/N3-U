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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"
#include "script/lua_runtime.h"

namespace nova {

class Scene;
class Player;

class LevelLogic {
public:
    LevelLogic(Scene& scene, Player& player, const Gol& gol);

    // Load the scripts and objects of the scene's current level and run its start
    // scripts. Call after Scene::load_level and spawning the player.
    void start();
    // Advance scripts, triggers and doors by `dt` seconds.
    void update(float dt);
    // The player pressed "use": interact with the nearest usable object, if any.
    bool use();
    // Run a Lua snippet as a script (with the game API), e.g. for testing.
    bool exec(const std::string& code);

    // Script messages (ShowMessage, print, errors), for the HUD/console.
    struct Message {
        std::string text;
        float seconds_left = 0;
    };
    const std::deque<Message>& messages() const { return messages_; }
    std::function<void(const std::string&)> log = [](const std::string&) {};
    bool trace = false;              // log every started script function
    bool level_ended = false;        // EndLevel() was called

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
    };
    Object* object(int id);
    void enable(Object& o, bool on);
    void show(Object& o, bool on);
    void set_door(Object& o, bool open_now);
    void message(const std::string& text, float seconds);
    void teleport_player(const glm::vec3& feet, float yaw);
    void start_script(const std::string& function, int owner);
    void send_event_later(const std::string& event, float seconds, int id = -1);
    void fade_fog(int what, glm::vec4 target, float seconds);   // 0 colour, 1 near, 2 far, 3 density

    Scene& scene;
    Player& player;
    const Gol& gol;
    ScriptRuntime runtime_;
    std::unordered_map<int, Object> objects_;
    std::vector<Trigger> triggers_;
    std::vector<Door> doors_;
    std::unordered_set<std::string> warned_;   // unimplemented API functions already reported

private:
    void load_objects();
    void update_triggers();
    void update_doors(float dt);
    void update_fog(float dt);

    struct DelayedEvent {
        std::string event;
        int id;
        float seconds;
    };
    std::vector<DelayedEvent> delayed_;
    struct FogFade {
        glm::vec4 from{0}, to{0};
        float elapsed = 0, duration = 0;
        bool active = false;
    } fog_[4];
    std::deque<Message> messages_;
    int properties_id_ = -1, start_spawn_id_ = -1;
    Stats stats_;
};

// Register the game's script functions with the runtime (game/game_api.cpp).
void register_game_api(LevelLogic& logic);

}  // namespace nova
