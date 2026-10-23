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
#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"
#include "assets/strings.h"
#include "game/controls.h"
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
    void start(bool keep_player = false) { start(keep_player ? StartMode::Section : StartMode::New); }
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
        std::string speaker;         // voice-over lines: who speaks ("Kal" for VO_KAL_...)
    };
    StringTable strings;             // MAIN and the level's table (English)
    InputScheme input = default_input_scheme();   // prompts are worded for it
    // Player controls the scripts switch on and off (Enable<Kind>Control); disabled ones
    // ignore input and their touch buttons are hidden.
    struct Controls {
        bool move = true, rotate = true, shoot = true, jump = true, aim = true, rush = true, powers = true, weapons = true,
             items = true;
    } controls;
    bool hud_enabled = true;               // EnableHud
    bool interface_in_cinematic = false;   // ForceInterfaceInCinematic: the HUD stays up in a cinematic
    std::string weapon_state;              // SetCrtWeaponState request, taken by the view model
    // Mission objectives (CLevel: SetPrimaryObjective clears the secondaries; each change
    // shows the objectives box for 5 s; completing every secondary completes the primary).
    struct Objectives {
        std::string primary;               // text
        bool primary_done = false;
        struct Secondary {
            std::string id, text;
            bool done = false;
        };
        std::vector<Secondary> secondary;
        float show = 0, alpha = 0;         // seconds left on screen; fade
    } objectives;
    // StartBulletTime(ms, factor): the game runs `factor` times as fast for `ms` of real
    // time (Application's time factor and its duration). Advance with real time.
    void bullet_time(float ms, float factor) { time_factor_ = factor, time_factor_left_ = ms / 1000.0f; }
    float time_scale() const { return time_factor_left_ > 0 ? time_factor_ : 1.0f; }
    void advance_real_time(float dt);
    // Quick-time events (CQTEManager): QTEAddSlowTime(factor, ms) queues ramps of the time
    // factor (over game time; the last usually freezes the game), QTEStart(kind) waits for
    // the player's button ("fire", "repel", "move"); pressing it ends the QTE and the game
    // speeds back up. `auto_qte` presses it after a moment (headless runs).
    void qte_add_slow_time(float factor, float ms) { qte_.ramps.push_back({factor, ms}); }
    void qte_start(const std::string& kind);
    void qte_stop();
    bool qte_press(const std::string& kind);   // true if that ended the QTE
    const std::string& qte_kind() const { return qte_.kind; }
    float qte_time() const { return qte_.time; }   // seconds (real) since it started
    bool auto_qte = false;
    // EnablePath(waypoint): the guide marker the HUD shows over a waypoint (-1: none).
    struct PathMarker {
        bool on = false;
        glm::vec3 position{0};
    } path_marker;
    // AddObjective(object[, text, flag, node]) / RemoveObjective(object): markers on objects
    // (CPlayerBaseComponent's objective list), drawn like the path marker.
    struct ObjectiveMarker {
        int id = -1;
        std::string text, node;
    };
    std::vector<ObjectiveMarker> objective_markers;
    // Colour grading (CLevel: the level's LUT, level properties [38]; SetColorGrading(lut,
    // ms) blends to another over ms, DisableColorGrading turns it off). Fills post.grading.
    void set_color_grading(const std::string& lut, float ms);
    void disable_color_grading() { grading_.on = false, grading_.dirty = true; }
    // SetFOV(fov, ms) / ResetFOV(ms): the field of view moves to a target (game units, 70
    // by default). SetZFar, SetMoveSensitivity, PlayerEnableRush.
    void set_fov(float fov, float ms) { fov_from_ = game_fov(), fov_to_ = fov, fov_len_ = ms / 1000.0f, fov_t_ = 0; }
    float game_fov() const { return fov_len_ > 0 && fov_t_ < fov_len_ ? fov_from_ + (fov_to_ - fov_from_) * fov_t_ / fov_len_ : fov_to_; }
    static constexpr int kPlayerId = 123456;   // the scripts' id for the player
    float zfar = 8000, move_sensitivity = 1;
    float look_sensitivity = 1;   // SetControlSensitivity: the view turns this much slower
    bool player_rush = true;
    bool fall_damage = true;   // Player_EnableFallDamage
    bool jetpack_unlocked = false;   // Unlock/LockSpecialPower("jetpack"); UseSpecialPower turns it on
    // StartTimer(s) / PauseTimer(on) (ILevelInterface::m_iTimer): a countdown shown at the
    // top of the screen (seconds; < 0: none), ticking each second.
    float timer = -1;
    bool timer_paused = false;
    struct Frost {
        float on = 0, ms = 1000, max = 1;   // HUD_StartFrost / HUD_StopFrost over ms; HUD_SetFrostMaxFactor
        uint32_t serial = 0;
    } frost;
    // Vehicles (CCarAIComponent, simplified): Vehicle_MoveOnPath drives one along a
    // waypoint chain at its maximum speed (Vehicle_SetMaxSpeed, km/h), turning no faster
    // than its steering allows, on the ground. Vehicle_Activate puts the player in the
    // using vehicle (its "camera" node is the seat), with the view kept within yaw limits
    // (Vehicle_SetYawLimits, relative to the vehicle, eased over a time);
    // SwitchToMachineGun mans a mounted gun; SwitchToPlayerOnFoot leaves either.
    bool vehicle_move_on_path(int id, int waypoint);
    void vehicle_stop(int id);
    // Vehicle_FollowObject(vehicle, target, metres): drive after an object (123456: the
    // player), keeping that far behind it.
    bool vehicle_follow(int id, int target, float metres);
    void vehicle_set(int id, float max_speed_kmh, float steer_max, float steer_speed);   // < 0: keep
    void vehicle_yaw_limits(int id, float min, float max, float ms);
    bool enter_vehicle(int id);
    bool man_gun(int id);
    bool enter_mech(int id);   // SwitchToMech: the player pilots the mech
    bool in_mech() const { return player_mech_ >= 0; }
    void leave_vehicle();
    int player_vehicle() const { return player_vehicle_; }   // -1 on foot
    int player_gun() const { return player_gun_; }
    // Where the riding player's eyes are, and the yaw range the view may take (degrees).
    bool seat(glm::vec3& eye, float& yaw_min, float& yaw_max);
    void reset_trigger_runs(int id) {
        if (Object* o = object(id); o && o->trigger >= 0) triggers_[size_t(o->trigger)].runs_left = triggers_[size_t(o->trigger)].runs;
    }
    void set_primary_objective(const std::string& id);
    void add_secondary_objective(const std::string& id);
    void secondary_objective_complete(const std::string& id);
    void primary_objective_complete();
    const std::deque<Message>& messages() const { return messages_; }
    std::function<void(const std::string&)> log = [](const std::string&) {};
    bool trace = false;              // log every started script function
    bool skip_intro = false;         // (testing) the start spawn point's script does not run
    bool level_ended = false;        // EndLevel() was called
    bool level_success = true;       // EndLevel(false): the mission failed
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
        int runs = -1;               // as loaded (ResetTriggerRunCount)
        bool player_target = true;
        bool inside = false;
        // A rotated box: tested in its own frame (box from local_min to local_max).
        bool rotated = false;
        glm::mat4 to_local{1};
        glm::vec3 local_min{0}, local_max{0};
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
    // The zone of the cinematic holding the view (kNone if none): the view belongs there.
    int cinematic_zone() const;
    bool cinematic_active() const {   // a cinematic holds the view
        Scene::CameraView v;
        return cinematic_view(v);
    }
    bool cinematic_mode = false;     // StartCinematic .. EndCinematic
    // PlayerLookTo(waypoint, ms): turn the player's view to a point.
    void player_look_to(const glm::vec3& at, float seconds);
    void ramp_param(size_t object, const std::string& param, float from, float to, float seconds) {
        std::erase_if(param_ramps_, [&](const ParamRamp& r) { return r.object == object && r.param == param; });
        param_ramps_.push_back({object, param, from, to, std::max(seconds, 1e-3f), 0});
    }
    // PostEffect_HeatHazeStart(ms, object) / Stop(ms) (CLevel::UpdatePostEffect_HeatHaze):
    // the haze grows over ms; with an object, it is strongest while the player faces it.
    void heat_haze(bool start, float ms, int object = -1) {
        if (start) {
            heat_.max = std::max(ms, heat_.now), heat_.stopping = false, heat_.object = object;
        } else {
            if (ms < heat_.now) heat_.max -= heat_.now - ms, heat_.now = ms;
            heat_.stopping = true;
        }
    }
    void motion_follow(int id, int leader, float metres) {
        if (Mover* m = mover(id)) m->follow = leader, m->follow_distance = metres;
    }
    // GetPlayerPositionFromCameraPosition: the next teleport keeps the camera's view.
    void handover_view(const glm::vec3& forward) { handover_ = forward, have_handover_ = true; }
    bool look_target(float& yaw, float& pitch);   // the view while turning or held (degrees)
    // CameraLookToWaypoint(wp, ms): the camera alone turns to a point and stays on it
    // (CGameCamera state 1) until CameraSwitchToPlayer gives back the player's own view
    // (state 0), or ResetPlayerCamera hands the player the view as it is.
    void camera_look_to(const glm::vec3& at, float seconds);
    void camera_to_player(bool keep_view);
    // The first clip of a state in gol type `type`'s state set ("" if it has none).
    std::string state_clip(int type, const std::string& state);
    // Tutorial steps (CLevelTutorial*): Start<Kind>Tutorial(ms) waits for the player to do
    // it (turn, reload, jump, move, shoot, switch weapons, sprint...) or for `ms` to pass,
    // then sends "TutorialStepEnded".
    void start_tutorial_step(const std::string& kind, float ms);
    void hide_message(const std::string& id);
    // Checkpoints (SaveGame(spawn point) -> PlayerComponent::SaveGame -> Application::
    // SaveCheckPoint): the level's state is saved (CLevel::Save: each object's
    // CGameObject::SaveSave — enabled, dead, where it stands if it moves, its trigger's runs,
    // door, effects and animation — the player's weapons and objectives, fog, post effects,
    // the loaded sections). When the player dies the level is reset and that state put back
    // (CLevel::RestartLevel, GS_Respawn -> CLevel::RespawnInit): the scripts start afresh
    // (globals cleared, the level properties' OnEnter runs again), objects return to their
    // checkpoint state, and the player spawns at the checkpoint, whose Obj<id>_Default runs.
    // Without a checkpoint the level starts over.
    void save_checkpoint(int spawn_id);
    // The player died and goes back: sets respawn_pending for the caller, who reloads the
    // scene with checkpoint_section() and calls respawn_at_checkpoint() (no checkpoint:
    // reloads the level).
    void respawn_player() { respawn_pending = true; }
    bool respawn_pending = false;
    uint32_t checkpoint_section() const;
    void respawn_at_checkpoint();
    int checkpoint() const { return checkpoint_; }
    // The checkpoint as bytes for the save file, and back (save_game.cpp); empty / false
    // without one or for data that does not read.
    std::vector<uint8_t> checkpoint_data() const;
    bool load_checkpoint_data(const std::vector<uint8_t>& data);
    // Called when a checkpoint is taken (the save file is written then).
    std::function<void()> on_checkpoint;
    // The end of level's play time bonus limits, minutes (level properties [43], [44]).
    float play_time_limit(int k) const { return play_time_limits_[k & 1]; }
    // Limited control (CPlayerLimitedControlComponent; SwitchToPlayerLimitedControl): the
    // player as the PlayerLimitedControl rig (arms_legs.bdae) seen through its camera —
    // falling and strafing in space (Ship_01's decompression), hanging from the hull with
    // the arm's console (Minigame: a tap on it wins), a hand on a panel. Its states (the
    // rig's state set) play their clips and run their Script tracks; one whose clip has
    // played goes on to its next; at "End" the player is back on foot where the camera was.
    // It moves by strafing (while idle or strafing) and the level's attraction, through
    // walls unless gravity is on; the view turns within its yaw/pitch limits and drifts
    // back after 2 s untouched.
    bool limited_active() const { return limited_.on; }
    void limited_begin();
    void limited_end();
    void limited_state(const std::string& state);
    void limited_hang(int object);    // PlayerLimitedControlStartHang: onto the object's plane, Hang
    void limited_gravity(bool on) { limited_.gravity = on; }
    void limited_limits(float yaw, float pitch) {   // degrees; < 0 keeps
        if (yaw >= 0) limited_.limit_yaw = yaw;
        if (pitch >= 0) limited_.limit_pitch = pitch;
    }
    void limited_strafe_speed(float s) { limited_.strafe = s; }
    // Move the player (`right`: -1..1 strafe input); false when not in limited control.
    bool limited_step(float right, float dt);
    // The view's yaw/pitch (degrees) as the player turned it, kept within the limits.
    void limited_view_input(float& yaw, float& pitch, float dt);
    bool limited_view(Scene::CameraView& out) const;   // the rig's camera, turned by the view
    bool limited_tap();   // a tap (click, use): the arm console in Minigame wins
    // Lifts (CElevatorAIComponent 0x18: [0]/[1] its two waypoints, [2] speed in m/s): it
    // waits (Idle) for the player to stand on it, then closes and carries them to its other
    // end (Opening, DestEnd), and once they step off it waits there for the way back.
    // SendEvent(lift, "MoveToStart"/"MoveToEnd") calls it to an end on its own; LockElevator
    // stops it from moving once it is back at rest, UnlockElevator lets it.
    void lock_elevator(int id, bool lock);
    bool elevator_event(int id, const std::string& event);   // false if not a lift's event
    // Linked objects (CLinkObjectComponent 0x17, [0]: the object it is linked to): carried
    // with that object (enemies riding a lift, guns on a vehicle); a linked NPC holds its
    // place (CNPCComponent::SetImobile) until Npc_UnLink. Npc_Link links it to another.
    bool link(int id, int target);
    void unlink(int id);
    // TeleportToWaypoint: move an object (actors through combat) to a position.
    void teleport_object(Object& o, const glm::vec3& position);
    // Post effects the scripts control (PostEffect_Enable / SetVar / SetParam).
    PostParams post;
    // CLevel's timed screen effects (one at a time; CLevel::StartPostEffect, UpdatePostEffect):
    // PostEffect_FlashbackStart(ms) turns the picture sepia over ms and holds it;
    // PostEffect_FlashbackStop(ms) drops it and, given a time, comes back from overexposed
    // (ColorLevels: white 0.1 -> 1 over ms); StartFlashEffect(ms) flashes white, the bright
    // parts fading last (Flash: threshold -1 -> 1).
    enum class ScreenEffect { None, Flash, Sepia, Levels, Gray };
    void screen_effect(ScreenEffect kind, float ms);
    // StartEndLevelEffect(ms, r, g, b, back): CLevel's Gray, the picture drained to a tinted
    // grey over ms (or, with `back`, its colour returning), held there.
    void gray_effect(float ms, const glm::vec3& tint, bool back);
    // EnableMotionBlur(on): fast turns blur the picture outwards (PlayerComponent::
    // UpdateFromInput: past 8 degrees a frame at 30 fps, the RadialBlur by the turn).
    bool motion_blur = false;
    void update_motion_blur(float frame_dt);   // once a rendered frame, with its time
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
    // The waypoint after `id` on its path (-1 if none).
    int next_waypoint(int id) const {
        auto it = waypoint_next_.find(id);
        return it == waypoint_next_.end() ? -1 : it->second;
    }
    std::vector<Trigger> triggers_;
    std::vector<Door> doors_;
    std::unordered_set<std::string> warned_;   // unimplemented API functions already reported

private:
    enum class StartMode { New, Section, Respawn };
    void start(StartMode mode);
    struct Checkpoint;
    std::shared_ptr<const Checkpoint> saved_;
    bool restoring_ = false;   // between the scripts' restart and the checkpoint put back
    void restore_checkpoint(const Checkpoint& cp);
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
    // tracks (12 to 22 s, this port's choice); it gives way while scripts play action music.
    std::string music_event_;
    float music_wait_ = 0;
    float play_time_limits_[2] = {0, 0};
    int music_ = -1;
    void update_music(float dt);
    // The jetpack's sound (PlayerComponent::EnableJetpackSound: sfx_jetpack.vxn), "ride"
    // while it fires, "idle" while it does not, "stop" when it is put away.
    int jetpack_sound_ = -1;
    void update_jetpack_sound();
    // Kal's breathing in the suit (PlayerComponent::SetBreathingState, kal_breathing_system
    // .vxn) while the player is in control: battle for 5 s after a shot or a hit, dying
    // under 20% of the shield, running while sprinting, then recovery for 4 s; else normal.
    int breath_sound_ = -1;
    std::string breath_state_;
    float breath_timer_ = 0, breath_recovery_ = 0, breath_damage_ = 0;
    bool breath_rushing_ = false;
    void update_breathing(float dt);
    std::deque<std::string> dialog_;
    bool dialog_active_ = false;
    int dialog_voice_ = -1;
    float dialog_wait_ = 0;
    std::string dialog_line_;   // the line being spoken
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
        glm::vec3 direction{0};          // along the current segment
        std::vector<int> sounds;         // started with the motion (loops stop with it)
        int follow = -1;                 // GameObject_MotionOnPathSetFollowTarget: keep with this mover,
        float follow_distance = 0;       // within this many metres along the way
    };
    // A mover by its object id (null if none).
    Mover* mover(int id) {
        for (Mover& m : movers_)
            if (m.id == id) return &m;
        return nullptr;
    }
    std::vector<Mover> movers_;
    int checkpoint_ = -1;
    struct TutorialStep {
        std::string kind;
        float time_left = -1;   // seconds; < 0: no time limit
        float progress = 0;
        glm::vec3 start{0};
        float last_yaw = 0, last_pitch = 0;
        std::string weapon;
        int count = 0;          // the combat count the step waits on, at its start
        bool active = false;
    } tutorial_;
    void update_tutorial(float dt);
    struct Cinematic {
        int id = 0;
        size_t index = 0;
        float time = 0, duration = 0;   // ms
        std::vector<std::pair<float, std::string>> events;   // start ms, script function
        size_t next = 0;
        bool was_visible = true;
        bool ending = false;   // past its length: ends next update (its last events run first)
    };
    std::vector<Cinematic> cinematics_;
    void update_cinematics(float dt);
    const Value* object_component(const Object& o, int comp) const;
    glm::vec3 look_at_{0}, handover_{0};
    bool have_handover_ = false;
    float look_left_ = 0, look_total_ = 0;
    float look_from_yaw_ = 0, look_from_pitch_ = 0;
    bool camera_held_ = false, restore_view_ = false;
    float held_yaw_ = 0, held_pitch_ = 0;
    void update_movers(float dt);
    struct Elevator {
        int id = 0, type = 0;
        size_t index = 0;
        glm::vec3 start{0}, end{0}, position{0};
        float speed = 1;
        bool to_end = true;            // where it goes next
        std::string state = "Idle";
        float state_time = 0, state_length = 0;
        bool moving = false, lock_pending = false;
        glm::vec3 from{0}, dest{0};
        float along = 0;
        std::vector<int> sounds;
    };
    std::vector<Elevator> elevators_;
    Elevator* elevator(int id);
    void elevator_state(Elevator& e, const std::string& state);
    void elevator_move(Elevator& e, bool to_end);
    void update_elevators(float dt);
    struct Limited {
        bool on = false, gravity = true;
        std::string state;
        float time = 0, clip_length = 0;   // seconds in the state; its clip's length
        size_t next_script = 0;
        float strafe = 0.05f, limit_yaw = 10, limit_pitch = 10;   // the player type's CPlayerLimitedControlComponent
        float defaults[3] = {0.05f, 10, 10};
        float base_yaw = 0;                // the rig's heading
        float yaw = 0, pitch = 0, idle = 0;   // the view's turn (degrees); seconds since turned
        int rig_type = -1;
        size_t rig = size_t(-1);
    } limited_;
    void update_limited(float dt);
    void place_rig();
    void load_type_scripts(int type);
    struct Link {
        int id = 0, target = 0;
        glm::mat4 local{1};          // the object in the target's frame
        glm::mat4 last{0};           // the target's transform when last carried
        bool held = false;           // a linked NPC told to hold its place
    };
    std::vector<Link> links_;
    void update_links();
    void mover_state(Mover& m, const std::string& state);
    bool fading_ = false, fade_in_ = true;
    float fade_total_ = 1, fade_left_ = 0;
    float shake_total_ = 0, shake_left_ = 0, shake_amount_ = 0, shake_clock_ = 0;
    // GameObject_Material_SetParameterAnimatedFloat: a parameter run from one value to
    // another over a time (CMaterialComponent::Update).
    struct ParamRamp {
        size_t object = 0;
        std::string param;
        float from = 0, to = 0, seconds = 1, elapsed = 0;
    };
    std::vector<ParamRamp> param_ramps_;
    struct {
        ScreenEffect kind = ScreenEffect::None;
        float time = 0, length = 0;   // seconds
        bool back = false;            // Gray: towards colour
    } screen_effect_;
    void update_screen_effect(float dt);
    float blur_yaw_ = 0, blur_pitch_ = 0;
    struct HeatHaze {
        float now = 0, max = 0;   // ms grown, ms to grow
        bool stopping = true;
        int object = -1;
    } heat_;
    void update_heat_haze(float dt);
    float time_factor_ = 1, time_factor_left_ = 0;
    float fov_from_ = 70, fov_to_ = 70, fov_len_ = 0, fov_t_ = 0;
    struct Vehicle {
        int id = 0;
        size_t index = 0;
        glm::vec3 position{0};
        float yaw = 0, speed = 0, max_speed = 12.5f, steer_max = 40, steer_speed = 40;
        std::vector<glm::vec3> route;
        size_t next = 0;
        bool moving = false;
        int follow = -1;              // object id (kPlayerId: the player), -1 none
        float follow_distance = 10;
        float yaw_min = -180, yaw_max = 180, from_min = -180, from_max = 180, limits_t = 0, limits_len = 0;
        // CVehicleCar's parts, objects of their own at the model's dummies (its HIDDEN_
        // stand-ins are not drawn): wheels, the gun, the driver and the shooter.
        struct Part {
            size_t object = 0;
            std::string node;
            enum class Kind { Wheel, Gun, Rider } kind = Kind::Rider;
        };
        std::vector<Part> parts;
        float wheel_angle = 0;
    };
    void spawn_vehicle_parts(Vehicle& v);
    void place_vehicle_parts(Vehicle& v);
    std::unordered_map<int, Vehicle> vehicles_;
    Vehicle* vehicle(int id);   // created from the object on first use
    void update_vehicles(float dt);
    int player_vehicle_ = -1, player_gun_ = -1, player_mech_ = -1;
    // An object's script for an event: Obj<id>_<event>, else its type's Templ<type>_<event>.
    bool start_object_script(const Object& o, const std::string& event);
    float gun_yaw_ = 0;   // the mounted gun's own heading
    struct Qte {
        std::string kind;                               // waiting for this button; "" none
        std::deque<std::pair<float, float>> ramps;      // (factor, game ms)
        float target = 1, left = 0;                     // the current ramp
        float time = 0;
    } qte_;
    struct Grading {
        std::string current, previous;
        float left = 0, total = 0;   // blend from previous to current (seconds)
        bool on = false, dirty = true;
    } grading_;
    std::string default_lut_;
    std::unordered_map<std::string, std::array<uint8_t, 1024>> luts_;
    const std::array<uint8_t, 1024>* lut(const std::string& name);   // its curves, nullptr if missing
    void update_grading(float dt);
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
