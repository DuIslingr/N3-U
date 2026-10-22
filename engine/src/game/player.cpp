#include "game/player.h"

#include <algorithm>
#include <cmath>

namespace nova {

void Player::spawn(const glm::vec3& feet, float yaw_deg) {
    center = feet + glm::vec3(0, 0, radius.z + 0.05f);
    yaw = yaw_deg;
    pitch = 0;
    vertical_speed = 0;
    on_ground = false;
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

    if (on_ground && in.jump) {
        vertical_speed = jump_speed;
        on_ground = false;
    }
    vertical_speed = std::max(vertical_speed - gravity * dt, -max_fall_speed);

    glm::vec3 vertical(0, 0, vertical_speed * dt);
    const bool rising = vertical_speed > 0;
    if (on_ground && !rising) vertical.z = std::min(vertical.z, -step_snap);   // stay on stairs and slopes

    const auto r = world.move(center, radius, step, vertical);
    if (rising) {
        // A rising sweep that hits something is a ceiling, not a floor.
        if (!r.falling) vertical_speed = 0;
        on_ground = false;
    } else {
        on_ground = !r.falling;
        if (on_ground) vertical_speed = 0;
    }
    center = r.position;
}

}  // namespace nova
