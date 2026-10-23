// Combat: the player's weapons and health, damageable objects, enemy actors and their
// projectiles.
//
// Targets are level objects with health (CHealthComponent). Actors are targets with an
// AI and a navigation component: they idle until they notice the player
// (CAwarenessComponent: own team, a mask of the teams they attack, sight range, field
// of view, close range), then chase along the nav mesh and attack. Ranged NPCs fire
// bursts of bolts from their weapon (CNPCComponent -> CNPCWeaponComponent ->
// CProjectileComponent); the others close in for melee. Damage falls off with distance
// as in CProjectileComponent::GetDamage; the player regenerates health after a pause
// (CPlayerBaseComponent). Animations are picked by name from each actor's clips; their
// root motion is held in place and the actor moves at the clip's root speed instead.
#pragma once

#include <algorithm>
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"
#include "game/navmesh.h"
#include "render/renderer.h"

namespace nova {

class Scene;
class Player;
class LevelLogic;

// CProjectileComponent: what a hit looks and sounds like, by surface material (the
// collision triangles' material byte).
struct Impacts {
    std::unordered_map<int, std::string> effects;
    std::unordered_map<int, std::vector<std::string>> sounds;
};

struct WeaponDef {
    std::string name;
    int clip = 30, reserve = 210;      // reserve -1: unlimited
    float interval = 0.1f;             // seconds between shots
    bool automatic = true;
    float reload_time = 1.8f;
    float spread_step = 1.5f;          // degrees added per shot (recovers over time)
    // CProjectileComponent::GetDamage: `near` damage up to near_dist, `far` beyond far_dist.
    float damage_near = 2, damage_far = 1, near_dist = 30, far_dist = 30;
    // Sounds (CComponentWeapon fire mode): one per shot, or a loop while firing and its end.
    std::vector<std::string> shot_sounds;
    std::string reload_sound, loop_sound, end_sound;
    Impacts impacts;
    std::string sprite;                // HUD icon and crosshair (.bsprite)
};

// An NPC command (CAIComponent::PushGoal_*, from NpcCmd_* script calls): goals run in
// order and take over the actor's AI; `id` is the script's, for WaitNpcCmdFinished.
struct NpcGoal {
    enum class Kind { Move, Jump, Turn, Teleport, Action } kind = Kind::Move;
    int id = 0;
    glm::vec3 pos{0};
    bool run = true;              // Move: the "run" state (else walking pace)
    std::string state;            // Action: the state to play
    float timer = 0, length = 0;
    bool started = false;
};

struct CombatInput {
    bool fire = false, reload = false;
    bool grenade = false;              // throw a grenade
    bool power = false;                // use the selected special power (Repel)
    int select = -1;                   // weapon slot to switch to
};

class Combat {
public:
    static constexpr uint32_t kPlayerTeams = 1u << 1;   // the player is team 1

    Combat(Scene& scene, Player& player, LevelLogic& logic, const Gol& gol);

    // Collect targets and actors from the loaded level (after LevelLogic::start, and
    // again after a section reload).
    void start(bool keep_stats = false);   // keep_stats: a section reload within the level
    // `eye`/`aim`: the camera position and view direction.
    void update(float dt, const CombatInput& in, const glm::vec3& eye, const glm::vec3& aim);
    // Effects (world) and HUD (screen) for this frame.
    void draw(Overlay& o, const glm::vec3& eye, int width, int height) const;
    // Keep the player out of actors (after the player moves).
    void push_player_out();

    // Script API. GetNoEnemies: living actors of every team but the player's.
    int enemies_alive() const { return count_teams(~kPlayerTeams); }
    int count_teams(uint32_t team_mask) const;   // CAIController::GetNoOpponentsByTeams
    std::vector<int> enemy_ids() const;
    // NPC commands: queue a goal for the actor with this id (false if there is none);
    // whether goal `goal_id` (-1: any) is still queued; drop all queued goals.
    bool push_goal(int actor, const NpcGoal& g);
    bool goal_pending(int actor, int goal_id) const;
    void cancel_goals(int actor);
    // A death screen offers Retry (respawn()); without one the player is back after 3 s.
    bool death_screen = false;
    void respawn();
    float dead_time() const { return dead_timer_; }
    void ammo_refill_on_respawn() {   // the checkpoint's ammunition is not saved here: refill clips
        for (size_t i = 0; i < weapons_.size(); ++i) clip_[i] = weapons_[i].clip, reserve_[i] = std::max(reserve_[i], weapons_[i].clip * 2);
    }
    // Put an actor at a position (TeleportToWaypoint); false if `id` is not an actor.
    bool place_actor(int id, const glm::vec3& position);
    // A spawn manager with this id that has not ended (SpawnEnd_<id> is still to come).
    bool spawner_pending(int id) const;
    bool damage(int id, float amount);   // false if no such target
    void kill_all_enemies();
    void set_invulnerable(int id, bool on);
    void damage_player(float amount, const glm::vec3& from);
    // Damage falling off to nothing at `radius` (grenades, GiveSplashDamage), behind cover none.
    void splash_damage(const glm::vec3& at, float damage, float radius, bool by_player);

    bool shoot_enabled = true;         // EnableShootControl
    bool aim_assist = true;            // settings: shots snap to an enemy a few degrees off the crosshair
    bool auto_switch = true;           // settings: an emptied weapon gives way to one with ammunition
    void set_max_shooters(int n) { max_shooters_ = std::max(n, 0); }
    // SetGlobalAwarenessFactor: how fast NPCs notice their enemies (0: not at all).
    float awareness_factor = 1;
    // LockTarget / IgnoreTarget (object ids; 0 is the player). False if `npc` is no actor.
    bool lock_target(int npc, int target, bool on);
    // SendEvent orders to an NPC: follow_mode, lock_in_cover/locked_in_cover/imobile,
    // lock_ai (bool); link_region (ai_region id, 0 none); run/fly (waypoint);
    // use_machinegun (gun). False if `npc` is no actor or the event is not an order.
    bool npc_order(int npc, const std::string& order, int arg, bool flag);
    bool ignore_target(int npc, int target, bool on);
    bool player_invulnerable = false;  // debugging (--god)
    float player_health = 150, player_max_health = 150;
    bool player_dead = false;

    struct Stats {
        size_t targets = 0, actors = 0, hostile = 0, shots = 0, hits = 0, kills = 0, enemy_shots = 0;
        float damage_taken = 0;
        // The end of level's statistics (GS_EndLevel).
        size_t headshot_kills = 0, repel_kills = 0, paralyze_kills = 0, slow_kills = 0, deaths = 0;
        float play_time = 0;
    };
    const Stats& stats() const { return stats_; }
    const WeaponDef& weapon() const { return weapons_[size_t(current_)]; }
    int ammo_in_clip() const { return clip_[size_t(current_)]; }
    // The player's weapon, for the view model: its gol type, and what it is doing.
    const std::string& weapon_type() const { return weapon_types_[size_t(current_)]; }
    bool reloading() const { return reload_left_ > 0; }
    const std::string& weapon_sprite() const { return weapon().sprite; }
    int clip_size() const { return weapon().clip; }
    int current_weapon() const { return current_; }
    // Weapons the player has (unlocked); `select` indices for CombatInput.
    int weapon_count() const;
    int owned_weapon(int k) const;          // the k-th weapon the player has, -1 if none
    int cycle_weapon(int step) const;       // the next (step +1) or previous (-1) one the player has
    // Script API (UnlockWeapon, LockWeapon, UseWeapon, GiveAmmo): weapons by their name
    // (CComponentWeapon [0], e.g. AutomaticShotGun) or gol type; "Grenade" is the grenades.
    bool unlock_weapon(const std::string& name);
    bool lock_weapon(const std::string& name);
    bool use_weapon(const std::string& name);
    bool give_ammo(const std::string& name, int amount);
    // The weapons earlier campaign levels unlocked, when a later level is started directly.
    void campaign_loadout(const std::string& level);
    // Riding a vehicle (its turret: VehicleWeaponPlayer) or manning a mounted gun
    // (MG_Player): that weapon instead of the player's own, endless; off again restores it.
    void use_vehicle_weapon(bool on) { use_special_weapon(vehicle_weapon_, on); }
    void use_mounted_gun(bool on) { use_special_weapon(mounted_gun_, on); }
    void use_mech_weapon(bool on) { use_special_weapon(mech_weapon_, on); }
    bool special_weapon() const { return current_ == vehicle_weapon_ || current_ == mounted_gun_ || current_ == mech_weapon_; }
    void spend_round() { int& c = clip_[size_t(current_)]; c = std::max(c - 1, 0); }   // QTE_Fire
    int grenades = 0;
    bool grenades_enabled = true;
    // Special powers (CPowerManager): Repel, the plasma hand, pushes back and hurts what is
    // in front; 5 s to recharge. The view model plays its state when taken.
    bool power_ready() const { return power_cooldown_ <= 0; }
    // Special powers by name (CPowerManager: Repel, Paralyze, Slow); Repel from the start.
    bool unlock_power(const std::string& name, bool on);
    bool select_power(const std::string& name);
    void cycle_power();                                   // Tab / swiping the power button
    const std::string& power() const { return powers_[size_t(power_index_)]; }
    bool use_power_now(const std::string& name);          // UseSpecialPower
    float power_charge() const { return 1 - power_cooldown_ / kPowerCooldown; }
    bool take_power_used() { return std::exchange(power_used_, false); }
    // A grenade throw has started (the view model plays ThrowGrenade); cleared when taken.
    bool take_grenade_throw() { return std::exchange(throw_started_, false); }
    float since_shot() const { return since_shot_; }   // seconds since the last shot
    int ammo_reserve() const { return reserve_[size_t(current_)]; }
    const Navigation& nav() const { return nav_; }
    std::function<void(const std::string&)> log = [](const std::string&) {};

    // One line per target (debugging): id, type, position, flags.
    std::vector<std::string> describe() const;
    // Nearest living hostile actor in view (for tests): its centre, false if none.
    bool nearest_enemy(const glm::vec3& eye, glm::vec3& out) const;

private:
    struct Target {
        int id = 0;
        size_t object = 0;
        std::string type;
        glm::vec3 pos{0};              // feet
        float yaw = 0;                 // degrees; the model faces +Y at 0
        float radius = 0.6f, height = 1.9f;
        bool headshots = false;
        bool actor = false, hostile = false, mobile = false, ranged = false, explosive = false;
        int team = 2;
        uint32_t attacks = 0;          // bit mask of teams
        float sight = 40, fov = 180, close = 10;
        float run_speed = 4;
        // Ranged attack: bolts.
        float bolt_damage = 3, bolt_speed = 40, burst_gap = 0.1f, burst_pause = 1.6f, fire_range = 35;
        int burst = 6;
        glm::vec4 bolt_color{1.0f, 0.45f, 0.15f, 1};
        // Melee.
        float melee_damage = 10, melee_range = 2.2f;
        bool invulnerable = false;
        bool spawned = false;
        // Who it fights: -1 the player, >= 0 a target index, kNoEnemy none. Scripts can
        // lock a target (LockTarget: object id, 0 the player) or make it ignore some.
        int enemy = kNoEnemy;
        int locked = -1;               // object id; 0 the player; -1 none
        std::vector<int> ignored;      // object ids; 0 the player
        float choose_timer = 0, awareness = 0;
        // Script orders (SendEvent): follow the player, hold the position (cover, a
        // mounted gun), AI off, keep within an ai_region box.
        bool follow = false, hold = false, ai_locked = false, has_region = false;
        float paralyzed = 0;   // seconds left frozen by the Paralyze power
        glm::vec3 region_min{0}, region_max{0};          // created by a spawn manager (its body is removed later)
        // CTargetableComponent: particle effects when hit and when killed, at a node.
        std::string hit_effect, hit_node, death_effect, death_node;
        // CHealthComponent sounds; the NPC weapon's shot sounds (one per burst) and impacts.
        std::vector<std::string> hurt_sounds, death_sounds, fire_sounds;
        float hurt_sound_wait = 0;
        Impacts bolt_impacts;

        enum class State { Idle, Chase, Attack, Melee, Hurt, Dying, Dead } state = State::Idle;
        bool aware = false;
        float timer = 0, fire_timer = 0, repath = 0, hurt_cooldown = 0, lost_sight = 0;
        int shots_left = 0;
        bool token = false;            // allowed to shoot at the player (a few at a time)
        bool melee_hit = false;
        int tri = -1;
        std::vector<glm::vec3> path;
        std::vector<uint8_t> path_off_mesh;   // walking to this point crosses a doorway
        size_t path_next = 0;
        glm::vec3 path_goal{0};
        // Off the nav mesh (spawned on a ledge or a wall): leap down onto it.
        bool leaping = false;
        std::deque<NpcGoal> goals;
        std::string clip_walk;
        float leap_t = 0;
        glm::vec3 leap_from{0}, leap_to{0};
        int leap_tri = -1;
        std::string clip_idle, clip_alert, clip_run, clip_fire, clip_melee, clip_hurt, clip_jump, playing;
        std::vector<std::string> clip_deaths;
    };
    // CSpawnManager: waves of pooled enemies at waypoints. A wave starts once fewer than
    // its threshold of the watched teams are alive; each entry appears after its delay at
    // its spawn point; after the last wave the manager switches off and raises SpawnEnd_<id>.
    struct SpawnManager {
        int id = 0;
        std::vector<int> points;       // waypoint ids, one per entry slot
        struct Entry {
            int type = -1, delay_ms = 0, waypoint = -1;
        };
        struct Wave {
            std::vector<Entry> entries;
            int group = -1, threshold = 0;
        };
        std::vector<Wave> waves;
        bool loop = false;
        uint32_t teams = ~0u;
        int wave = -1;
        float time = 0;
        std::vector<uint8_t> spawned;
        bool ended = false;
    };
    void update_spawners(float dt);
    void spawn(SpawnManager& m, size_t slot, const SpawnManager::Entry& e);

    struct Bolt {
        glm::vec3 pos, vel;
        float damage, life;
        glm::vec4 color;
        int owner;                     // target index
    };
    struct Tracer {
        glm::vec3 from, to;
        float life;
    };
    struct Spark {
        glm::vec3 pos;
        float life;
        glm::vec4 color;
    };

    const Value* component(const LevelObject& o, int comp) const;
    void setup_actor(Target& t, const LevelObject& o);
    void play(Target& t, const std::string& clip, bool loop);
    void update_actor(Target& t, float dt);
    void move_along_path(Target& t, float dt, float speed_scale = 1);
    void run_goal(Target& t, float dt);
    void path_to(Target& t, const glm::vec3& goal);
    // Run along the nav mesh towards `goal` (repathing as it moves), stopping `stop` away.
    void go_towards(Target& t, const glm::vec3& goal, float stop, float dt);
    void stay_on_mesh(Target& t);
    void face(Target& t, const glm::vec3& towards, float dt);
    static constexpr int kNoEnemy = -2;
    // The enemy's feet and the point to shoot at; false if it is gone.
    bool enemy_points(const Target& t, int enemy, glm::vec3& feet, glm::vec3& chest) const;
    bool sees_point(const Target& t, const glm::vec3& point) const;
    bool fights(const Target& t, int enemy) const;   // a valid enemy for t
    int choose_enemy(const Target& t) const;
    Target* find_target(int id);
    bool line_clear(const glm::vec3& a, const glm::vec3& b) const;
    void notice(Target& t);
    void apply_damage(Target& t, float amount, bool by_player);
    void die(Target& t);
    // Start a particle effect at `at` with its +Z along `normal`; false if there is no such effect.
    bool effect(const std::string& name, const glm::vec3& at, const glm::vec3& normal = {0, 0, 1});
    void sound(const std::vector<std::string>& names, const glm::vec3* at);
    void sound(const std::string& name, const glm::vec3* at) { if (!name.empty()) sound(std::vector<std::string>{name}, at); }
    // A projectile hitting the level: the material's effect and sound.
    void impact(const Impacts& im, uint8_t material, const glm::vec3& at, const glm::vec3& normal);
    int fire_loop_ = -1;                // the player's looping fire sound
    std::vector<std::string> player_hurt_sounds_;
    float player_hurt_wait_ = 0;
    void fire_player(const glm::vec3& eye, const glm::vec3& aim);
    // Grenades (CGrenadeComponent of Grenade_Player): thrown along the view, they bounce
    // off the level and explode when the fuse runs out, hurting everything in the radius.
    struct GrenadeDef {
        int type = -1;
        float fuse = 2.3f, damage = 40, radius = 10, speed = 25;
        std::string effect = "explosion", trail, bounce_sound;
        std::vector<std::string> explode_sounds;
    };
    struct Grenade {
        size_t object = size_t(-1);
        glm::vec3 pos{0}, vel{0};
        float fuse = 0, bounce_wait = 0, spin = 0;
        int trail = -1;
        bool live = false;
    };
    void load_grenade();
    void throw_grenade(const glm::vec3& eye, const glm::vec3& aim);
    void update_grenades(float dt);
    void explode(const glm::vec3& at);
    GrenadeDef grenade_def_;
    std::vector<Grenade> live_grenades_;
    float throw_delay_ = -1, grenade_cooldown_ = 0;
    static constexpr float kPowerCooldown = 5.0f;
    float power_cooldown_ = 0;
    bool power_used_ = false;
    std::vector<std::string> powers_{"Repel"};
    int power_index_ = 0;
    void repel(const glm::vec3& eye, const glm::vec3& aim);
    void paralyze(const glm::vec3& eye, const glm::vec3& aim);
    void use_selected_power(const glm::vec3& eye, const glm::vec3& aim);
    bool throw_started_ = false;
    void fire_bolt(Target& t, const glm::vec3& at);
    // Ray against targets: index and distance, -1 if none within `max_t`.
    int ray_target(const glm::vec3& from, const glm::vec3& dir, float max_t, float& t_hit, glm::vec3& point) const;
    WeaponDef load_weapon(const std::string& type_name) const;
    glm::vec3 player_center() const;
    bool alive(const Target& t) const;

    Scene& scene_;
    Player& player_;
    LevelLogic& logic_;
    const Gol& gol_;
    Navigation nav_;
    std::vector<Target> targets_;
    std::vector<SpawnManager> spawners_;
    int next_spawn_id_ = 2000000;
    std::vector<Bolt> bolts_;
    std::vector<Tracer> tracers_;
    std::vector<Spark> sparks_;
    std::vector<WeaponDef> weapons_;
    std::vector<int> clip_, reserve_;
    int current_ = 0;
    std::vector<std::string> weapon_types_;
    std::vector<uint8_t> owned_;
    int vehicle_weapon_ = -1, mounted_gun_ = -1, mech_weapon_ = -1, before_special_ = 0;
    void use_special_weapon(int index, bool on);
    int find_weapon(const std::string& name) const;
    float since_shot_ = 100;
    float fire_cooldown_ = 0, reload_left_ = 0, spread_ = 0;
    bool trigger_held_ = false;
    float regen_delay_ = 0, regen_rate_ = 20, regen_pause_ = 3.0f;
    float damage_flash_ = 0, hit_marker_ = 0, dead_timer_ = 0, protect_ = 0;
    bool killing_headshot_ = false, killing_repel_ = false;   // what the damage being dealt is
    int max_shooters_ = 2;   // enemies firing at the player at once (attack tokens; SetMaxActiveEnemies)
    static constexpr float kBurstInterval = 1.2f;   // seconds between any two bursts starting
    float burst_cooldown_ = 0;
    glm::vec3 last_eye_{0}, last_aim_{0, 1, 0};
    bool aim_on_enemy_ = false;
    std::mt19937 rng_{1234};
    Stats stats_;
};

}  // namespace nova
