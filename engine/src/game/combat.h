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

#include <functional>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"
#include "game/navmesh.h"
#include "render/renderer.h"

namespace nova {

class Scene;
class Player;
class LevelLogic;

struct WeaponDef {
    std::string name;
    int clip = 30, reserve = 210;      // reserve -1: unlimited
    float interval = 0.1f;             // seconds between shots
    bool automatic = true;
    float reload_time = 1.8f;
    float spread_step = 1.5f;          // degrees added per shot (recovers over time)
    // CProjectileComponent::GetDamage: `near` damage up to near_dist, `far` beyond far_dist.
    float damage_near = 2, damage_far = 1, near_dist = 30, far_dist = 30;
};

struct CombatInput {
    bool fire = false, reload = false;
    int select = -1;                   // weapon slot to switch to
};

class Combat {
public:
    static constexpr uint32_t kPlayerTeams = 1u << 1;   // the player is team 1

    Combat(Scene& scene, Player& player, LevelLogic& logic, const Gol& gol);

    // Collect targets and actors from the loaded level (after LevelLogic::start, and
    // again after a section reload).
    void start();
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
    bool damage(int id, float amount);   // false if no such target
    void kill_all_enemies();
    void set_invulnerable(int id, bool on);
    void damage_player(float amount, const glm::vec3& from);

    bool shoot_enabled = true;         // EnableShootControl
    bool player_invulnerable = false;  // debugging (--god)
    float player_health = 150, player_max_health = 150;
    bool player_dead = false;

    struct Stats {
        size_t targets = 0, actors = 0, hostile = 0, shots = 0, hits = 0, kills = 0, enemy_shots = 0;
        float damage_taken = 0;
    };
    const Stats& stats() const { return stats_; }
    const WeaponDef& weapon() const { return weapons_[size_t(current_)]; }
    int ammo_in_clip() const { return clip_[size_t(current_)]; }
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
        bool spawned = false;          // created by a spawn manager (its body is removed later)
        // CTargetableComponent: particle effects when hit and when killed, at a node.
        std::string hit_effect, hit_node, death_effect, death_node;

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
    void move_along_path(Target& t, float dt);
    void stay_on_mesh(Target& t);
    void face(Target& t, const glm::vec3& towards, float dt);
    bool sees_player(const Target& t) const;
    bool line_clear(const glm::vec3& a, const glm::vec3& b) const;
    void notice(Target& t);
    void apply_damage(Target& t, float amount, bool by_player);
    void die(Target& t);
    // Start a particle effect at `at` with its +Z along `normal`; false if there is no such effect.
    bool effect(const std::string& name, const glm::vec3& at, const glm::vec3& normal = {0, 0, 1});
    void fire_player(const glm::vec3& eye, const glm::vec3& aim);
    void fire_bolt(Target& t);
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
    float fire_cooldown_ = 0, reload_left_ = 0, spread_ = 0;
    bool trigger_held_ = false;
    float regen_delay_ = 0, regen_rate_ = 20, regen_pause_ = 3.0f;
    float damage_flash_ = 0, hit_marker_ = 0, dead_timer_ = 0, protect_ = 0;
    static constexpr int kMaxShooters = 2;   // enemies firing at the player at once (attack tokens)
    static constexpr float kBurstInterval = 1.2f;   // seconds between any two bursts starting
    float burst_cooldown_ = 0;
    glm::vec3 last_eye_{0}, last_aim_{0, 1, 0};
    bool aim_on_enemy_ = false;
    std::mt19937 rng_{1234};
    Stats stats_;
};

}  // namespace nova
