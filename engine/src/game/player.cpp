#include "game/player.h"

#include <algorithm>
#include <cmath>

namespace nova {

void Player::spawn(const glm::vec3& feet, float yaw_deg) {
    center = feet + glm::vec3(0, 0, radius.z + 0.05f);
    yaw = yaw_deg;
    pitch = 0;
    ++spawns;
    vertical_speed = 0, landing_speed = 0;
    on_ground = false;
    stuck_time = escape_time = 0;
}

glm::vec3 Player::view_forward() const {
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return {-std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p)};
}

void Player::update(const CollisionWorld& world, const PlayerInput& in, float dt) {
    // Horizontal intent from the view yaw (pitch doesn't affect walking).
    const float y = glm::radians(yaw);
    const glm::vec3 fwd(-std::sin(y), std::cos(y), 0);
    const glm::vec3 right(std::cos(y), std::sin(y), 0);
    glm::vec3 wish = fwd * in.forward + right * in.right;
    if (glm::length(wish) > 1.0f) wish = glm::normalize(wish);
    const glm::vec3 step = wish * (in.run ? run_speed : walk_speed) * dt;

    if (jetpack) {
        // Thrust while the jump is held (it beats gravity); otherwise a slow descent.
        if (in.jump) vertical_speed = std::min(vertical_speed + (gravity + 14.0f) * dt, 7.0f), on_ground = false;
        vertical_speed = std::max(vertical_speed - gravity * dt, -6.0f);
    } else {
        if (on_ground && in.jump) {
            vertical_speed = jump_speed;
            on_ground = false;
        }
        vertical_speed = std::max(vertical_speed - gravity * dt, -max_fall_speed);
    }

    glm::vec3 vertical(0, 0, vertical_speed * dt);
    const bool rising = vertical_speed > 0;
    if (on_ground && !rising) vertical.z = std::min(vertical.z, -step_snap);   // stay on stairs and slopes

    const auto r = world.move(center, radius, step, vertical, 0.0005f, escape_time > 0);
    // Stuck detection: asked to move, but went less than a fifth of the way.
    const float wanted = glm::length(glm::vec2(step)), moved = glm::length(glm::vec2(r.position - center));
    if (wanted > 1e-4f && moved < wanted * 0.2f) stuck_time += dt;
    else stuck_time = std::max(stuck_time - dt, 0.0f);
    escape_time = std::max(escape_time - dt, 0.0f);
    if (stuck_time > 0.6f) escape_time = 0.5f, stuck_time = 0;
    if (rising) {
        // A rising sweep that hits something is a ceiling, not a floor.
        if (!r.falling) vertical_speed = 0;
        on_ground = false;
    } else {
        const bool was_on_ground = on_ground;
        on_ground = !r.falling;
        if (on_ground && !was_on_ground) landing_speed = -vertical_speed;
        if (on_ground) vertical_speed = 0;
    }
    center = r.position;
}

}  // namespace nova
