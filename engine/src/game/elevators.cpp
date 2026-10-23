// LevelLogic's lifts (CElevatorAIComponent). See level_logic.h.
#include <algorithm>

#include "audio/audio.h"
#include "game/anim_sounds.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace nova {

LevelLogic::Elevator* LevelLogic::elevator(int id) {
    for (Elevator& e : elevators_)
        if (e.id == id) return &e;
    return nullptr;
}

void LevelLogic::elevator_state(Elevator& e, const std::string& state) {
    if (trace) log("lift " + std::to_string(e.id) + " " + state);
    e.state = state, e.state_time = 0, e.state_length = 0;
    if (const AnimSounds::State* st = anim_sounds_->state(e.type, state); st && !st->loop && !st->clip.empty())
        e.state_length = scene.clip_length(e.index, st->clip);
    if (audio)
        for (int h : anim_sounds_->play_state(*audio, e.type, state, e.position)) e.sounds.push_back(h);
}

void LevelLogic::elevator_move(Elevator& e, bool to_end) {
    // StartFreeMove: from where it is to the waypoint, at its speed.
    e.to_end = to_end;
    e.from = e.position, e.dest = to_end ? e.end : e.start;
    e.along = 0, e.moving = glm::distance(e.from, e.dest) > 1e-3f;
}

void LevelLogic::lock_elevator(int id, bool lock) {
    Elevator* e = elevator(id);
    if (!e) return;
    if (!lock) {   // Unlock
        e->lock_pending = false;
        elevator_state(*e, "Idle");
    } else if (e->state == "Idle") {   // Lock: at once when at rest, else when it is next
        elevator_state(*e, "Lock");
    } else {
        e->lock_pending = true;
    }
}

bool LevelLogic::elevator_event(int id, const std::string& event) {
    Elevator* e = elevator(id);
    if (!e || (event != "MoveToStart" && event != "MoveToEnd")) return false;
    // OnEvent: called to an end (unless it is there already).
    const bool to_end = event == "MoveToEnd";
    if (glm::distance(e->position, to_end ? e->end : e->start) <= 0.1f) return true;
    elevator_state(*e, "Called");
    elevator_move(*e, to_end);
    return true;
}

void LevelLogic::update_elevators(float dt) {
    const glm::vec3 feet = player.center - glm::vec3(0, 0, player.radius.z);
    for (Elevator& e : elevators_) {
        const Object* o = object(e.id);
        if (!o || !o->enabled) continue;
        e.state_time += dt;
        // The player on it: within its box, any height above (CheckElevator: top + 200).
        glm::vec3 lo, hi;
        if (!scene.object_bounds(e.index, lo, hi)) continue;
        const bool on = feet.x > lo.x && feet.x < hi.x && feet.y > lo.y && feet.y < hi.y && feet.z > lo.z && feet.z < hi.z + 200;
        const bool clear = feet.x + player.radius.x < lo.x || feet.x - player.radius.x > hi.x || feet.y + player.radius.y < lo.y ||
                           feet.y - player.radius.y > hi.y || feet.z > hi.z + 200 || feet.z + 2 * player.radius.z < lo.z;
        if (e.moving && (e.state != "Called" || clear)) {
            const float length = glm::distance(e.from, e.dest);
            e.along = std::min(e.along + e.speed * dt, length);
            const glm::vec3 pos = e.from + (e.dest - e.from) * (e.along / length);
            float floor_z;
            uint32_t owner = 0;
            const bool riding = scene.collision().ray_down(feet + glm::vec3(0, 0, 0.35f), 0.8f, floor_z, &owner) && owner == e.index;
            const glm::vec3 delta = pos - e.position;
            e.position = pos;
            scene.move_object(e.index, pos);
            if (Object* mo = object(e.id)) mo->position = pos;
            if (riding) player.center += delta;
            if (audio)
                for (int h : e.sounds) audio->set_position(h, pos);
            if (e.along >= length) {   // there: open
                e.moving = false;
                elevator_state(e, "Opening");
            }
            continue;
        }
        const std::string s = e.state;
        if (s == "Idle") {
            if (on) elevator_state(e, "Closing");
        } else if (s == "Closing") {
            if (e.state_time >= e.state_length) {   // closed: off it goes
                elevator_state(e, "InUse");
                elevator_move(e, e.to_end);
            }
        } else if (s == "Opening") {
            if (e.state_time >= e.state_length) elevator_state(e, "DestEnd");
        } else if (s == "Called") {
            if (on) elevator_state(e, "Closing");   // UseElevator
            else if (!e.moving) elevator_move(e, e.to_end);
        } else if (s == "DestEnd") {
            if (!on) {   // stepped off: at rest here, the way back next
                elevator_state(e, e.lock_pending ? "Lock" : "Idle");
                e.lock_pending = false;
                e.to_end = !e.to_end;
            }
        }
    }
}

}  // namespace nova
