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
    float gravity = 18.0f, jump_speed = 6.0f, max_fall_speed = 50.0f;
    float step_snap = 0.3f;         // when grounded, follow the floor down this far per step

    glm::vec3 center{0};
    float yaw = 0, pitch = 0;       // degrees; yaw 0 looks along +Y, positive turns left
    float vertical_speed = 0;
    bool on_ground = false;
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
