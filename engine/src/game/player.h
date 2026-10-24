// First-person player movement on a level's collision.
#pragma once

#include <glm/glm.hpp>

#include "game/physics.h"

namespace nova {

struct PlayerInput {
    float forward = 0, right = 0;   // -1..1
    bool jump = false, run = false;
};

class Player {
public:
    // Ellipsoid radii (metres): 0.8 m wide, 1.8 m tall. Eye 0.75 m above the centre.
    glm::vec3 radius{0.4f, 0.4f, 0.9f};
    float eye_height = 0.75f;
    float walk_speed = 5.0f, run_speed = 9.0f;
    // CComponentPlayerComponent (gol 0x2a, set by Combat): gravity [13], the fall speed's
    // limit [6], and the jump: an upward push of jump_speed * (time left / jump_time)^2
    // for jump_time seconds ([14], [15]) while gravity builds up (CCharacterController::Move).
    float gravity = 9.8f, jump_speed = 8.0f, jump_time = 0.8f, max_fall_speed = 30.0f;
    float step_snap = 0.3f;         // when grounded, follow the floor down this far per step
    // PlayerComponent::ComputeStepTimer: ms between footsteps, [72] slow to [71] at full speed.
    float step_min_ms = 400, step_max_ms = 600;

    glm::vec3 center{0};
    glm::vec3 push{0};              // an outside pull, m/s (SetAttractionDirection), added to the walk
    float yaw = 0, pitch = 0;       // degrees; yaw 0 looks along +Y, positive turns left
    float vertical_speed = 0;
    float fall_speed = 0;           // what gravity has built up since leaving the ground
    float jump_left = 0;            // seconds of the jump's push left
    uint32_t jumps = 0;             // bumped by each jump off the ground
    bool on_ground = false;
    float landing_speed = 0;   // gravity's speed at the last landing (taken by Combat)
    bool jetpack = false;      // the jetpack power: jumping thrusts upwards, falls are slowed
    bool thrusting = false;    // the jetpack firing (the jump held), as of the last update
    bool rushing = false;      // sprinting (run held while moving), as of the last update
    // Wedged (pushing but hardly moving): move by Irrlicht's one-sided rule for a while.
    float stuck_time = 0, escape_time = 0;

    // Place the player standing on the floor at `feet`.
    void spawn(const glm::vec3& feet, float yaw_deg);
    uint32_t spawns = 0;   // bumped by spawn(): the view should take the player's yaw/pitch
    void update(const CollisionWorld& world, const PlayerInput& in, float dt);

    glm::vec3 eye() const { return center + glm::vec3(0, 0, eye_height); }
    glm::vec3 view_forward() const;
};

}  // namespace nova
