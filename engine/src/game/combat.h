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
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"
#include "game/decals.h"
#include "game/navmesh.h"
#include "game/ragdoll.h"
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
    DecalTable decals;
};

// A projectile's looks in flight (CProjectileComponent and its game object): the object
// it carries ([36]: the bullet streak), its effects (autostart, and [23] when [22]) and its
// own model (its type, when it has a mesh). With none of these, our glow.
struct ProjectileLooks {
    int trail = -1;
    std::vector<std::string> fx;
    int body = -1;
    bool any() const { return trail >= 0 || !fx.empty() || body >= 0; }
};

struct WeaponDef {
    std::string name;
    int type = -1;                     // its gol type
    int clip = 30, reserve = 210;      // reserve -1: unlimited
    float interval = 0.1f;             // seconds between shots
    bool automatic = true;
    // Its Reload state: the clip's length (at its speed), and when its GameEvent 4 fills
    // the clip (seconds into it; -1 at the end).
    float reload_time = 1.8f, reload_refill = -1;
    // Its spread (degrees; CComponentWeapon [23]-[27] from the hip, [28]-[32] aiming): the
    // least, the most, its growth per second walking, its shrink per second otherwise and
    // its growth firing (per second of the frame a shot goes in).
    struct Spread {
        float min = 1, max = 2, walk = 10, recover = 20, fire = 1.5f;
    } hip, aim;
    // Its crosshair (CWeaponComponent::DrawTargetCrossHair: frames 4-8 of its sprite): the
    // arms [34] pixels (at 640 high) further out at the widest spread, above, right, below
    // and left [36] (else at the corners); [33] shown aiming too; [46] a scope's, shown only
    // looking through it.
    int cross_px = 15;
    bool cross_plus = false, cross_aiming = false, scope = false;
    float aim_move = 1;                // [52]: the player's speed aiming (GetAimPlayerMoveFactor)
    // Lock-on (CWeaponComponent::UpdateTarget): [20] ms on a target before the lock (< 0:
    // none), [21] degrees off the view it is kept. Its projectile's [15] homing (how fast
    // it turns to the target), [16] only when locked.
    float lock_ms = -1, lock_cone = 0, homing = 0;
    bool homing_needs_lock = false;
    // CProjectileComponent::GetDamage: `near` damage up to near_dist, `far` beyond far_dist.
    float damage_near = 2, damage_far = 1, near_dist = 30, far_dist = 30;
    // Sounds (CComponentWeapon fire mode): one per shot, or a loop while firing and its end.
    std::vector<std::string> shot_sounds;
    std::string reload_sound, loop_sound, end_sound;
    Impacts impacts;
    std::string sprite;                // HUD icon and crosshair (.bsprite)
    // The bullet (CComponentProjectile): its speed (m/s), looks, explosion ([6] damage,
    // [7] radius; CProjectileComponent::Die) and life ([39] ms). It flies (a projectile, not
    // an instant hit) when slow with looks of its own, or when it explodes: the rocket
    // launcher, the plasma guns.
    float bullet_speed = 0;
    int grenade = -1;                  // the projectile is a grenade (CGrenadeComponent): its type
    ProjectileLooks looks;
    float splash_damage = 0, splash_radius = 0, bullet_life = 3;
    bool flies = false;
    // A rail beam ([32]: CBulletRailGunSceneNode) from the muzzle to the hit: its texture
    // ([30]) and width ([31]).
    std::string beam_texture;
    float beam_width = 0;
};

// An NPC command (CAIComponent::PushGoal_*, from NpcCmd_* script calls): goals run in
// order and take over the actor's AI; `id` is the script's, for WaitNpcCmdFinished.
struct NpcGoal {
    enum class Kind { Move, Jump, Turn, Teleport, Action } kind = Kind::Move;
    int id = 0;
    glm::vec3 pos{0};
    bool run = true;              // Move: the "run" state (else walking pace)
    bool dash = false;            // Teleport: by super speed (NPCSuperSpeedToWaypoint)
    std::string state;            // Action: the state to play
    float timer = 0, length = 0;
    bool started = false;
};

struct CombatInput {
    bool fire = false, reload = false;
    bool aiming = false;               // the view narrowed (the weapon's aiming spread)
    bool grenade = false;              // throw a grenade (or a mine, when they are picked)
    bool switch_throwable = false;     // grenades <-> mines
    bool detonate = false;             // set off the mines placed
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
    // Checkpoints: what the game saves of combat (PlayerComponent's CWeaponManager and
    // CPowerManager, the actors' CActorComponent/CNPCComponent): the weapons the player has,
    // their ammunition and the one in hand, grenades and powers; which actors are dead, and
    // where the others stand (they come back at full health).
    struct Stats {
        size_t targets = 0, actors = 0, hostile = 0, shots = 0, hits = 0, kills = 0, enemy_shots = 0;
        float damage_taken = 0;
        // The end of level's statistics (GS_EndLevel).
        size_t headshot_kills = 0, repel_kills = 0, paralyze_kills = 0, slow_kills = 0, deaths = 0;
        size_t criticals = 0;   // falls below 30% of the shield
        float play_time = 0;
    };
    struct Saved {
        std::vector<uint8_t> owned;
        std::vector<int> clip, reserve;
        int current = 0, grenades = 0, mines = 0, power_index = 0;
        std::vector<std::string> powers;
        struct Actor {
            int id = 0;
            bool dead = false;
            glm::vec3 pos{0};
            float yaw = 0;
        };
        std::vector<Actor> actors;
        Stats stats;   // the level's statistics as they were (all but the retries and the time)
    };
    Saved save() const;
    void restore(const Saved& s);   // after start() on the freshly loaded level
    // The arsenal alone (weapons, ammunition, grenades, mines, powers): a level begun from
    // the save made when the one before ended.
    void restore_arsenal(const Saved& s);
    bool is_actor(int id) const;
    // A death screen offers Retry (respawn()); without one the player is back after 3 s.
    bool death_screen = false;
    void respawn();
    float dead_time() const { return dead_timer_; }
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
    bool npc_order(int npc, const std::string& order, int arg, bool flag, float extra1 = 0, float extra2 = 0);
    bool ignore_target(int npc, int target, bool on);
    // IsParalyzed: an object the Paralyze power has frozen (doors, power modules, enemies).
    // -1 if it has no health.
    int paralyzed(int id) const;
    // Npc_SetBulletType (CNPCComponent::SetWeaponBulletType): the NPC fires this projectile type.
    bool set_bullet_type(int npc, int type);
    // Enter/ExitInvisibility: an NPC cloaked (held so) or out of it again; false if it can't.
    bool script_invisibility(int npc, bool on);
    // LockEnemyAggro(npc, value) (CAIComponent::LockAggro): its aggressiveness held there.
    bool lock_aggressiveness(int npc, float value);
    // Npc_GetAccuracy / Npc_SetAttackBehavior (the attack-choice scripts).
    float accuracy(int npc) const;
    void set_attack_behaviour(int npc, const std::string& name);
    // NpcCmd_TeleportToTarget (CNPCComponent::StartTeleportToTarget): the NPC appears on the
    // walkable area a few metres from the player, on its own side. Queued as goal `goal_id`.
    bool teleport_to_player(int npc, int goal_id);
    // Npc_SetCustomIdleState (CNPCComponent::SetCustomIdleState): a state of the NPC's
    // state set to idle in ("ConsoleWork", "Talk"); "" for its usual idle.
    bool set_idle_state(int npc, const std::string& state);
    bool player_invulnerable = false;  // debugging (--god)
    float player_health = 150, player_max_health = 150;
    // The difficulty (0 Easy, 1 Normal, 2 Hard; Menu_SetDifficulty): gol 4509 DifficultyLevels
    // scales the player's shield ([1]: 1.2, 1, 0.8) and the pause before it regenerates
    // ([3]: 0.8, 1, 1.2). Set before the level starts.
    void set_difficulty(int d);
    int difficulty() const { return difficulty_; }
    bool player_dead = false;

    const Stats& stats() const { return stats_; }
    const WeaponDef& weapon() const { return weapons_[size_t(current_)]; }
    int ammo_in_clip() const { return clip_[size_t(current_)]; }
    // The player's weapon, for the view model: its gol type, and what it is doing.
    const std::string& weapon_type() const { return weapon_types_[size_t(current_)]; }
    bool reloading() const { return reload_left_ > 0; }
    // Where the view model's muzzle is now: the player's bullets leave from it (null: a
    // point below and right of the eye).
    void set_muzzle(const glm::vec3* at) { has_muzzle_ = at != nullptr; if (at) muzzle_ = *at; }
    // The actors' weapons and other attachments onto their nodes, after the scene animated.
    void place_attachments();
    // The rail beams, as textured quads after the particles.
    void build_beams(const glm::vec3& eye, ParticleList& out);
    const std::string& weapon_sprite() const { return weapon().sprite; }
    // What the HUD draws the crosshair with (CWeaponComponent::DrawTargetCrossHair).
    struct Crosshair {
        std::string sprite;
        float arms = 0;            // how far out the arms are, in pixels at 640 high
        bool plus = false, scope = false;
        glm::vec4 color{1};        // red on an enemy
        float alpha = 0;
        float distance = -1;       // the scope's: to what it is on
    };
    Crosshair crosshair() const;
    // CLevel::StartHitMarker: the player's shot hit something that takes damage (Chatboard
    // frame 231 in the middle while it lasts).
    bool hit_marker() const { return hit_marker_ > 0; }
    int clip_size() const { return weapon().clip; }
    int current_weapon() const { return current_; }
    // SetMinEnemiesOnPlayer / RestoreMinEnemiesOnPlayer (CAIController +0xac, 1 to start
    // with): how many hostile actors at least fight the player rather than its allies.
    int min_enemies_on_player = 1;
    // Weapons the player has (unlocked); `select` indices for CombatInput.
    int weapon_count() const;
    int owned_weapon(int k) const;          // the k-th weapon the player has, -1 if none
    int cycle_weapon(int step) const;       // the next (step +1) or previous (-1) one the player has
    // Script API (UnlockWeapon, LockWeapon, UseWeapon, GiveAmmo): weapons by their name
    // (CComponentWeapon [0], e.g. AutomaticShotGun) or gol type; "Grenade" is the grenades.
    bool unlock_weapon(const std::string& name);
    bool lock_weapon(const std::string& name);
    bool use_weapon(const std::string& name);
    bool has_weapon(const std::string& name) const;   // unlocked
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
    // The Military Shop's (CUpgradesManager::ApplyUpgrade). A shield core: the shield
    // overcharged by `percent` of its maximum (the HUD's UpgradeVanguard/Crypton/Plasma bar),
    // until it is back under the maximum; only bought at or under it.
    int core = -1;            // 0 Vanguard, 1 Crypton, 2 Plasma; -1 none
    float core_percent = 0;
    bool core_allowed() const { return player_health <= player_max_health; }
    void take_core(int kind, float percent);
    // Instant Ammo Refill (CWeaponManager::RefillWeapons): every weapon held to its full clip
    // and reserve; allowed while one is short.
    bool can_refill() const;
    void refill_weapons();
    // Mines (PowerManager ThrowMine, Mine_Player), there from the start as grenades are (SF1
    // teaches them; Lock/UnlockWeapon("Mines")): the grenade button throws them instead when
    // switched to; they stick where they land and go off together when detonated (EvDetonate).
    bool mines_enabled = true;
    int mines = 0;
    bool mines_picked() const { return throw_mines_; }
    // Counts for the tutorial steps: throws (grenades or mines), switches, detonations.
    int throws() const { return throws_; }
    int switches() const { return switches_; }
    int detonations() const { return detonations_; }
    bool mines_placed() const;
    // The last pick-up taken (CLevelInterfaceNormal::ShowPowerUpInfo): its icon (frame 0 of
    // the .bsprite) and what it gave, for 2 s.
    struct PickupInfo {
        std::string sprite;
        int amount = 0;
        float time = 0;   // left to show
    };
    const PickupInfo& pickup_info() const { return pickup_info_; }
    // Repel's reach scaled (the Repel Upgrade, CPhysicsObjectComponent::ComputeRepel) and
    // damage added to the Paralyze shot (the Paralyze Upgrade, CProjectileComponent::GetDamage).
    float repel_scale = 1, paralyze_damage = 0;
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
    // The view's jolt for the last hit (CPlayerBaseComponent::ReceiveDamage): Hurt_front,
    // Hurt_left_heavy, ... by where it came from; the caller adds "IronSight_" while aiming.
    std::string take_view_hurt() { return std::exchange(view_hurt_, {}); }
    // The HUD's answer to the last hits (CPlayerBaseComponent::ReceiveDamage): the shield's
    // ripple while over half of it is left (at most every 3 s), the blood under that (the
    // shield's fraction left); and the blood's fade when the shield starts regenerating.
    bool take_hud_damage() { return std::exchange(hud_damage_, false); }
    float take_hud_blood() { return std::exchange(hud_blood_, -1.0f); }
    int take_scratch() { return std::exchange(scratch_, -1); }   // a spider's claw: 0 left, 1 right, -1 none
    bool take_blood_fade() { return std::exchange(blood_fade_, false); }
    // The hit indicator: shown while true, pointing `angle` degrees clockwise from straight up.
    bool hit_indicator(float& angle) const {
        angle = hit_angle_;
        return hit_left_ > 0;
    }
    // A landing hard enough for the weapon's landing clip: 0 Landing, 1 LandingMed,
    // 2 LandingHi; -1 if none since the last call.
    int take_landing() { return std::exchange(landing_, -1); }
    // A grenade throw has started (the view model plays ThrowGrenade); cleared when taken.
    bool take_grenade_throw() { return std::exchange(throw_started_, false); }
    // CLevel::DrawGrenades: enemy grenades that can reach the player (where they are).
    std::vector<glm::vec3> grenade_threats() const;
    float since_shot() const { return since_shot_; }   // seconds since the last shot
    unsigned shots() const { return shots_; }           // the player's shots so far
    // Shooting (CWeaponComponent's firing flag: set by FireButtonPress, cleared by
    // FireButtonRelease): the trigger held with shots still coming.
    bool firing() const { return trigger_held_ && since_shot_ < 0.2f; }
    // Riding a jeep: its health, 0..1 (the HUD's jeep shield); -2 not riding one.
    float vehicle_shield() const;
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
        // CAwarenessComponent::Alert: once aware, its sight [4] and view cone [7] (unset: the
        // same); DetectTarget: an enemy within [8] m is heard wherever it is.
        float sight_alert = -1, fov_alert = -1, hear = 0;
        float run_speed = 4;
        // Ranged attack: bolts.
        float bolt_damage = 3, bolt_speed = 40, burst_gap = 0.1f, burst_pause = 1.6f, fire_range = 35;
        int burst = 6;
        glm::vec4 bolt_color{1.0f, 0.45f, 0.15f, 1};
        ProjectileLooks bolt_looks;
        // Attacks played as states whose clips' GameEvents fire the shots (CComponentNPC [54]
        // {state, weight}: the Alien Commander's single shot and multi-shot sweep); the
        // state playing in such an attack, and how far its events have run.
        std::vector<std::pair<std::string, float>> attack_choices;
        std::string attack_state, attack_clip;
        float attack_ms = -1;
        // The hurt look (CEffectsLocalComponent, 0x93): a technique worn while its time
        // parameter runs from start to end (per ms x 0.001), a hit restarting it once it
        // passed `again`; the hit's point in a parameter. hurt_time < 0: not on.
        std::string hurt_technique, hurt_param, hurt_impact_param;
        // Its death look (CComponentHealth [13] model, [14] material, [20] sound): after the
        // death clip the body dissolves through it over 1.5 s and goes
        // (CHealthComponent::ActivateDeathEffect). dissolve < 0: not begun.
        std::string death_model, death_material, vanish_sound;
        std::string paralyze_model, paralyze_material;   // [15]/[16]: worn while paralyzed
        // The cloak (CNPCComponent::ChangeToInvisible / UpdateInvisibility; CComponentNPC
        // [62]/[63] material, [64] fade ms, [65] cloaks in a fight, [68]/[69] sounds;
        // CComponentAI [41]/[42] time invisible, [43] time visible at least, [45] damage
        // that sends it under). The material refracts the scene behind where its noise plus
        // BlendFactor is under 0.5: 0.5 the body, -0.5 a shimmer.
        enum class Cloak { Visible, FadingOut, Invisible, FadingIn } cloak = Cloak::Visible;
        std::string cloak_model, cloak_material, cloak_sound, uncloak_sound;
        bool auto_cloak = false, script_cloak = false;
        float cloak_fade = 0.8f, cloak_timer = 0, visible_left = 0;
        float invisible_min = 3, invisible_max = 3, visible_min = 1, cloak_damage = 1;
        std::vector<std::pair<float, float>> recent_damage;   // (clock, amount), the last 5 s
        float dissolve = -1;
        // CPhysicsRagdollComponent (0x8e): [0] has one, [1] its kind in RagdollsSettings;
        // a bullet's push the damage times [17] times [18], at most [19]; an explosion's [11].
        bool has_ragdoll = false;
        int ragdoll_kind = 0;
        float ragdoll_hit = 90, ragdoll_hit_max = 100, ragdoll_blast = 32;
        std::shared_ptr<Ragdoll> ragdoll;   // falling
        // CCharacterShadowPlaneComponent (0xa3): a shadow plane model ([0]) under it, [1]-[3]
        // from its feet turned with it; its material told where the bones of [9] are
        // (SPosition0...) and faded (Factor) by whether the player sees it ([4]).
        size_t shadow = size_t(-1);
        glm::vec3 shadow_offset{0};
        std::vector<std::string> shadow_nodes;
        std::vector<float> shadow_low;   // each node's least height (its material's SPosition z)
        std::vector<float> shadow_radius;   // and its spot's size (SRadius)
        bool shadow_seen = false;
        float shadow_factor = 0;
        float last_damage = 0;
        float hurt_start = 0.1f, hurt_end = 1, hurt_again = 0.6f, hurt_time = -1;
        // Its magazine (CComponentNPCWeapon [1] rounds, [2] a shot's; -1: endless), the
        // rounds left, a reload under way (its clip's seconds left) and its sounds ([23]).
        int mag_size = -1, per_shot = 1, mag = 0;
        float reload_left = 0;
        std::vector<std::string> reload_sounds;
        // What it carries on its nodes: its weapon (CNPCComponent's first, its object
        // first here) and CAttachedObjects (0x91: the marines' head lamp).
        struct Attachment {
            size_t object;
            std::string node;
            bool shown = false;
            bool on = true;   // a linked object: started (CLinkedObjectsComponent::StartObject)
        };
        std::vector<Attachment> attached;
        // Its linked objects (CLinkedObjectsComponent, 0x91: {type, node, started, ?}; a
        // marine's head lamp): their places in `attached`.
        std::vector<size_t> linked;
        bool armed = false;                // attached[0] is its weapon
        // The weapon's muzzle object (CComponentNPCWeapon [9] at [10]), shown per shot.
        int muzzle_type = -1;
        std::string muzzle_node;
        size_t muzzle_object = size_t(-1);
        std::vector<std::string> muzzle_clips;
        bool muzzle_shown = false;
        float bolt_splash = 0, bolt_radius = 0;   // its explosion (rockets)
        // Melee.
        float melee_damage = 10, melee_range = 2.2f;
        float melee_at = 0.45f;   // seconds into the blow it lands
        // Its melee position round the player (CTargetableComponent: -1 none), checked every
        // 0.3 s; without one it keeps off (a fiend circles, CCybertoothAiComponent action 2:
        // [8]-[0] m away).
        int melee_slot = -1;
        float slot_check = 0, circle_r = -1, circle_min = 7, circle_max = 12;
        bool circles = false, circle_left = false;
        std::string clip_circle_left, clip_circle_right;   // RunLeftStrong / RunRightStrong
        bool invulnerable = false;
        bool spawned = false;
        // Who it fights: -1 the player, >= 0 a target index, kNoEnemy none. Scripts can
        // lock a target (LockTarget: object id, 0 the player) or make it ignore some.
        int enemy = kNoEnemy;
        int locked = -1;               // object id; 0 the player; -1 none
        // Manning a mounted gun (use_machinegun; CMachineGunAIComponent::ActivateBy): its
        // level object, its state (0 idle, 1 a burst, 2 a pause), the time left in that, to
        // the next shot, the way it points and its firing loop's sound.
        int gun = -1, gun_state = 0, gun_loop = -1;
        float gun_timer = 0, gun_shot = 0, gun_yaw = 0;
        std::vector<int> ignored;      // object ids; 0 the player
        float choose_timer = 0, awareness = 0;
        // Script orders (SendEvent): follow the player, hold the position (cover, a
        // mounted gun), AI off, keep within an ai_region box.
        bool follow = false, hold = false, ai_locked = false, has_region = false;
        // "hurt" false: no hurt reactions (CNPCComponent flag 5); "exit_area": leaving, gone
        // once out of the player's sight (StartAutoDisable).
        bool no_hurt = false, exiting = false;
        float exit_time = 0;
        float paralyzed = 0;   // seconds left frozen by the Paralyze power
        float paralyze_time = 5;   // CHealthComponent::GetParalyzeDuration (health [3], ms)
        glm::vec3 region_min{0}, region_max{0};          // created by a spawn manager (its body is removed later)
        // CTargetableComponent: particle effects when hit and when killed, at a node.
        std::string hit_effect, hit_node, death_effect, death_node;
        // CHealthComponent sounds; the NPC weapon's shot sounds (one per burst) and impacts.
        std::vector<std::string> hurt_sounds, death_sounds, fire_sounds;
        std::vector<std::string> aggro_sounds;   // CComponentNPC [55]: turning on the player
        int weapon_kind = 1;   // CComponentNPC_Config (0x4c) [0]: 2 rocket launcher, 3 sniper rifle
        float hurt_sound_wait = 0;
        Impacts bolt_impacts;

        // CNPCWeaponComponent targeting: until it has the target, the aim point circles it at
        // a radius shrinking from up to sweep_max to sweep_min (m/s rate, turning at deg/s);
        // each shot's spread grows by spread_step up to (1 - accuracy) x spread_max degrees.
        // Accuracy (CAIComponent +0x80): time aimed over (1 - k) x acc_time, k rising from 0
        // past acc_near to 1 at acc_far, 1 within acc_near.
        float spread_max = 1, spread_step = 0.1f, sweep_max = 2, sweep_min = 0.05f, sweep_rate = 1, sweep_speed = 180;
        float acc_near = 10, acc_far = 50, acc_time = 25;
        float sweep_r = 2, sweep_a = 0, sweep_sign = 1, spread = 0, aim_time = 0;
        bool locked_on = false;
        // Attack behaviours (CAIComponent's list: name, its BA_Update script): shots per burst
        // and the gap between them, chosen per burst by the type's BA_ChooseAttack script.
        struct Behaviour {
            std::string name;
            int shots = 0;
            float gap = 0.1f, pause = 0;
        };
        std::vector<Behaviour> behaviours;
        std::string choose_fn, behaviour;
        // Aggressiveness (CAIComponent +0x50: 50 at first, rising while unhurt, near the
        // target, unseen by it or in cover, falling out of cover; LockEnemyAggro holds it) and
        // its bands (CComponentAI [66]: from the most aggressive, each with its threshold, the
        // distance it keeps from the target, its pause between bursts).
        float aggr = 50;
        bool aggr_locked = false;
        struct Band {
            float threshold = 0, range_min = 15, range_max = 25, pause_min = 1, pause_max = 1;
        // Weights of what it does from cover (UpdateLowCover's fuzzy actions): fire, lean,
        // blind fire, peek, roll out, throw a grenade.
        float fire = 100, lean = 0, blind = 0, peek = 0, roll = 0, grenade = 0;
        float crouch_min = 0, crouch_max = 0;   // seconds crouched in the open (after a roll out)
        float jump = 0;                         // [11]: % to jump to a jump point when it may
        float throw_chance = 0;                 // [12]: % to throw an object when it may
        };
        std::vector<Band> bands;
        // Its factors per second (CComponentAI [51]-[65], CAIComponent::GetAF_*); hurt: x the
        // share of its health lost; a team member gone: once.
        float af_hurt = 100, af_unhurt = 0.8f, af_near = 15, af_near_dist = 10, af_unseen = 0, af_behind = 0, af_in_cover = 0,
              af_out_of_cover = 0, af_team_alive = 20, af_team_dead = 0, af_last = 5;
        int team_seen = -1;   // team members alive last time
        float max_health = 10;
        // Cover (UseCoversForAttack): the cover point it holds, what it does there (moving to
        // it, hidden behind it, up and firing), for how long; when it looks for one again.
        bool use_covers = false;
        // Super speed (CNPCComponent::TrySSToPosition / UpdateSuperSpeed_), the Executors'
        // blur-fast dash: AI [9] can; NPC [30] how far aside, [31] the shortest, [32] m/s;
        // AI [31]/[32] ms before the next (timer 2, as between rolls).
        bool can_dash = false, dashing = false, dash_was_invulnerable = false;
        float dash_side = 7, dash_min = 5, dash_speed = 12, dash_wait = 0, dash_wait_min = 5, dash_wait_max = 10;
        float dash_blend = 0, dash_blend_ms = 0;   // the blur material's BlendFactor (CGameObject::MatsUpdate)
        glm::vec3 dash_from{0}, dash_to{0};
        // Jumping between jump points (CAIComponent::HideFromThreats, ChooseJumpPointInRange):
        // AI [11] can; a chance every AI [37]-[38] ms (timer 4); the take-off point at most
        // NPC [52] m of path away, walked to beyond NPC [53] m.
        bool can_jump = false;
        // Telekinesis (CNPCComponent::ChooseAttack / CanThrowObject / UpdateUpperBodyThrow /
        // DoThrowCurrentObject): AI [12] can; by the band's chance every AI [39]-[40] ms, an
        // object within AI [46] m of it, at least AI [48] m from the enemy, the enemy at least
        // AI [47] m away; pulled to NPC [59]'s node with NPC [56] playing, flung with [57].
        bool can_throw = false;
        float throw_wait = 0, throw_wait_min = 5, throw_wait_max = 7, throw_reach = 20, throw_enemy_min = 5, throw_object_min = 5;
        float throw_drag = 2.5f;   // AI [50]: how far in front it holds the object
        std::string grab_node, telekinesis_sound, telekinesis_end_sound;
        int held = -1, throw_stage = 0;   // held: the object (index); stages: pulling, throwing, after
        // The tentacle (CTentacleAIComponent, NPC [58]'s type): TentaclePart links from the grab
        // node to the object, growing at the type's speed; the telekinesis loop's voice.
        bool tentacle = false;
        float tentacle_len = 0;
        std::vector<size_t> tentacle_parts;
        int tk_voice = -1;
        bool clip_speed_set = false;   // a state played at its own speed (play() puts it back)
        float throw_timer = 0;
        // A throwable object (CPhysicsObjectComponent [1]; [6] m/s across when thrown): pulled,
        // held, then flying on a ballistic arc (CPhysicsObjectComponent::Throw) until it lands.
        bool throwable = false;
        float throw_speed = 22, throw_lift = 1;   // [6]; CThrowMotionComponent [6]: raised this far when grabbed
        // A dropship (CDropShipAIComponent, gol 0x9f: [0] its rocket, [1] the muzzle node, [2]
        // turn speed and [3] its acceleration (degrees a second), [5] its bank, [7] seconds
        // between attacks, [8]-[9] the rockets' miss (degrees), [10] a sound): hovering, it
        // turns to face its target; facing it, Attack, a rocket at the fire clip's event.
        bool dropship = false;
        std::string ds_muzzle, ds_sound;
        float ds_turn_max = 20, ds_turn_accel = 10, ds_pause = 1, ds_miss_min = 15, ds_miss_max = 30;
        float ds_yaw = 0, ds_turn = 0, ds_fire_at = -1, ds_wait = 0;
        bool ds_no_turn = false;      // disable_rotation
        // An armed car (CVehicleCarAI, gol 0x97; its gun CVehicleCar [0], a weapon whose
        // first fire mode gives the bullet [1], the muzzle object [9] at node [11], the
        // firing loop [17] and its end [18]): what it sees, its gun turns to and fires at,
        // a shot per loop of the gun's Attack_AI clip; destroyed, it plays Death.
        bool car = false, vehicle = false;   // vehicle: any CVehicleCar (0x96)
        // A jump attack: a big spider's (CBigSpiderAiComponent, 0x55: [6] farthest, [7] nearest
        // it jumps from, 30 m/s under gravity 30, 20-25 s between) or a fiend's
        // (CCybertoothAiComponent, 0x57: [7] farthest, [8] nearest, [10] speed, [11] gravity,
        // [19]-[20] s between): its start state, a ballistic leap at the target (JumpAir), a
        // claw in the air as it reaches it (JumpAirAttackLeft/Right, its event 5), its landing.
        bool spider = false, pounces = false;
        float jump_min = 10, jump_max = 20, jump_cool = 0, jump_timer = 0, jump_event = -1;
        float jump_speed = 30, jump_gravity = 30, jump_cool_min = 20, jump_cool_max = 25, jump_hit_left = 0.244f, jump_hit_right = 0.212f;
        int jump_type_left = 11, jump_type_right = 12;   // the claws' damage types
        std::string jump_start = "JumpStart", jump_land = "JumpLand";
        // A Red Fury trooper's jetpack (CRedFuryAiComponent, 0x59; its aggro level [22][0]:
        // [2]-[3] m from the target and [4]-[5] up in the air, [22]-[23] ms flying before it
        // lands, firing [26] ms then holding [27]-[28] ms): SendEvent "fly" (waypoint, speed,
        // steering) takes it up to the waypoint; there it fights from the air, moving between
        // points round its target, then lands and fights on foot (an AirOnly one stays up).
        bool red_fury = false, rf_air = false, rf_air_only = false, rf_fire_on = false, rf_landing = false, rf_directed = false;
        bool rf_spell_sounded = false;
        float rf_mode_timer = 0, rf_fire_timer = 0, rf_shot = 0, rf_repick = 0, rf_land_t = 0;
        float rf_air_min = 15, rf_air_max = 25, rf_h_min = 5, rf_h_max = 7, rf_time_min = 8, rf_time_max = 12;
        float rf_fire_on_s = 1.5f, rf_fire_off_min = 3, rf_fire_off_max = 4, rf_speed = 7, rf_accel = 10;
        glm::vec3 rf_vel{0}, rf_dest{0};
        int jump_phase = 0;   // 0 none, 1 start, 2 in the air, 3 landing
        bool jump_right = false;
        glm::vec3 jump_vel{0}, jump_goal{0};
        // An enemy mech (CMechAIComponent, 0x23: [3] % rockets when the target is [8]-[9] m
        // away, [4] ms out of sight before the guns stop, [11] its chain guns and [12] its rocket
        // pods {node, left, muzzle object}, [18]-[21] the guns' loop and stop, the rocket and
        // the cool-down sounds, [25] the bullet, [26] the rocket): AI_Fire or AI_MissileFire,
        // each GameEvent 2 / 3 a shot from the left / right, then AI_Cooldown.
        bool mech_ai = false;
        float mech_rockets = 20, mech_rocket_min = 10, mech_rocket_max = 60, mech_lost = 2, mech_unseen = 0;
        std::string mech_gun[2], mech_pod[2], mech_loop_sound, mech_stop_sound, mech_rocket_sound, mech_cool_sound;
        int mech_flash_type[2][2] = {{-1, -1}, {-1, -1}};   // [gun / pod][left / right]
        size_t mech_flash[2][2] = {{size_t(-1), size_t(-1)}, {size_t(-1), size_t(-1)}};
        int mech_plays = 0, mech_loop = -1;
        Impacts alt_impacts;   // the second projectile (a mech's rocket)
        ProjectileLooks alt_looks;
        float alt_damage = 30, alt_speed = 40, alt_splash = 0, alt_radius = 0;
        int car_gun_type = -1, car_loop = -1;
        bool car_firing = false, car_muzzle_shown = false;
        float car_interval = 0, car_shot = 0, car_yaw = 0, car_pitch = 0;
        float car_spread = 0, car_spread_min = 0.2f, car_spread_max = 1, car_spread_recover = 0.25f, car_spread_fire = 0.1f;
        std::string car_loop_sound, car_stop_sound;
        int flight = 0, thrower = -1;   // flight: 1 pulled, 2 held, 3 flying, 4 reached for
        bool flung = false;             // flying thrown (it goes off where it lands), not dropped
        glm::vec3 flight_from{0}, flight_vel{0};
        float flight_t = 0, flight_len = 0.5f;
        float jump_wait = 0, jump_wait_min = 5, jump_wait_max = 7, jump_reach = 8, jump_walk = 2;
        int cover = -1;
        enum class CoverPhase { Moving, Hidden, Acting } cover_phase = CoverPhase::Moving;
        // What it does out of hiding (UpdateLowCover's fuzzy actions), on which side of the
        // cover (CNPCComponent +0x31c: the Left states), and the clip a transition leads to.
        enum class CoverAct { Fire, Lean, Blind, Peek } cover_act = CoverAct::Fire;
        bool cover_left = false;
        std::string cover_from, cover_next;
        // A clip that moves it (its root's travel applied to its position): the roll out of
        // cover. Its root where it was last applied.
        std::string root_clip;
        glm::vec3 root_prev{0};
        bool root_moves = true;        // false: a clip it only waits out (getting up after the roll)
        float root_scale = 1;          // the root's travel scaled (a vault to the cover's depth)
        bool root_vault = false;       // vaulting a cover: off the walkable area until it lands
        int root_cover = -1;           // rolling into this cover: hidden there when the clip ends
        float crouch_left = 0;         // crouched in the open, firing (UpdateCrouch), this much longer
        bool cover_next_loop = false;
        float cover_timer = 0, cover_eval = 0;
        float idle_in_cover_min = 1, idle_in_cover_max = 2, attack_min = 1, attack_max = 3;   // seconds
        float stand_height = 1.9f;
        bool crouched = false;
        // Grenades (CComponentNPC [4] type, [5] how many, [6]/[7] throw range).
        int grenades = 0;
        float grenade_min = 20, grenade_max = 50, throw_at = -1;   // throw_at: seconds until the grenade leaves the hand
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
        std::string clip_idle_default;
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
        int owner;                     // target index; -1: the player
        float splash = 0, radius = 0;  // its explosion
        int weapon = -1;               // the player's: its weapon (the impacts)
        bool alt = false;              // its shooter's second projectile (the impacts)
        int trail_type = -1;           // its streak: object and where it started
        size_t trail = size_t(-1);
        glm::vec3 from{0};
        std::vector<int> fx{};         // its effects' handles
        int body_type = -1;            // its model's object
        size_t body = size_t(-1);
        int home = -1;                 // the target it homes on (index), -1 none
        float homing = 0, speed = 0;
    };
    // A bullet's streak (CProjectileComponent::LaunchBullet / UpdateBullet): its trail
    // object flies from the muzzle to where the shot ends at the bullet's speed, growing
    // ("start"), then fades there ("end").
    struct Tracer {
        size_t object;
        int type;
        glm::vec3 from, pos, dir, end;
        float speed;
        bool ending = false;
        float length = 1;              // its scale along the flight once it stopped
    };
    // A bullet stopped (CProjectileComponent::Die): its streak fades there, gone at once
    // after under 3 m of flight, shortened under 7.7 m.
    // False when it went at once (its object back in the pool).
    bool end_trail(Tracer& t);
    void launch_trail(const WeaponDef& w, const glm::vec3& from, const glm::vec3& to);
    void update_trails(float dt);
    size_t take_trail(int type, const glm::vec3& at);
    static glm::mat4 bolt_matrix(const glm::vec3& pos, const glm::vec3& dir);
    void hurt_look(Target& t, const glm::vec3* at);   // a hit: the hurt technique on (or again)
    void paralyze_target(Target& t);                    // frozen, wearing its paralyze material
    // The states set of a type (its own and its parents'): per state, its clips with their
    // GameEvents {ms, id}, whether it loops and the state after it.
    struct StateDef {
        std::vector<std::pair<std::string, std::vector<std::pair<float, int>>>> clips;
        bool loop = false;
        int plays = 1;   // its clips played in turn this many times
        std::string next;
    };
    const std::unordered_map<std::string, StateDef>& states_of(int type);
    std::unordered_map<int, std::unordered_map<std::string, StateDef>> states_cache_;
    bool play_attack_state(Target& t, const std::string& state);
    void event_attack(Target& t, const glm::vec3& chest, bool on_player);
    void mech_attack(Target& t, const glm::vec3& chest, bool sees, float dt);
    bool mech_state(Target& t, const std::string& state);
    void mech_shot(Target& t, const glm::vec3& at, bool left, bool rocket);
    void mech_stop(Target& t);
    void cloak(Target& t, bool instant);
    void uncloak(Target& t);
    void update_cloak(Target& t, float dt);
    float clock_ = 0;
    void update_paralysis(Target& t, float dt);
    void update_hurt_looks(float dt);
    void set_hurt_time(Target& t);
    ProjectileLooks read_looks(int type) const;
    std::vector<std::string> first_state_clips(int type) const;
    int attachment_id_ = 0;
    void launch_bolt(Bolt b, const ProjectileLooks& looks);   // a hidden one of that type, or a new one
    void pose_trail(size_t object, const glm::vec3& from, const glm::vec3& head, const glm::vec3& dir, float length = -1);
    struct Spark {
        glm::vec3 pos;
        float life;
        glm::vec4 color;
    };

    const Value* component(const LevelObject& o, int comp) const;
    void setup_actor(Target& t, const LevelObject& o);
    bool set_projectile(Target& t, int type);   // its bolts: the projectile type's (CProjectileComponent)
    void play(Target& t, const std::string& clip, bool loop);
    void update_actor(Target& t, float dt);
    void move_along_path(Target& t, float dt, float speed_scale = 1);
    void run_goal(Target& t, float dt);
    void attack_fire(Target& t, const glm::vec3& chest, bool on_player, bool sees, float dt);
    // Cover points (CComponentBuiltinCover 0xb3 entries: offset, facing angle, flags; [6]
    // set: low cover), and the fight from cover (combat_ai.cpp).
    struct CoverPoint {
        glm::vec3 pos{0}, dir{0, 1, 0};
        bool low = true;
        bool left = false, right = false;   // [8]/[9]: its sides to come out of (CCoverPoint bits 4, 8)
        // [7]: one to vault over (bit 2), as far as its facing scaled by the object's (+0x40).
        bool jumpable = false;
        float jump_dist = 1;
        int holder = -1;   // the target index holding it
    };
    std::vector<CoverPoint> covers_;
    void load_covers();
    void read_ai(Target& t, const Value& ai);
    void update_aggressiveness(Target& t, float dt, float dist, bool sees, bool in_cover);
    const Target::Band* band(const Target& t) const;
    bool cover_protects(const CoverPoint& c, const glm::vec3& from) const;
    int choose_cover(const Target& t, const glm::vec3& enemy_feet, const glm::vec3& enemy_eye) const;
    void leave_cover(Target& t);
    void start_root_motion(Target& t, const std::string& clip, bool moves = true, bool reverse = false, float scale = 1);
    bool try_vault(Target& t, const CoverPoint& c);
    bool update_root_motion(Target& t);   // false once the clip is over
    // Dash to (near) `to` if it can (`forced`: a script's order, whatever the wait); the
    // way there must be clear and, when it has an enemy, show it from there.
    bool try_dash(Target& t, const glm::vec3& to, bool forced, const glm::vec3* enemy_chest = nullptr);
    bool try_dash_aside(Target& t, const glm::vec3& enemy_feet, const glm::vec3& enemy_chest);   // TrySSRandom
    void update_dash(Target& t, float dt);
    // CMachineGunAIComponent (gol 0x43): its bullet [0], the turn [3] it covers (degrees,
    // both ways together) and its pitch limits [4]/[5], the gunner's node [6], a shot every
    // [7] ms in bursts of [8]-[9] ms with [10]-[11] ms between, its firing loop [13] and its
    // stop [14].
    struct GunDef {
        float damage = 20, speed = 480;
        ProjectileLooks looks;
        Impacts impacts;
        float turn = 90, pitch_min = -45, pitch_max = 45;
        std::string gunner_node;
        float interval = 0.2f, burst_min = 3, burst_max = 5, pause_min = 1, pause_max = 3;
        std::string loop_sound, stop_sound;
    };
    std::unordered_map<int, GunDef> guns_;
    const GunDef* gun_def(int type);
    void update_gunner(Target& t, float dt, const glm::vec3& chest, bool sees);
    void update_dropship(Target& t, float dt);
    void update_car(Target& t, float dt);
    void car_fire(Target& t, bool on);   // start / stop its gun's firing (clip, sound, shooter)
    // Waypoints flagged as jump points ([9], CWayPointObject::IsJumpPoint) and where each
    // lands (its next waypoint).
    struct JumpPoint {
        int id = 0, landing = -1;   // landing: index into jump_points_
        glm::vec3 pos{0};
    };
    std::vector<JumpPoint> jump_points_;
    void load_jump_points();
    bool try_jump(Target& t, const glm::vec3& enemy_feet, const glm::vec3& enemy_chest);
    bool try_throw(Target& t, const glm::vec3& enemy_feet);
    void update_throw(Target& t, float dt, const glm::vec3& enemy_chest);
    void update_flight(Target& o, float dt);
    void drop_held(Target& t);
    void play_state(Target& t, const std::string& state, bool loop);
    void update_tentacle(Target& t, float dt, const glm::vec3& to);
    void remove_tentacle(Target& t);
    int tentacle_part_type_ = -2;   // gol TentaclePart (-1: none); -2 not looked up
    float tentacle_link_ = 0;       // a link's length (its start to end nodes)
    int tentacle_ids_ = 0;
    // EnterLowCoverRoll: into a low cover about a roll away; false when it can't.
    bool roll_into_cover(Target& t, int cover, const glm::vec3& enemy_eye);
    void set_crouched(Target& t, bool on);
    // The fight from cover; false when the actor has no cover to fight from (the usual
    // chase and attack then).
    bool fight_from_cover(Target& t, float dt, const glm::vec3& target, const glm::vec3& chest, bool sees, bool on_player);
    void path_to(Target& t, const glm::vec3& goal);
    // Run along the nav mesh towards `goal` (repathing as it moves), stopping `stop` away.
    void go_towards(Target& t, const glm::vec3& goal, float stop, float dt);
    void stay_on_mesh(Target& t);
    void face(Target& t, const glm::vec3& towards, float dt);
    static constexpr int kNoEnemy = -2;
    // The enemy's feet and the point to shoot at; false if it is gone.
    bool enemy_points(const Target& t, int enemy, glm::vec3& feet, glm::vec3& chest) const;
    bool sees_point(const Target& t, const glm::vec3& point) const;
    static float sight_range(const Target& t);
    bool fights(const Target& t, int enemy) const;   // a valid enemy for t
    void keep_enemies_on_player();
    int choose_enemy(const Target& t) const;
    Target* find_target(int id);
    const Target* find_target(int id) const { return const_cast<Combat*>(this)->find_target(id); }
    bool line_clear(const glm::vec3& a, const glm::vec3& b) const;
    void notice(Target& t);
    void apply_damage(Target& t, float amount, bool by_player);
    void die(Target& t);
    int best_melee_slot(const Target& t) const;
    bool melee_slot_point(const Target& t, int slot, glm::vec3& out) const;
    void hold_off(Target& t, const glm::vec3& target, float dt);
    static constexpr int kMeleeSlots = 4;   // CTargetableComponent::Init: a radius under 50
    // Start a particle effect at `at` with its +Z along `normal`; false if there is no such effect.
    bool effect(const std::string& name, const glm::vec3& at, const glm::vec3& normal = {0, 0, 1});
    void sound(const std::vector<std::string>& names, const glm::vec3* at);
    void sound(const std::string& name, const glm::vec3* at) { if (!name.empty()) sound(std::vector<std::string>{name}, at); }
    // A projectile hitting something: the material's effect and sound; on the world (`world`), its decal too.
    void impact(const Impacts& im, uint8_t material, const glm::vec3& at, const glm::vec3& normal, bool world = false);
    int fire_loop_ = -1;                // the player's looping fire sound
    std::vector<std::string> player_hurt_sounds_;
    float player_hurt_wait_ = 0;
    // CPlayerBaseComponent [3]: the least time between two jolts (ms); [4]: the damage
    // that makes one heavy.
    float view_hurt_pause_ = 0, view_hurt_heavy_ = 0, view_hurt_wait_ = 0;
    float hit_angle_ = 0, hit_left_ = 0;   // seconds left of the hit indicator
    bool hud_damage_ = false, blood_fade_ = false;
    float hud_blood_ = -1, hud_damage_wait_ = 0;
    float base_shield_percent_ = 50;   // CPlayerBaseComponent [0]: the share of the shield the blood takes over at
    std::string view_hurt_;
    float fall_damage_from_ = 15, fall_damage_per_ = 16;
    std::array<float, 3> landing_speeds_{5, 15, 20};
    int landing_ = -1;
    bool critical_ = false;   // below 30% of the shield at the last hit
    int difficulty_ = 1;
    float base_max_health_ = 150, base_regen_pause_ = 3;
    float difficulty_factor_[7][3] = {};   // DifficultyLevels' lists by difficulty
    void fire_player(const glm::vec3& eye, const glm::vec3& aim);
    // Grenades (CGrenadeComponent of Grenade_Player): thrown along the view, they bounce
    // off the level and explode when the fuse runs out, hurting everything in the radius.
    struct GrenadeDef {
        int type = -1;
        float fuse = 2.3f, damage = 40, radius = 10, speed = 25;
        bool sticky = false;           // a mine: stuck where it lands, set off by the detonation
        bool burst_on_actor = false;   // [12] explodes on collision with 22 (characters): the launcher's
        std::string effect = "explosion", trail, bounce_sound;
        std::vector<std::string> explode_sounds;
        DecalTable decals;             // [19]: its scorch mark (CGrenadeComponent::PlaceDecal)
    };
    struct Grenade {
        size_t object = size_t(-1);
        const GrenadeDef* def = nullptr;
        bool npc = false;              // an enemy's (NPC_Grenade): it hurts the player, the HUD warns of it
        bool stuck = false;            // a mine on a surface, waiting for the detonation
        glm::vec3 pos{0}, vel{0};
        float fuse = 0, bounce_wait = 0, spin = 0;
        int trail = -1;
        bool live = false;
    };
    void load_grenade();
    static GrenadeDef read_grenade(const Gol& gol, int type);
    void throw_grenade(const glm::vec3& eye, const glm::vec3& aim);
    void launch_grenade(const GrenadeDef& def, const glm::vec3& at, const glm::vec3& vel, bool npc);
    // An NPC's throw (CNPCComponent::ThrowGrenadeFromCover): lobbed to land at the target.
    void npc_throw_grenade(Target& t, const glm::vec3& target);

    // NPC barks (NPCBarksManager; gol NPCBarksManager comp 0xa5: [0] seconds between any
    // two, [2] groups {NPC types, barks {id, seconds before the same again, sounds}}): a
    // bark of an NPC's group plays at its head, a random sound but not the last one, when
    // no bark has played for the gap (counted from the end of the last) and the same bark
    // not for its own time. Without an NPC, the hostile one nearest the player that has it.
    struct BarkSet {
        int id = 0;
        float interval = 5;
        std::vector<std::string> sounds;
        float next = 0;
        int last = -1;
    };
    struct BarkGroup {
        std::vector<int> types;
        std::vector<BarkSet> barks;
    };
    std::vector<BarkGroup> bark_groups_;
    float bark_gap_ = 1, bark_clock_ = 0, bark_next_ = 0;
    int bark_voice_ = -1;
    BarkSet* bark_playing_ = nullptr;
    void load_barks();
    BarkSet* bark_set(const Target& t, int id);
    void bark(int id, const Target* npc);
    void update_barks(float dt);
    void acquire_bark(const Target& t);
    std::string last_aggro_;   // CAIController +0xc4: the last aggro sound any NPC played
    void aggro_sound(const Target& t);   // CNPCComponent::SetCurrentTarget, from none
    GrenadeDef npc_grenade_def_;
    std::map<int, GrenadeDef> fired_grenades_;   // weapons firing grenades (the grenade launcher), per type
    void update_grenades(float dt);
    void explode(const Grenade& g);
    GrenadeDef grenade_def_, mine_def_;
    bool throw_mines_ = false, throwing_mine_ = false;
    int throws_ = 0, switches_ = 0, detonations_ = 0;
    void detonate();
    std::vector<Grenade> live_grenades_;
    float throw_delay_ = -1, grenade_cooldown_ = 0;
    // CAIController's global ticket 1 (CComponentAIController [4] ms, [5] at a time): one
    // NPC grenade, then none for 2 s; the first after 2 s.
    static constexpr float kGrenadeTicketWait = 2.0f;
    float grenade_ticket_ = kGrenadeTicketWait;
    static constexpr float kPowerCooldown = 5.0f;
    float power_cooldown_ = 0;
    bool power_used_ = false;
    std::vector<std::string> powers_{"Repel"};
    int power_index_ = 0;
    void repel(const glm::vec3& eye, const glm::vec3& aim);
    // Pick-ups (CPowerUpComponent, gol comp 0x4a: weapon, power, ?, ammo only, amount,
    // radius, seconds to come back, sound, ?, switch to it): walked into, they give a weapon's
    // ammunition (and the weapon, unless ammo only), or grenades or mines (pickups.cpp).
    struct Pickup {
        int id = 0;
        int weapon = -1;            // in the arsenal
        int power = -1;             // 1 grenades, 2 mines
        bool unlocks = true, switch_to = false;
        int amount = 0;
        float radius = 2, regenerate = -1;
        std::string sound;
        float gone = -1;            // seconds since taken, while it comes back
    };
    std::vector<Pickup> pickups_;
    PickupInfo pickup_info_;
    void load_pickup(int id, const LevelObject& o, const Value& v);
    // Breakables (CBreakableComponent; gol comp 0x13: health, damage stages {share of health
    // left in %, state, collision off}, capsules {health, state, length, radius, position,
    // rotation, node}): glass panes and consoles. With capsules, a shot through one takes
    // their health (the first's) and at none sets that capsule's state; without, a shot on
    // its collision or a blast takes its own, each stage it falls to setting its state on.
    struct Breakable {
        int id = 0, type = 0;
        size_t object = 0;
        float health = 0, max_health = 1, stage = 100;
        float capsule_health = 0;
        struct Stage {
            float below = 0;
            int state = -1;
            bool collision_off = false;
        };
        std::vector<Stage> stages;
        struct Capsule {
            glm::vec3 a{0}, b{0};
            float radius = 0;
            int state = -1;
        };
        std::vector<Capsule> capsules;
    };
    std::vector<Breakable> breakables_;
    // Damage zones (CDamageComponent, gol 0x99: ..., [10] enabled, [11] damage a second, [12]
    // its kind, [13] factors by type, [15] teams it hurts (0: all), [16] a sound): a box (the
    // object's CComponentBuiltinBox 0xa6: width, depth, height up from its base) scaled and
    // turned with the object; the player inside is hurt (CDamageComponent::Update): kill
    // volumes under the levels, electric arcs, fire.
    struct DamageZone {
        int id = 0;
        glm::mat4 to_local{1};
        glm::vec3 lo{0}, hi{0};      // the box in its own frame
        float per_second = 0;
        uint32_t teams = 0;
        std::string sound;
        int sound_handle = -1;
    };
    std::vector<DamageZone> damage_zones_;
    void load_damage_zone(int id, const LevelObject& o, const Value& v);
    void update_damage_zones(float dt);
    void load_breakable(int id, size_t object, const LevelObject& o, const Value& v);
    // A shot from `from` to `to` (where it stopped: on `owner`'s collision, if `wall`).
    void shoot_breakables(const glm::vec3& from, const glm::vec3& to, float damage, bool wall, uint32_t owner);
    void damage_breakable(Breakable& b, float damage);
    void set_breakable_state(Breakable& b, int state);
    void update_pickups(float dt);
    bool take_pickup(const Pickup& p);
    void paralyze(const glm::vec3& eye, const glm::vec3& aim);
    void use_selected_power(const glm::vec3& eye, const glm::vec3& aim);
    bool throw_started_ = false;
    void fire_bolt(Target& t, const glm::vec3& at);
    // CNPCWeaponComponent::NeedReload: fewer rounds than `shots` shots (as many as a
    // magazine holds at most).
    bool need_reload(const Target& t, int shots) const;
    void refill(Target& t);   // the magazine full again, with the reload sound
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
    struct Beam {
        glm::vec3 from, to;
        float width, age = 0;
        std::string texture;
    };
    std::vector<Beam> beams_;
    std::unordered_map<int, std::vector<size_t>> trail_pool_;   // hidden trail and projectile objects, per type
    glm::vec3 muzzle_{0};
    bool has_muzzle_ = false;
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
    unsigned shots_ = 0;
    float fire_cooldown_ = 0, reload_left_ = 0;
    bool reload_filled_ = false;
    int low_shield_voice_ = -1;   // sfx_shield_low playing
    int scratch_ = -1;
    // An NPC's blow (CNPCComponent::GiveMeleeAttackDamage, PlayerComponent::ReceiveDamage):
    // damage type 5/6 a fist, 11/12 a spider's left/right claw (its scratch and camera jolt);
    // the type's impact sound (PlayerComponent [melee sounds]).
    void melee_hit_player(float amount, const glm::vec3& from, int type);
    std::unordered_map<int, std::vector<std::string>> melee_sounds_;
    void start_melee(Target& t);
    bool update_red_fury(Target& t, float dt, const glm::vec3& feet, const glm::vec3& chest, bool sees);
    void red_fury_pick_point(Target& t, const glm::vec3& feet);
    bool update_spider_jump(Target& t, float dt, const glm::vec3& feet, bool on_player, bool sees, float dist);
    int mech_ = -1;                // the mech piloted (its health is the player's)
    float own_health_ = 0, own_max_health_ = 0;
    // The player's lock-on: the target (index, -1 none), time on it, locked, the lock's
    // hold after locking (CWeaponComponent +0x1ec, 5 s), and the RLTarget marker.
    int lock_target_ = -1;
    float lock_time_ = 0, lock_hold_ = 0;
    bool locked_ = false;
    size_t lock_marker_ = size_t(-1);
    std::string lock_marker_node_;
    void update_lock(float dt, const glm::vec3& eye, const glm::vec3& aim);
    void stop_lock();
    // The spread (degrees; CWeaponComponent +0x5c), whether aiming, where the player was
    // (walking or not), the crosshair's fade (0..1) and fire kick (0..1, +0x1fc), and how
    // far off what the crosshair is on is (-1: nothing within 200 m).
    float spread_ = 1;
    bool aiming_ = false;
    glm::vec3 last_center_{0};
    float cross_alpha_ = 0, cross_kick_ = 0, aim_distance_ = -1;
    bool trigger_held_ = false;
    float regen_delay_ = 0, regen_rate_ = 20, regen_pause_ = 3.0f;
    float hit_marker_ = 0, dead_timer_ = 0, protect_ = 0;
    bool killing_headshot_ = false, killing_repel_ = false;   // what the damage being dealt is
    enum class Cause { Shot, Explosion, Fire } damage_cause_ = Cause::Shot;
    // What the damage being dealt comes from: the weapon or projectile's gol type (for the
    // death it brings, RagdollsSettings), its direction, and an explosion's centre.
    int damage_source_ = -1;
    glm::vec3 damage_dir_{0}, blast_at_{0};
    RagdollSettings ragdolls_;
    bool start_ragdoll(Target& t);
    const glm::vec3* damage_point_ = nullptr;   // where a shot struck, for the hurt animation
    // One of the clips of a state of the target's type (StateSetMarine's DeathHeadShot,
    // HurtLeftLeg, ...) that its animation has; empty if none.
    std::string state_variant(const Target& t, const std::string& state);
    int max_shooters_ = 2;   // enemies firing at the player at once (attack tokens; SetMaxActiveEnemies;
                             // CComponentAIController [0] of gol AIController, as min_enemies_on_player is [1])
    static constexpr float kBurstInterval = 1.2f;   // seconds between any two bursts starting
    float burst_cooldown_ = 0;
    glm::vec3 last_eye_{0}, last_aim_{0, 1, 0};
    bool aim_on_enemy_ = false;
    std::mt19937 rng_{1234};
    Stats stats_;
};

}  // namespace nova
