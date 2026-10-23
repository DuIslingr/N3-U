// LevelLogic's limited control (CPlayerLimitedControlComponent): the PlayerLimitedControl
// rig and its states. See level_logic.h.
#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "game/anim_sounds.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace nova {

void LevelLogic::limited_begin() {
    if (limited_.rig == size_t(-1) || limited_.on) return;
    limited_.on = true;
    limited_.gravity = true;
    limited_.strafe = limited_.defaults[0], limited_.limit_yaw = limited_.defaults[1], limited_.limit_pitch = limited_.defaults[2];
    limited_.base_yaw = player.yaw;
    limited_.yaw = limited_.pitch = limited_.idle = 0;
    scene.set_object_visible(limited_.rig, true);
    place_rig();
    limited_state("Init");
}

void LevelLogic::limited_end() {
    if (!limited_.on) return;
    // CPlayerLimitedControlComponent::Update at "End": the player where the camera is,
    // looking its way.
    Scene::CameraView cv;
    if (limited_view(cv)) {
        player.spawn(cv.position - glm::vec3(0, 0, player.eye_height + player.radius.z), glm::degrees(std::atan2(-cv.forward.x, cv.forward.y)));
        player.pitch = glm::degrees(std::asin(glm::clamp(cv.forward.z, -1.0f, 1.0f)));
    }
    limited_.on = false;
    limited_.state.clear();
    scene.set_object_visible(limited_.rig, false);
}

void LevelLogic::limited_state(const std::string& state) {
    if (limited_.rig == size_t(-1)) return;
    const AnimSounds::State* st = anim_sounds_->state(limited_.rig_type, state);
    if (!st) {
        log("limited control: no state " + state);
        return;
    }
    if (trace) log("limited control state " + state);
    limited_.state = state;
    limited_.time = 0, limited_.next_script = 0;
    limited_.clip_length = 0;
    if (!st->clip.empty() && scene.play_clip(limited_.rig, st->clip, st->loop)) limited_.clip_length = scene.clip_length(limited_.rig, st->clip);
}

void LevelLogic::limited_hang(int id) {
    const Object* o = object(id);
    if (!o || !limited_.on) return;
    // CPlayerLimitedControlComponent::StartHang: along its heading onto the plane through
    // the object facing its way, then Hang.
    const float y = glm::radians(limited_.base_yaw);
    const glm::vec3 d(-std::sin(y), std::cos(y), 0);
    glm::vec3 n = glm::vec3(scene.object_matrix(o->index)[1]);
    if (glm::length(n) > 1e-4f) {
        n = glm::normalize(n);
        if (const float den = glm::dot(d, n); std::abs(den) > 1e-4f) player.center += d * (glm::dot(o->position - player.center, n) / den);
    }
    place_rig();
    limited_state("Hang");
}

bool LevelLogic::limited_tap() {
    if (!limited_.on || limited_.state != "Minigame") return false;
    limited_state("Win");
    return true;
}

void LevelLogic::update_limited(float dt) {
    if (!limited_.on) return;
    limited_.time += dt;
    if (const AnimSounds::State* st = anim_sounds_->state(limited_.rig_type, limited_.state)) {
        // Script tracks run at their times into the state (some after its clip has ended).
        std::vector<std::pair<float, std::string>> scripts = st->scripts;
        std::sort(scripts.begin(), scripts.end());
        while (limited_.next_script < scripts.size() && scripts[limited_.next_script].first <= limited_.time * 1000.0f)
            start_script(scripts[limited_.next_script++].second, kPlayerId);
        if (!st->loop && limited_.clip_length > 0 && limited_.time >= limited_.clip_length && !st->next.empty()) {
            limited_state(st->next);
            return;
        }
    }
    if (limited_.state == "End") {
        limited_end();
        return;
    }
    if (auto_qte && limited_.state == "Minigame" && limited_.time > 0.5f) limited_tap();
}

bool LevelLogic::limited_step(float right, float dt) {
    if (!limited_.on) return false;
    glm::vec3 v = player.push;   // the level's attraction (m/s)
    const std::string& s = limited_.state;
    if ((s == "Idle" || s == "MoveLeft" || s == "MoveRight") && right != 0) {
        const char* want = right < 0 ? "MoveLeft" : "MoveRight";
        if (s != want) limited_state(want);
        // The original moves strafe x 0.001 x frame ms per ms: at its 30 fps, strafe x 33 m/s.
        const float y = glm::radians(limited_.base_yaw);
        v += glm::vec3(std::cos(y), std::sin(y), 0) * (right < 0 ? -1.0f : 1.0f) * limited_.strafe * 33.0f;
    }
    if (limited_.gravity) {
        const glm::vec3 pull = player.push;
        player.push = v;
        player.update(scene.collision(), PlayerInput{}, dt);
        player.push = pull;
    } else {
        player.center += v * dt;
        player.vertical_speed = 0;
    }
    place_rig();
    return true;
}

void LevelLogic::place_rig() {
    // At the player's height less 0.3 m (CPlayerLimitedControlComponent::Update), facing
    // where the player faced when it began.
    const glm::vec3 at = player.center + glm::vec3(0, 0, -player.radius.z + 1.8f - 0.3f);
    scene.set_object_transform(limited_.rig, glm::rotate(glm::translate(glm::mat4(1), at), glm::radians(limited_.base_yaw), glm::vec3(0, 0, 1)));
}

void LevelLogic::limited_view_input(float& yaw, float& pitch, float dt) {
    if (!limited_.on) return;
    float oy = std::fmod(yaw - limited_.base_yaw + 540.0f, 360.0f) - 180.0f, op = pitch;
    if (std::abs(oy - limited_.yaw) > 1e-3f || std::abs(op - limited_.pitch) > 1e-3f) {
        limited_.idle = 0;
    } else if ((limited_.idle += dt) > 2.0f) {   // untouched for 2 s: back to the middle at 10 deg/s
        oy -= std::clamp(oy, -10.0f * dt, 10.0f * dt);
        op -= std::clamp(op, -10.0f * dt, 10.0f * dt);
    }
    // SetCameraYaw/SetCameraPitch: within the ellipse of the limits.
    const float ly = std::max(limited_.limit_yaw, 1e-3f), lp = std::max(limited_.limit_pitch, 1e-3f);
    if (const float k = (oy / ly) * (oy / ly) + (op / lp) * (op / lp); k > 1) oy /= std::sqrt(k), op /= std::sqrt(k);
    limited_.yaw = oy, limited_.pitch = op;
    yaw = limited_.base_yaw + oy, pitch = op;
}

bool LevelLogic::limited_view(Scene::CameraView& out) const {
    if (!limited_.on || !scene.camera_view(limited_.rig, out)) return false;
    const glm::vec3 right = glm::normalize(glm::cross(out.forward, out.up));
    const glm::mat3 turn = glm::mat3(glm::rotate(glm::mat4(1), glm::radians(limited_.yaw), glm::vec3(0, 0, 1))) *
                           glm::mat3(glm::rotate(glm::mat4(1), glm::radians(limited_.pitch), right));
    out.forward = turn * out.forward, out.up = turn * out.up;
    return true;
}

}  // namespace nova
