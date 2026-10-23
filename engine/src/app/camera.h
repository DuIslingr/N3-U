// The game's view: position, yaw/pitch (or a direction, for cinematic cameras), field of view.
#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "render/renderer.h"

namespace nova {

// The game's field of view (CLevel::SetTargetFov: the camera gets fov * height / width
// degrees vertically; 70 by default). Aim mode narrows it.
constexpr float kFov = 70, kAimZoom = 0.65f;

struct Camera {
    glm::vec3 position{0};
    float yaw = 0, pitch = 0;   // degrees; yaw 0 looks along +Y, positive yaw turns left (game convention)
    float fov = kFov;
    bool game_fov = true;       // fov in the game's units (else vertical degrees: cinematic cameras)
    float zfar = 8000;
    // A camera that looks along a given direction (cinematics) instead of yaw/pitch.
    bool directed = false;
    glm::vec3 direction{0, 1, 0}, up{0, 0, 1};

    glm::vec3 forward() const {
        if (directed) return direction;
        const float y = glm::radians(yaw), p = glm::radians(pitch);
        return {-std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p)};
    }
    // Seen through the view model's camera layer (a hurt jolt): `offset` in the view's
    // frame (x right, y forward, z up).
    void jolt(const glm::mat4& offset) {
        if (directed || offset == glm::mat4(1)) return;
        const glm::mat4 look = glm::rotate(glm::rotate(glm::mat4(1), glm::radians(yaw), glm::vec3(0, 0, 1)), glm::radians(pitch),
                                           glm::vec3(1, 0, 0)) * offset;
        position += glm::vec3(look[3]);
        directed = true, direction = glm::vec3(look[1]), up = glm::vec3(look[2]);
    }
    FrameParams frame(int w, int h) const {
        FrameParams f;
        f.view = glm::lookAt(position, position + forward(), directed ? up : glm::vec3(0, 0, 1));
        // Reversed depth (near -> 1, far -> 0) spreads the float depth buffer's precision
        // evenly; near and far are swapped on purpose.
        const float vertical = game_fov ? fov * float(h) / float(std::max(w, 1)) : fov;
        f.proj = glm::perspectiveRH_ZO(glm::radians(vertical), float(w) / float(std::max(h, 1)), zfar, 0.05f);
        f.camera = position;
        return f;
    }
};

}  // namespace nova
