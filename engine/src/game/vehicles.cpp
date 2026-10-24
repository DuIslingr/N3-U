// LevelLogic's vehicles: scripted driving along waypoint chains, the player riding one or
// manning a mounted gun. See level_logic.h.
#include <algorithm>
#include <cmath>

#include "game/level_logic.h"
#include "game/combat.h"
#include "game/physics.h"
#include "game/player.h"
#include "game/scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace nova {

namespace {

float angle_diff(float from, float to) {   // degrees, -180..180
    float d = std::fmod(to - from + 540.0f, 360.0f) - 180.0f;
    return d;
}

}  // namespace

LevelLogic::Vehicle* LevelLogic::vehicle(int id) {
    if (auto it = vehicles_.find(id); it != vehicles_.end()) return &it->second;
    Object* o = object(id);
    if (!o) return nullptr;
    Vehicle& v = vehicles_[id];
    v.id = id;
    v.index = o->index;
    v.position = o->position;
    v.yaw = o->yaw;
    spawn_vehicle_parts(v);
    place_vehicle_parts(v);
    return &v;
}

void LevelLogic::spawn_vehicle_parts(Vehicle& v) {
    const Object* o = object(v.id);
    const Value* car = o ? object_component(*o, 0x96) : nullptr;
    if (!car || car->size() < 25) return;
    // [0] gun, [1] shooter, [2] driver types ... [24] wheels: [type, front, node, effect node, radius...].
    auto spawn = [&](int type, const std::string& node, Vehicle::Part::Kind kind, bool front = false) {
        if (type < 0 || !gol.count(type)) return;
        const size_t index = scene.spawn_object(type, -4000 - int(scene.level().objects.size()), v.position, v.yaw);
        scene.set_object_visible(index, true);
        v.parts.push_back({index, node, kind, front, {}});
    };
    spawn((*car)[0].i, "dummy_afet_support", Vehicle::Part::Kind::Gun);
    spawn((*car)[1].i, "dummy_shooter", Vehicle::Part::Kind::Rider);
    spawn((*car)[2].i, "dummy_driver", Vehicle::Part::Kind::Rider);
    for (const Value& w : (*car)[24].list)
        if (w.size() > 2 && w[2].type == Value::String) spawn(w[0].i, w[2].s, Vehicle::Part::Kind::Wheel, w[1].i != 0);
}

void LevelLogic::place_vehicle_parts(Vehicle& v) {
    const Object* o = object(v.id);
    const bool shown = o && o->visible;
    const bool ridden = v.id == player_vehicle_;
    // The gun turned (by the rider's view, or its AI): the shooter platform (the gun's
    // mount and the shooter) turns with it about the vehicle's up axis.
    const bool turned = (ridden || v.gun_aimed) && !v.destroyed;
    const float yaw = ridden ? angle_diff(v.yaw, player.yaw) : v.gun_yaw, pitch = ridden ? player.pitch : v.gun_pitch;
    if (v.platform == -2) {
        v.platform = scene.node_index(v.index, "dummy_platform_shooter");
        if (v.platform >= 0) {
            const glm::mat4 car = scene.object_matrix(v.index), node = scene.object_matrix(v.index, "dummy_platform_shooter");
            v.platform_rest = glm::normalize(glm::quat_cast(glm::mat3(glm::inverse(car) * node)));
        }
    }
    if (v.platform >= 0 && turned) {
        std::vector<glm::quat> layer(scene.node_names(v.index).size(), glm::quat(1, 0, 0, 0));
        if (size_t(v.platform) < layer.size())
            layer[size_t(v.platform)] = glm::inverse(v.platform_rest) * glm::angleAxis(glm::radians(yaw), glm::vec3(0, 0, 1)) * v.platform_rest;
        scene.set_additive(v.index, std::move(layer));
    } else if (v.platform >= 0) {
        scene.set_additive(v.index, {});
    }
    for (Vehicle::Part& p : v.parts) {
        // The player's place: the shooter is the player. A destroyed one's riders are gone.
        const bool show = shown && !(ridden && p.node == "dummy_shooter") && !(v.destroyed && p.kind == Vehicle::Part::Kind::Rider);
        scene.set_object_visible(p.object, show);
        if (!show) continue;
        glm::mat4 m = scene.object_matrix(v.index, p.node);
        if (p.kind == Vehicle::Part::Kind::Wheel) {
            if (p.front) m = glm::rotate(m, glm::radians(v.steer), glm::vec3(0, 0, 1));
            m = glm::rotate(m, v.wheel_angle, glm::vec3(1, 0, 0));
        } else if (p.node == "dummy_driver" && p.object < scene.level().objects.size()) {
            // CVehicleCar::UpdateMeshes2: the driver turns the wheel (DriveTurnLeft/Right,
            // posed by how far the wheels are turned), else DriveIdle.
            const int type = scene.level().objects[p.object].type;
            const float r = v.steer_max > 0 ? std::clamp(v.steer / v.steer_max, -1.0f, 1.0f) : 0.0f;
            const std::string clip = state_clip(type, std::abs(r) < 1e-3f ? "DriveIdle" : r > 0 ? "DriveTurnLeft" : "DriveTurnRight");
            if (!clip.empty()) {
                if (clip != p.pose) scene.play_clip(p.object, clip, std::abs(r) < 1e-3f), p.pose = clip;
                if (std::abs(r) >= 1e-3f) scene.set_clip_time(p.object, scene.clip_length(p.object, clip) * std::abs(r));
            }
        } else if (p.node == "dummy_shooter" && p.object < scene.level().objects.size() &&
                   std::abs((turned ? pitch : 0.0f) - p.posed_pitch) > 0.25f) {
            // CVehicleCar::UpdateShooterStates: ShootUp / ShootDown held |pitch| / 30 degrees
            // into its clip on a second slot; here the turn from its first frame, layered.
            p.posed_pitch = turned ? pitch : 0.0f;
            const float f = std::min(std::abs(p.posed_pitch) / 30.0f, 1.0f);
            const std::string clip = state_clip(scene.level().objects[p.object].type, p.posed_pitch > 0 ? "ShootUp" : "ShootDown");
            std::vector<glm::quat> from, to;
            float length = 0;
            if (f > 0.01f && !clip.empty() && scene.sample_rotations(p.object, clip, 0, from, &length) &&
                scene.sample_rotations(p.object, clip, std::max(length - 1, 0.0f) * f, to)) {
                for (size_t n = 0; n < from.size(); ++n) to[n] = glm::normalize(glm::inverse(from[n]) * to[n]);
                scene.set_additive(p.object, std::move(to));
            } else {
                scene.set_additive(p.object, {});
            }
        } else if (p.kind == Vehicle::Part::Kind::Gun && turned) {
            m = glm::translate(glm::mat4(1), glm::vec3(m[3]));
            m = glm::rotate(m, glm::radians(v.yaw + yaw), glm::vec3(0, 0, 1));
            m = glm::rotate(m, glm::radians(pitch), glm::vec3(1, 0, 0));
        }
        scene.set_object_transform(p.object, m);
    }
}

bool LevelLogic::vehicle_gun(int id, size_t& gun, size_t& shooter) {
    Vehicle* v = vehicle(id);
    if (!v) return false;
    gun = shooter = size_t(-1);
    for (const Vehicle::Part& p : v->parts) {
        if (p.kind == Vehicle::Part::Kind::Gun) gun = p.object;
        if (p.node == "dummy_shooter") shooter = p.object;
    }
    return gun != size_t(-1);
}

void LevelLogic::aim_vehicle_gun(int id, float yaw, float pitch) {
    if (Vehicle* v = vehicle(id)) v->gun_aimed = true, v->gun_yaw = yaw, v->gun_pitch = pitch;
}

void LevelLogic::destroy_vehicle(int id) {
    Vehicle* v = vehicle(id);
    if (!v) return;
    v->destroyed = true, v->moving = false, v->speed = 0, v->follow = -1;
    place_vehicle_parts(*v);
}

bool LevelLogic::vehicle_move_on_path(int id, int waypoint) {
    Vehicle* v = vehicle(id);
    if (!v || v->destroyed) return false;
    v->route.clear();
    std::unordered_set<int> seen;
    for (int wp = waypoint; wp >= 0 && seen.insert(wp).second;) {
        const Object* w = object(wp);
        if (!w) break;
        v->route.push_back(w->position);
        auto it = waypoint_next_.find(wp);
        wp = it == waypoint_next_.end() ? -1 : it->second;
    }
    v->next = 0;
    v->moving = !v->route.empty();
    return v->moving;
}

void LevelLogic::vehicle_stop(int id) {
    if (Vehicle* v = vehicle(id)) v->moving = false, v->speed = 0, v->follow = -1;
}

bool LevelLogic::vehicle_follow(int id, int target, float metres) {
    Vehicle* v = vehicle(id);
    if (!v || (target != kPlayerId && !object(target))) return false;
    v->follow = target, v->follow_distance = metres;
    v->route.clear(), v->next = 0;
    v->moving = true;
    return true;
}

void LevelLogic::vehicle_set(int id, float max_speed_kmh, float steer_max, float steer_speed) {
    Vehicle* v = vehicle(id);
    if (!v) return;
    if (max_speed_kmh >= 0) v->max_speed = max_speed_kmh / 3.6f;
    if (steer_max >= 0) v->steer_max = steer_max;
    if (steer_speed >= 0) v->steer_speed = steer_speed;
}

void LevelLogic::vehicle_yaw_limits(int id, float min, float max, float ms) {
    Vehicle* v = vehicle(id);
    if (!v) return;
    const float t = v->limits_len > 0 ? std::min(v->limits_t / v->limits_len, 1.0f) : 1.0f;
    v->from_min = v->from_min + (v->yaw_min - v->from_min) * t;
    v->from_max = v->from_max + (v->yaw_max - v->from_max) * t;
    v->yaw_min = min, v->yaw_max = max;
    v->limits_t = 0, v->limits_len = ms / 1000.0f;
}

bool LevelLogic::enter_vehicle(int id) {
    if (!vehicle(id)) return false;
    player_vehicle_ = id, player_gun_ = -1;
    if (combat) combat->use_vehicle_weapon(true);
    return true;
}

bool LevelLogic::man_gun(int id) {
    if (!object(id)) return false;
    gun_yaw_ = object(id)->yaw;
    player_gun_ = id, player_vehicle_ = -1;
    if (combat) combat->use_mounted_gun(true);
    return true;
}

bool LevelLogic::enter_mech(int id) {
    Object* o = object(id);
    if (!o) return false;
    player_mech_ = id, player_vehicle_ = player_gun_ = -1;
    player.spawn(o->position, o->yaw);   // the player stands where the mech stood, as the mech
    scene.set_object_visible(o->index, false);
    if (combat) combat->use_mech_weapon(true);
    return true;
}

void LevelLogic::leave_vehicle() {
    if (limited_.on) limited_end();   // SwitchToPlayerOnFoot ends limited control too
    if (Object* m = player_mech_ >= 0 ? object(player_mech_) : nullptr) {
        // The mech stays where it was walked to; the player steps out in front.
        m->position = player.center - glm::vec3(0, 0, player.radius.z), m->yaw = player.yaw;
        scene.set_object_pose(m->index, m->position, m->yaw);
        scene.set_object_visible(m->index, m->visible);
        if (combat) combat->use_mech_weapon(false);
        const float y = glm::radians(player.yaw);
        player.spawn(m->position + glm::vec3(-std::sin(y), std::cos(y), 0) * 4.0f, player.yaw);
        player_mech_ = -1;
        return;
    }
    if (player_vehicle_ < 0 && player_gun_ < 0) return;
    // Out at the feet of the seat.
    glm::vec3 eye;
    float a, b;
    if (seat(eye, a, b)) player.spawn(eye - glm::vec3(0, 0, player.radius.z + player.eye_height), player.yaw);
    if (combat) combat->use_vehicle_weapon(false), combat->use_mounted_gun(false);
    if (Object* g = player_gun_ >= 0 ? object(player_gun_) : nullptr) {
        g->yaw = gun_yaw_, scene.set_object_pose(g->index, g->position, gun_yaw_);
    }
    player_vehicle_ = player_gun_ = -1;
}

bool LevelLogic::seat(glm::vec3& eye, float& yaw_min, float& yaw_max) {
    if (player_vehicle_ >= 0) {
        Vehicle* v = vehicle(player_vehicle_);
        if (!v) return false;
        // The player is the shooter behind the turret (the model's "camera" is a chase
        // camera): the view swings around the gun's pivot, a little above and behind it.
        const glm::vec3 pivot(scene.object_matrix(v->index, "dummy_afet_support")[3]);
        const float y = glm::radians(player.yaw);
        eye = pivot - glm::vec3(-std::sin(y), std::cos(y), 0) * 1.25f + glm::vec3(0, 0, 0.55f);
        const float t = v->limits_len > 0 ? std::min(v->limits_t / v->limits_len, 1.0f) : 1.0f;
        yaw_min = v->yaw + v->from_min + (v->yaw_min - v->from_min) * t;
        yaw_max = v->yaw + v->from_max + (v->yaw_max - v->from_max) * t;
        return true;
    }
    if (player_gun_ >= 0) {
        const Object* g = object(player_gun_);
        if (!g) return false;
        // The gun turns with the view (CMachineGunAIComponent's "dummy" is its pivot); the
        // gunner stands behind it, 70 degrees each side of where it was set up.
        const glm::vec3 pivot(scene.object_matrix(g->index, "dummy")[3]);
        const float y = glm::radians(player.yaw);
        eye = (pivot == glm::vec3(0) ? g->position + glm::vec3(0, 0, 1.2f) : pivot) - glm::vec3(-std::sin(y), std::cos(y), 0) * 1.0f +
              glm::vec3(0, 0, 0.6f);
        yaw_min = gun_yaw_ - 70, yaw_max = gun_yaw_ + 70;
        return true;
    }
    return false;
}

void LevelLogic::update_vehicles(float dt) {
    for (auto& [id, v] : vehicles_) {
        if (v.limits_len > 0) v.limits_t += dt;
        if (!v.moving) {
            v.steer -= std::clamp(v.steer, -v.steer_speed * 3.0f * dt, v.steer_speed * 3.0f * dt);
            continue;
        }
        // Following: the leader is the route (the player when they are in it), stopping
        // short of it by the distance.
        if (v.follow != -1) {
            const bool player_leads = v.follow == kPlayerId || v.follow == player_mech_ || v.follow == player_vehicle_;
            const Object* lead = player_leads ? nullptr : object(v.follow);
            v.route.assign(1, player_leads ? player.center : lead ? lead->position : v.position);
            v.next = 0;
        }
        // Brake for the end of the route, else up to the maximum speed.
        float left = 0;
        glm::vec3 at = v.position;
        for (size_t k = v.next; k < v.route.size(); ++k) left += glm::distance(glm::vec2(at), glm::vec2(v.route[k])), at = v.route[k];
        if (v.follow != -1) left -= v.follow_distance;
        const float accel = 6.0f, brake = 8.0f;
        if (v.speed * v.speed * 0.5f / brake >= left) v.speed = std::max(v.speed - brake * dt, 0.0f);
        else v.speed = std::min(v.speed + accel * dt, v.max_speed);
        // Steer towards the next waypoint: no faster than the steering speed, nor than the
        // wheels' angle allows at this speed (a 2.8 m wheelbase).
        const glm::vec3 target = v.route[v.next];
        const glm::vec2 to = glm::vec2(target - v.position);
        const float want = glm::degrees(std::atan2(-to.x, to.y));
        const float rate = std::min(v.steer_speed * 3.0f, glm::degrees(v.speed * std::tan(glm::radians(v.steer_max)) / 2.8f));
        const float turn = std::clamp(angle_diff(v.yaw, want), -rate * dt, rate * dt);
        v.yaw += turn;
        // The front wheels at the angle that turn takes at this speed.
        const float want_steer = v.speed > 0.1f ? glm::degrees(std::atan(glm::radians(turn / std::max(dt, 1e-4f)) * 2.8f / v.speed)) : 0.0f;
        const float steer_step = v.steer_speed * 3.0f * dt;
        v.steer += std::clamp(std::clamp(want_steer, -v.steer_max, v.steer_max) - v.steer, -steer_step, steer_step);
        const float y = glm::radians(v.yaw);
        const glm::vec3 old = v.position;
        v.position += glm::vec3(-std::sin(y), std::cos(y), 0) * v.speed * dt;
        // Waypoint reached (or passed alongside): on to the next.
        const float d = glm::length(glm::vec2(target - v.position));
        if (v.follow != -1) {
        } else if (d < 4.0f || (d < 12.0f && glm::dot(glm::vec2(target - v.position), glm::vec2(target - old)) < 0)) {
            if (++v.next >= v.route.size()) v.moving = false, v.speed = 0, v.next = v.route.size() - 1;
        }
        // On the ground.
        float floor_z;
        if (scene.collision().ray_down(v.position + glm::vec3(0, 0, 3.0f), 12.0f, floor_z))
            v.position.z += (floor_z - v.position.z) * std::min(1.0f, dt * 8.0f);
        scene.set_object_pose(v.index, v.position, v.yaw);
        if (Object* o = object(id)) o->position = v.position, o->yaw = v.yaw;
        v.wheel_angle += v.speed * dt / 0.42f;
    }
    for (auto& [id, v] : vehicles_) place_vehicle_parts(v);
    if (Object* g = player_gun_ >= 0 ? object(player_gun_) : nullptr) scene.set_object_pose(g->index, g->position, player.yaw);
}

}  // namespace nova
