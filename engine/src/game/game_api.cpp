// The game's script functions (the C functions libNOVA3_neon.so registers with Lua).
// Every name the game registers exists; those not implemented yet log their first use
// and return nothing. Behaviour follows the decompiled originals where noted.
#include <cstdlib>
#include <random>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "audio/audio.h"
#include "game/combat.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace nova {

namespace {

const char* const kGameFunctions[] = {
#include "script/game_api_names.inc"
};

LevelLogic& ctx(lua_State* L) { return *static_cast<LevelLogic*>(lua_touserdata(L, lua_upvalueindex(1))); }
LevelLogic::Object* obj(lua_State* L, int arg) { return ctx(L).object(int(lua_tointeger(L, arg))); }
float seconds_arg(lua_State* L, int arg) { return lua_isnumber(L, arg) ? float(lua_tonumber(L, arg)) / 1000.0f : 0.0f; }
std::string str_arg(lua_State* L, int arg) { return lua_isstring(L, arg) ? lua_tostring(L, arg) : ""; }

int not_implemented(lua_State* L) {
    LevelLogic& c = ctx(L);
    const std::string name = lua_tostring(L, lua_upvalueindex(2));
    if (c.warned_.insert(name).second) c.log("script API not implemented yet: " + name);
    return 0;
}

// --- flow ---------------------------------------------------------------------------

// Wait(ms): yield (2, ms) — the manager resumes the thread when the time is up.
int api_wait(lua_State* L) {
    const lua_Integer ms = lua_tointeger(L, 1);
    lua_pushinteger(L, 2);
    lua_pushinteger(L, ms);
    return lua_yield(L, 2);
}

// WaitForEvent(name): yield (4, name, id). "AllEnemiesDead" returns at once; "SpawnEnd_<id>"
// waits only while that spawn manager is still running (one that never runs would hang).
int api_wait_for_event(lua_State* L) {
    const std::string name = str_arg(L, 1);
    if (name == "AllEnemiesDead") return 0;
    if (name.rfind("SpawnEnd_", 0) == 0) {
        LevelLogic& c = ctx(L);
        if (!c.combat || !c.combat->spawner_pending(std::atoi(name.c_str() + 9))) return 0;
    }
    lua_pushinteger(L, 4);
    lua_pushstring(L, name.c_str());
    lua_pushinteger(L, -1);
    return lua_yield(L, 3);
}

int api_return_now(lua_State*) { return 0; }   // waits on things that have already happened

// WaitForEnemiesDead(id, ...): wait until each listed object that is still alive has died
// (yield 4, "WaitForEnemiesDead", ids...; Combat sends the event per death).
int api_wait_for_enemies_dead(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int n = lua_gettop(L);
    std::vector<int> alive;
    for (int k = 1; k <= n; ++k) {
        const int id = int(lua_tointeger(L, k));
        const LevelLogic::Object* o = c.object(id);
        if (o && !o->dead) alive.push_back(id);
    }
    if (alive.empty()) return 0;
    lua_pushinteger(L, 4);
    lua_pushstring(L, "WaitForEnemiesDead");
    for (int id : alive) lua_pushinteger(L, id);
    return lua_yield(L, int(alive.size()) + 2);
}

// SendEvent(id, name, ...): wake scripts waiting for it, and run Obj<id>_<name> if defined.
int api_send_event(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int(lua_tointeger(L, 1));
    const std::string name = str_arg(L, 2);
    if (c.trace) c.log("SendEvent " + std::to_string(id) + " " + name);
    c.runtime_.send_event(name, id);
    c.start_script("Obj" + std::to_string(id) + "_" + name, id);
    c.elevator_event(id, name);   // a lift called to an end
    // Orders to NPCs (CAIComponent::OnEvent): a waypoint/region/gun id or a flag after the name.
    if (c.combat && lua_gettop(L) >= 2) {
        const int arg = lua_isnumber(L, 3) ? int(lua_tointeger(L, 3)) : 0;
        // A word ("start" / "stop") for some (CLinkedObjectsComponent's "flashlight").
        const bool flag = lua_gettop(L) < 3 || (lua_type(L, 3) == LUA_TSTRING ? str_arg(L, 3) != "stop" : lua_toboolean(L, 3) != 0);
        c.combat->npc_order(id, name, arg, flag, lua_isnumber(L, 4) ? float(lua_tonumber(L, 4)) : 0.0f,
                            lua_isnumber(L, 5) ? float(lua_tonumber(L, 5)) : 0.0f);
    }
    return 0;
}

// --- objects ------------------------------------------------------------------------

int api_enable(lua_State* L) {
    if (auto* o = obj(L, 1)) ctx(L).enable(*o, true);
    return 0;
}
int api_disable(lua_State* L) {
    if (auto* o = obj(L, 1)) ctx(L).enable(*o, false);
    return 0;
}
int api_show(lua_State* L) {
    if (auto* o = obj(L, 1)) ctx(L).show(*o, true);
    return 0;
}
int api_hide(lua_State* L) {
    if (auto* o = obj(L, 1)) ctx(L).show(*o, false);
    return 0;
}
int api_is_dead(lua_State* L) {
    auto* o = obj(L, 1);
    lua_pushboolean(L, o && o->dead);
    return 1;
}
int api_get_health(lua_State* L) {
    auto* o = obj(L, 1);
    lua_pushnumber(L, o ? o->health : 0);
    return 1;
}
int api_set_health(lua_State* L) {
    if (auto* o = obj(L, 1)) o->health = float(lua_tonumber(L, 2));
    return 0;
}
int api_set_state(lua_State* L) {
    if (auto* o = obj(L, 1)) ctx(L).set_state(*o, str_arg(L, 2));
    return 0;
}
int api_get_state(lua_State* L) {
    auto* o = obj(L, 1);
    lua_pushstring(L, o ? o->state.c_str() : "");
    return 1;
}
int api_get_position(lua_State* L) {
    auto* o = obj(L, 1);
    const glm::vec3 p = o ? o->position : glm::vec3(0);
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    lua_pushnumber(L, p.z);
    return 3;
}
// Object_SetPosition(id, x, y, z): the object there (CGameObject::SetPosition), its
// collision and effects with it; an actor through its AI.
int api_set_position(lua_State* L) {
    LevelLogic& c = ctx(L);
    auto* o = obj(L, 1);
    if (!o) return 0;
    o->position = {lua_tonumber(L, 2), lua_tonumber(L, 3), lua_tonumber(L, 4)};
    if (c.combat && c.combat->is_actor(o->id)) {
        c.combat->place_actor(o->id, o->position);
    } else if (o->index < c.scene.level().objects.size()) {
        if (!c.scene.move_object(o->index, o->position)) c.scene.set_object_pose(o->index, o->position, o->yaw);
        c.move_effects(*o);
    }
    return 0;
}
// GetPlayerPositionFromCameraPosition() -> x, y, z: where the player stands to see from
// the current (cinematic) camera: its position less the eye height.
int api_player_pos_from_camera(lua_State* L) {
    LevelLogic& c = ctx(L);
    Scene::CameraView cv;
    glm::vec3 eye = c.player.eye();
    if (c.cinematic_view(cv)) eye = cv.position, c.handover_view(cv.forward);
    const glm::vec3 feet = eye - glm::vec3(0, 0, c.player.radius.z + c.player.eye_height);
    if (c.trace) c.log("player position from the camera: " + std::to_string(feet.x) + " " + std::to_string(feet.y) + " " + std::to_string(feet.z));
    lua_pushnumber(L, feet.x), lua_pushnumber(L, feet.y), lua_pushnumber(L, feet.z);
    return 3;
}
int api_enable_interact(lua_State* L) {
    if (auto* o = obj(L, 1)) o->interact_enabled = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
    return 0;
}

// --- doors (CDoorAIComponent flags: +0x44 locked, +0x42 forced open) ------------------

LevelLogic::Door* door(lua_State* L, LevelLogic::Object*& o) {
    o = obj(L, 1);
    return o && o->door >= 0 ? &ctx(L).doors_[size_t(o->door)] : nullptr;
}
int api_lock_door(lua_State* L) {
    LevelLogic::Object* o;
    if (auto* d = door(L, o)) d->locked = true;
    return 0;
}
int api_unlock_door(lua_State* L) {
    LevelLogic::Object* o;
    if (auto* d = door(L, o)) d->locked = false;
    return 0;
}
int api_keep_door_unlocked(lua_State* L) {
    LevelLogic::Object* o;
    if (auto* d = door(L, o)) d->keep_unlocked = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
    return 0;
}
int api_open_door(lua_State* L) {
    LevelLogic::Object* o;
    if (auto* d = door(L, o)) {
        d->forced_open = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
        if (d->forced_open) ctx(L).set_door(*o, true);
    }
    return 0;
}
int api_close_door(lua_State* L) {
    LevelLogic::Object* o;
    if (auto* d = door(L, o)) {
        d->forced_open = false;
        ctx(L).set_door(*o, false);
    }
    return 0;
}
template <bool Lock>
int api_all_doors(lua_State* L) {
    for (auto& d : ctx(L).doors_) d.locked = Lock;
    return 0;
}
int api_close_all_doors(lua_State* L) {
    LevelLogic& c = ctx(L);
    for (auto& [id, o] : c.objects_)
        if (o.door >= 0) {
            c.doors_[size_t(o.door)].forced_open = false;
            c.set_door(o, false);
        }
    return 0;
}

// --- level --------------------------------------------------------------------------

int api_set_skybox(lua_State* L) {
    ctx(L).scene.set_skybox(int(lua_tointeger(L, 1)));
    return 0;
}
int api_fog_color(lua_State* L) {   // Fog_SetColor(r, g, b, ms), bytes
    const glm::vec4 c(lua_tonumber(L, 1) / 255.0, lua_tonumber(L, 2) / 255.0, lua_tonumber(L, 3) / 255.0, 1.0);
    ctx(L).fade_fog(0, c, seconds_arg(L, 4));
    return 0;
}
template <int What>
int api_fog_value(lua_State* L) {   // Fog_SetNear/Far/Density(value, ms)
    ctx(L).fade_fog(What, glm::vec4(float(lua_tonumber(L, 1))), seconds_arg(L, 2));
    return 0;
}
int api_teleport_player(lua_State* L) {   // TeleportPlayerToZone(spawn point id)
    if (auto* o = obj(L, 1)) ctx(L).teleport_player(*o);
    return 0;
}
template <bool On>
int api_portal(lua_State* L) {   // EnablePortal / DisablePortal(id): CZonePortal +8
    ctx(L).scene.zones().set_portal_enabled(int(lua_tointeger(L, 1)), On);
    return 0;
}
int api_fog_lock(lua_State* L) {   // Fog_SetLock(bool): zones stop changing the fog
    ctx(L).fog_locked = lua_toboolean(L, 1);
    return 0;
}
int api_load_level_sections(lua_State* L) {   // LoadLevelSections(n, effect): reload with section n
    const lua_Integer n = lua_tointeger(L, 1);
    if (n >= 1 && n <= 32 && !(ctx(L).scene.section() & (1u << (n - 1)))) ctx(L).pending_section = 1u << (n - 1);
    return 0;
}
// EndLevel([success]): the level is over; the next one loads (a failure restarts it).
int api_end_level(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.level_ended = true;
    c.level_success = lua_gettop(L) < 1 || lua_toboolean(L, 1) != 0;
    return 0;
}
// StartEndLevelEffect(ms, r, g, b, back): the picture drained to grey, tinted (r, g, b),
// over ms (the Gray post effect); back: the colour returning instead.
int api_start_end_level_effect(lua_State* L) {
    ctx(L).gray_effect(float(lua_tonumber(L, 1)), glm::vec3(float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4))),
                       lua_toboolean(L, 5) != 0);
    return 0;
}
int api_false(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
// StartFadeIn/StartFadeOut(ms[, colour]), CameraShake(ms, amount), TeleportToWaypoint(obj, wp).
template <bool In>
int api_fade(lua_State* L) {
    const uint32_t c = lua_gettop(L) >= 2 ? uint32_t(lua_tointeger(L, 2)) : 0;   // SColor ARGB; 0: black
    ctx(L).start_fade(In, float(lua_tointeger(L, 1)), glm::vec3(float((c >> 16) & 255), float((c >> 8) & 255), float(c & 255)) / 255.0f);
    return 0;
}

int api_camera_shake(lua_State* L) {
    ctx(L).shake(float(lua_tointeger(L, 1)), float(lua_tonumber(L, 2)));
    return 0;
}

int api_teleport_to_waypoint(lua_State* L) {
    LevelLogic& c = ctx(L);
    LevelLogic::Object* o = c.object(int(lua_tointeger(L, 1)));
    const LevelLogic::Object* wp = c.object(int(lua_tointeger(L, 2)));
    if (o && wp) c.teleport_object(*o, wp->position);
    return 0;
}

// GameObject_MotionOnPathStart(id) / Stop(id) / StopAtWaypoint(id, wp) /
// ChangeLinearVelocityMax(id, speed): objects on paths (elevators).
int api_motion_start(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.motion_start(int(lua_tointeger(L, 1)))) c.log("GameObject_MotionOnPathStart: no path for " + std::to_string(lua_tointeger(L, 1)));
    return 0;
}
int api_motion_stop(lua_State* L) {
    ctx(L).motion_stop(int(lua_tointeger(L, 1)));
    return 0;
}
int api_motion_stop_at(lua_State* L) {
    ctx(L).motion_stop(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)));
    return 0;
}
int api_motion_speed(lua_State* L) {
    ctx(L).motion_max_speed(int(lua_tointeger(L, 1)), float(lua_tonumber(L, 2)));
    return 0;
}

// Cinematics: StartCinematicObject(id[, skip]), EndCinematicObject(id),
// StartCinematic()/EndCinematic(), PlayCameraAnim(obj, clip[, loop]) (waits for the clip), PlayerLookTo(wp, ms).
// StartCinematicObject waits until the cinematic has ended ("CinematicEnd_<id>").
int api_start_cinematic_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int(lua_tointeger(L, 1));
    if (!c.start_cinematic(id)) {
        c.log("StartCinematicObject: no cinematic " + std::to_string(id));
        return 0;
    }
    lua_pushinteger(L, 4);
    lua_pushstring(L, ("CinematicEnd_" + std::to_string(id)).c_str());
    lua_pushinteger(L, id);
    return lua_yield(L, 3);
}
int api_end_cinematic_object(lua_State* L) {
    ctx(L).end_cinematic(int(lua_tointeger(L, 1)));
    return 0;
}
template <bool On>
int api_cinematic_mode(lua_State* L) {
    ctx(L).cinematic_mode = On;
    return 0;
}
int api_play_camera_anim(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (LevelLogic::Object* o = c.object(int(lua_tointeger(L, 1)))) {
        if (o->index == std::numeric_limits<size_t>::max()) return 0;
        if (!o->visible) c.show(*o, true);
        // The script waits out the clip, once through even when it loops
        // (CAnimationComponent::GetCurrentAnimationLength).
        Scene::ClipState cs;
        if (c.scene.play_clip(o->index, str_arg(L, 2), lua_toboolean(L, 3) != 0) && c.scene.clip_state(o->index, cs)) {
            lua_pushinteger(L, 2);
            lua_pushinteger(L, lua_Integer(cs.length));
            return lua_yield(L, 2);
        }
    }
    return 0;
}
// PlayerLookToDirection(dx, dy, dz, ms): turn the view toward the player's head plus the
// offset over ms (PlayerComponent::CinematicLookAt), the script waiting meanwhile.
int api_player_look_to_direction(lua_State* L) {
    LevelLogic& c = ctx(L);
    const glm::vec3 d(float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)));
    const int ms = int(lua_tointeger(L, 4));
    c.player_look_to(c.player.eye() + d, float(ms) / 1000.0f);
    lua_pushinteger(L, 2);
    lua_pushinteger(L, ms);
    return lua_yield(L, 2);
}
// GameObject_MotionOnPathSetFollowTarget(mover, leader, metres).
int api_motion_follow(lua_State* L) {
    ctx(L).motion_follow(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)), float(lua_tonumber(L, 3)));
    return 0;
}
int api_add_objective(lua_State* L) {
    LevelLogic& c = ctx(L);
    LevelLogic::ObjectiveMarker m;
    m.id = int(lua_tointeger(L, 1));   // ids also come as strings ("103153")
    m.text = str_arg(L, 2);
    m.node = str_arg(L, 4);
    if (c.trace) c.log("objective marker on " + std::to_string(m.id) + (c.object(m.id) ? "" : " (no such object)"));
    if (c.object(m.id)) c.objective_markers.push_back(m);
    return 0;
}
int api_remove_objective(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int(lua_tointeger(L, 1));
    auto& v = c.objective_markers;
    v.erase(std::remove_if(v.begin(), v.end(), [id](const LevelLogic::ObjectiveMarker& m) { return m.id == id; }), v.end());
    return 0;
}
// SetAttractionDirection(x, y, z): a pull on the player (CLevel's attraction), m/s.
int api_set_attraction(lua_State* L) {
    ctx(L).player.push = glm::vec3(float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)));
    return 0;
}
int api_vehicle_deactivate(lua_State* L) {   // Vehicle_Deactivate(): out of the vehicle
    ctx(L).leave_vehicle();
    return 0;
}
// NPCSuperSpeedToWaypoint(npc, waypoint): CNPCComponent::TrySSToPosition, the Volterites'
// blur-fast dash; taken here as a jump there at once.
int api_npc_super_speed(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.combat) c.combat->npc_order(int(lua_tointeger(L, 1)), "teleport", int(lua_tointeger(L, 2)), true);
    return 0;
}
// GameObject_Material_*(object, material, ...): CMaterialComponent's per-object material
// changes. Parameters apply to the object's materials that have them.
int api_material_technique(lua_State* L) {   // (object, material, technique)
    if (LevelLogic::Object* o = obj(L, 1)) {
        const bool ok = ctx(L).scene.set_object_technique(o->index, str_arg(L, 2), str_arg(L, 3));
        if (ctx(L).trace) ctx(L).log("technique " + str_arg(L, 2) + " -> " + str_arg(L, 3) + (ok ? "" : " (failed)"));
    }
    return 0;
}
int api_material_float(lua_State* L) {   // (object, material, param, value)
    if (LevelLogic::Object* o = obj(L, 1)) ctx(L).scene.set_object_param(o->index, str_arg(L, 3), glm::vec4(float(lua_tonumber(L, 4))), 1);
    return 0;
}
int api_material_vector3(lua_State* L) {   // (object, material, param, x, y, z)
    if (LevelLogic::Object* o = obj(L, 1))
        ctx(L).scene.set_object_param(o->index, str_arg(L, 3),
                                      glm::vec4(float(lua_tonumber(L, 4)), float(lua_tonumber(L, 5)), float(lua_tonumber(L, 6)), 0), 3);
    return 0;
}
int api_material_animated_float(lua_State* L) {   // (object, material, param, from, to, ms)
    if (LevelLogic::Object* o = obj(L, 1))
        ctx(L).ramp_param(o->index, str_arg(L, 3), float(lua_tonumber(L, 4)), float(lua_tonumber(L, 5)), float(lua_tonumber(L, 6)) / 1000.0f);
    return 0;
}
int api_heat_haze_start(lua_State* L) {   // PostEffect_HeatHazeStart(ms, object)
    ctx(L).heat_haze(true, float(lua_tointeger(L, 1)), lua_isnumber(L, 2) ? int(lua_tointeger(L, 2)) : -1);
    return 0;
}
int api_heat_haze_stop(lua_State* L) {   // PostEffect_HeatHazeStop(ms)
    ctx(L).heat_haze(false, float(lua_tointeger(L, 1)));
    return 0;
}
// CameraLookToWaypoint(wp, ms, nowait): the camera alone turns to a waypoint, the script
// waiting meanwhile unless `nowait`; CameraSwitchToPlayer() gives back the player's own
// view, ResetPlayerCamera() hands them the view as it is.
int api_camera_look_to_waypoint(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* wp = c.object(int(lua_tointeger(L, 1)));
    const int ms = int(lua_tointeger(L, 2));
    if (!wp) return 0;
    c.camera_look_to(wp->position, float(ms) / 1000.0f);
    if (lua_toboolean(L, 3)) return 0;
    lua_pushinteger(L, 2);
    lua_pushinteger(L, ms);
    return lua_yield(L, 2);
}
template <bool KeepView>
int api_camera_to_player(lua_State* L) {
    ctx(L).camera_to_player(KeepView);
    return 0;
}
// SetControlSensitivity(k): the view turns k times as fast (until the level ends).
int api_set_control_sensitivity(lua_State* L) {
    ctx(L).look_sensitivity = std::max(float(lua_tonumber(L, 1)), 0.0f);
    return 0;
}
// Npc_SetCustomIdleState(npc, state): the state the NPC idles in (nil: its usual one).
int api_npc_custom_idle(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.combat) c.combat->set_idle_state(int(lua_tointeger(L, 1)), str_arg(L, 2));
    return 0;
}
int api_vehicle_follow(lua_State* L) {   // Vehicle_FollowObject(vehicle, target, metres)
    ctx(L).vehicle_follow(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)), float(lua_tonumber(L, 3)));
    return 0;
}
// IsParalyzed(id): frozen by the Paralyze power (CHealthComponent::IsParalyzed); nothing
// for an object without health.
int api_is_paralyzed(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int p = c.combat ? c.combat->paralyzed(int(lua_tointeger(L, 1))) : -1;
    if (p < 0) return 0;
    lua_pushboolean(L, p);
    return 1;
}
// GetZone(id): the id of the zone the object is in (actors: where they stand), -1 if none.
int api_get_zone(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = obj(L, 1);
    int zone = ZoneSystem::kNone;
    if (o && o->index != std::numeric_limits<size_t>::max())
        zone = c.combat && c.combat->is_actor(o->id) ? c.scene.zones().zone_at(o->position + glm::vec3(0, 0, 0.5f))
                                                      : c.scene.zones().object_zone(o->index);
    const auto& zones = c.scene.zones().zones();
    lua_pushinteger(L, zone >= 0 && size_t(zone) < zones.size() ? zones[size_t(zone)].id : -1);
    return 1;
}
// GetObjType(id): the object's gol type id (CGameObject +0x88).
int api_get_obj_type(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = obj(L, 1);
    if (!o || o->index >= c.scene.level().objects.size()) return 0;
    lua_pushinteger(L, c.scene.level().objects[o->index].type);
    return 1;
}
int api_npc_set_bullet_type(lua_State* L) {   // Npc_SetBulletType(npc, weapon slot, projectile type)
    LevelLogic& c = ctx(L);
    if (c.combat) c.combat->set_bullet_type(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 3)));
    return 0;
}
int api_npc_link(lua_State* L) {   // Npc_Link(npc, object)
    ctx(L).link(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)));
    return 0;
}
int api_npc_unlink(lua_State* L) {   // Npc_UnLink(npc)
    ctx(L).unlink(int(lua_tointeger(L, 1)));
    return 0;
}
int api_start_timer(lua_State* L) {   // StartTimer(seconds)
    ctx(L).timer = float(lua_tointeger(L, 1)), ctx(L).timer_paused = false;
    return 0;
}
int api_pause_timer(lua_State* L) {   // PauseTimer(on)
    ctx(L).timer_paused = lua_toboolean(L, 1) != 0;
    return 0;
}
int api_npc_teleport_to_target(lua_State* L) {   // NpcCmd_TeleportToTarget(npc, cmd id)
    LevelLogic& c = ctx(L);
    if (c.combat) c.combat->teleport_to_player(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)));
    return 0;
}
// Limited control: SwitchToPlayerLimitedControl(), PlayerLimitedControlSetState(state),
// PlayerLimitedControlStartHang(object), PlayerLimitedControlUseGravity(on),
// PlayerLimitedControlSetLimitYaw/Pitch(degrees), SetLimitedControlStrafeSpeed(speed).
int api_limited_begin(lua_State* L) {
    ctx(L).limited_begin();
    return 0;
}
int api_limited_state(lua_State* L) {
    ctx(L).limited_state(str_arg(L, 1));
    return 0;
}
int api_limited_hang(lua_State* L) {
    ctx(L).limited_hang(int(lua_tointeger(L, 1)));
    return 0;
}
int api_limited_gravity(lua_State* L) {
    ctx(L).limited_gravity(lua_toboolean(L, 1) != 0);
    return 0;
}
template <int Which>
int api_limited_limit(lua_State* L) {
    const float v = float(lua_tonumber(L, 1));
    ctx(L).limited_limits(Which == 0 ? v : -1, Which == 1 ? v : -1);
    return 0;
}
int api_limited_strafe(lua_State* L) {
    ctx(L).limited_strafe_speed(float(lua_tonumber(L, 1)));
    return 0;
}
int api_player_set_position(lua_State* L) {   // PlayerSetPosition(x, y, z): the player's feet there
    LevelLogic& c = ctx(L);
    c.player.center = glm::vec3(float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)) + c.player.radius.z);
    return 0;
}
int api_flashback_start(lua_State* L) {   // PostEffect_FlashbackStart(ms)
    ctx(L).screen_effect(LevelLogic::ScreenEffect::Sepia, float(lua_tonumber(L, 1)));
    return 0;
}
int api_flashback_stop(lua_State* L) {   // PostEffect_FlashbackStop(ms)
    const float ms = float(lua_tonumber(L, 1));
    ctx(L).screen_effect(ms > 0 ? LevelLogic::ScreenEffect::Levels : LevelLogic::ScreenEffect::None, ms);
    return 0;
}
int api_start_flash(lua_State* L) {   // StartFlashEffect(ms)
    ctx(L).screen_effect(LevelLogic::ScreenEffect::Flash, float(lua_tonumber(L, 1)));
    return 0;
}
template <bool Lock>
int api_lock_elevator(lua_State* L) {   // LockElevator(lift) / UnlockElevator(lift) (ids also as strings)
    ctx(L).lock_elevator(int(lua_tointeger(L, 1)), Lock);
    return 0;
}
// The attack-choice scripts (Templ<type>_BA_ChooseAttack): Npc_GetPosture(npc) (standing:
// NPC.PostureStand; the port's NPCs do not take cover), Npc_GetAccuracy(npc),
// Npc_SetAttackBehavior(npc, behaviour).
int api_npc_posture(lua_State* L) {
    lua_pushinteger(L, 0);
    return 1;
}
int api_npc_accuracy(lua_State* L) {
    LevelLogic& c = ctx(L);
    lua_pushnumber(L, c.combat ? c.combat->accuracy(int(lua_tointeger(L, 1))) : 0.0);
    return 1;
}
int api_npc_attack_behaviour(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.combat) c.combat->set_attack_behaviour(int(lua_tointeger(L, 1)), str_arg(L, 2));
    return 0;
}
int api_lock_enemy_aggro(lua_State* L) {   // LockEnemyAggro(npc, aggressiveness)
    LevelLogic& c = ctx(L);
    if (c.combat) c.combat->lock_aggressiveness(int(lua_tointeger(L, 1)), float(lua_tonumber(L, 2)));
    return 0;
}
int api_player_look_to(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (const LevelLogic::Object* wp = c.object(int(lua_tointeger(L, 1))))
        c.player_look_to(wp->position, float(lua_tointeger(L, 2)) / 1000.0f);
    return 0;
}

// Tutorials: Start<Kind>Tutorial([ms]); TutorialMessage(id, ms) (0 hides it).
template <const char* Kind>
int api_tutorial_step(lua_State* L) {
    // StartGrenadeTutorial(ms, true) is the one for switching between grenades and mines.
    const std::string kind = std::string(Kind) == "Grenade" && lua_toboolean(L, 2) ? "GrenadeSwitch" : Kind;
    ctx(L).start_tutorial_step(kind, lua_gettop(L) >= 1 ? float(lua_tointeger(L, 1)) : 0.0f);
    return 0;
}
constexpr char kRotate[] = "Rotate", kMove[] = "Move", kJump[] = "Jump", kReload[] = "Reload", kShoot[] = "Shoot",
               kChangeWeapon[] = "ChangeWeapon", kRush[] = "Rush", kIronSight[] = "IronSight", kGrenade[] = "Grenade",
               kSpecialPower[] = "SpecialPower", kParalyze[] = "Paralyze", kDetonate[] = "Detonate", kShield[] = "Shield";
int api_tutorial_message(lua_State* L) {
    LevelLogic& c = ctx(L);
    const std::string id = str_arg(L, 1);
    const int ms = int(lua_tointeger(L, 2));
    if (ms > 0) c.message(id, float(ms) / 1000.0f);
    else c.hide_message(id);
    return 0;
}

// SaveGame(spawn point): a checkpoint. ShowProgressSavedMessage().
int api_save_game(lua_State* L) {
    ctx(L).save_checkpoint(int(lua_tointeger(L, 1)));
    return 0;
}
int api_progress_saved(lua_State* L) {
    ctx(L).message("FPS_GAME_SAVED", 2.5f);
    return 0;
}

// --- NPC commands (CAIComponent goals) -------------------------------------------------
// NpcCmd_*(npc, goal id, waypoint, ...): queue a goal; WaitNpcCmdFinished(npc, goal id or
// -1) waits while it is queued (Combat sends NpcCmdFinished_<npc>_<id> when it is done).

int push_npc_goal(lua_State* L, NpcGoal::Kind kind, int wp_arg) {
    LevelLogic& c = ctx(L);
    if (!c.combat) return 0;
    NpcGoal g;
    g.kind = kind;
    g.id = int(lua_tointeger(L, 2));
    if (wp_arg > 0) {
        const LevelLogic::Object* wp = c.object(int(lua_tointeger(L, wp_arg)));
        if (!wp) return 0;
        g.pos = wp->position;
    }
    if (kind == NpcGoal::Kind::Move && lua_gettop(L) >= 5) g.run = str_arg(L, 5) != "move" && str_arg(L, 5) != "walk";
    if (kind == NpcGoal::Kind::Action) g.state = str_arg(L, 3);
    c.combat->push_goal(int(lua_tointeger(L, 1)), g);
    return 0;
}

int api_npc_move(lua_State* L) { return push_npc_goal(L, NpcGoal::Kind::Move, 3); }
int api_npc_cover(lua_State* L) { return push_npc_goal(L, NpcGoal::Kind::Move, 3); }   // to the cover point
int api_npc_jump(lua_State* L) { return push_npc_goal(L, NpcGoal::Kind::Jump, 3); }
int api_npc_turn(lua_State* L) { return push_npc_goal(L, NpcGoal::Kind::Turn, 3); }
int api_npc_teleport(lua_State* L) { return push_npc_goal(L, NpcGoal::Kind::Teleport, 3); }
int api_npc_action(lua_State* L) { return push_npc_goal(L, NpcGoal::Kind::Action, 0); }

int api_npc_cancel(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.combat) c.combat->cancel_goals(int(lua_tointeger(L, 1)));
    return 0;
}

int api_wait_npc_cmd(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int npc = int(lua_tointeger(L, 1)), id = int(lua_tointeger(L, 2));
    if (!c.combat || !c.combat->goal_pending(npc, id)) return 0;
    lua_pushinteger(L, 4);
    lua_pushstring(L, ("NpcCmdFinished_" + std::to_string(npc) + "_" + std::to_string(id)).c_str());
    return lua_yield(L, 2);
}

// --- sound (VoxSoundManager) ----------------------------------------------------------
// The game passes a fade time (ms) to every Play; here it fades in loops and music only,
// so one-shots keep their attack. Defaults as the game's: 4000 ms, StartMusic 0.

Audio::Params sound_params(lua_State* L, const SoundDef* def, int loop_arg, int fade_arg, int fade_default) {
    Audio::Params p;
    p.loop = lua_toboolean(L, loop_arg) ? 1 : 0;
    const float fade = lua_gettop(L) >= fade_arg ? float(lua_tointeger(L, fade_arg)) : float(fade_default);
    if (p.loop || (def && def->group == 1)) p.fade_in_ms = fade;
    return p;
}

float fade_arg(lua_State* L, int arg, int fallback) {
    return lua_gettop(L) >= arg ? float(lua_tointeger(L, arg)) : float(fallback);
}

// StartSound(name, loop[, fade ms]).
int api_start_sound(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.audio) return 0;
    const std::string name = str_arg(L, 1);
    c.audio->play(name, sound_params(L, c.audio->table().sound(name), 2, 3, 4000));
    return 0;
}

// StopSound(name[, fade ms]) / StopMusic(name[, fade ms]).
int api_stop_sound(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.audio) c.audio->stop(str_arg(L, 1), fade_arg(L, 2, 4000));
    return 0;
}

int api_stop_music(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.audio) c.audio->stop(str_arg(L, 1), fade_arg(L, 2, 0));
    return 0;
}

// StartSoundAtWaypoint(name, waypoint, loop[, fade ms]): a 3D sound at the waypoint.
int api_start_sound_at_waypoint(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.audio) return 0;
    const std::string name = str_arg(L, 1);
    Audio::Params p = sound_params(L, c.audio->table().sound(name), 3, 4, 0);
    const LevelLogic::Object* wp = c.object(int(lua_tointeger(L, 2)));
    if (wp) p.position = &wp->position;
    c.audio->play(name, p);
    return 0;
}

// StartSoundFromEvent(event[, fade ms]) / StopSoundFromEvent(event[, fade ms]).
int api_start_sound_from_event(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.audio) return 0;
    Audio::Params p;
    p.fade_in_ms = fade_arg(L, 2, 4000);
    c.audio->play_event(str_arg(L, 1), p);
    return 0;
}

int api_stop_sound_from_event(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.audio) c.audio->stop(str_arg(L, 1), fade_arg(L, 2, 4000));
    return 0;
}

// StartMusic(name, loop[, fade ms]): also the level's current music.
int api_start_music(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.audio) return 0;
    std::string name = str_arg(L, 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();   // "overloading_reactor "
    Audio::Params p = sound_params(L, c.audio->table().sound(name), 2, 3, 0);
    c.audio->play(name, p);
    return 0;
}

// SetInteractiveMusicState(sound, state): an interactive (.vxn) sound to a state, e.g.
// Volteron 3's reactor alarm to its "outro".
int api_set_interactive_music_state(lua_State* L) {
    LevelLogic& c = ctx(L);
    std::string name = str_arg(L, 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (c.audio) c.audio->set_state(name, str_arg(L, 2));
    return 0;
}

// EnableMotionBlur(on): fast turns blur the picture (CLevel's flag; off stops it at once).
int api_enable_motion_blur(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.motion_blur = lua_toboolean(L, 1) != 0;
    if (!c.motion_blur) c.post.radial_blur = 0;
    return 0;
}

// Particle effects: StartEffect(id[, index]) / StopEffect(id[, index]); id 0 is the player.
int api_start_effect(lua_State* L) {
    LevelLogic& g = ctx(L);
    const int id = int(lua_tointeger(L, 1)), index = int(lua_tointeger(L, 2));
    if (LevelLogic::Object* o = id ? g.object(id) : &g.player_object()) g.start_effect(*o, index);
    else if (g.warned_.insert("StartEffect " + std::to_string(id)).second) g.log("StartEffect: no effects on object " + std::to_string(id));
    return 0;
}

int api_stop_effect(lua_State* L) {
    LevelLogic& g = ctx(L);
    const int id = int(lua_tointeger(L, 1));
    if (LevelLogic::Object* o = id ? g.object(id) : &g.player_object()) g.stop_effect(*o, int(lua_tointeger(L, 2)));
    return 0;
}

// Post effects (PostEffect_Enable / SetVar / SetParam): DOF and HEIGHTFOG.
int api_post_enable(lua_State* L) {
    const std::string name = str_arg(L, 1);
    const bool on = lua_toboolean(L, 2) != 0;
    if (name == "DOF") ctx(L).post.dof = on;
    else if (name == "HEIGHTFOG") ctx(L).post.height_fog = on;
    return 0;
}
int api_post_set_var(lua_State* L) {   // (effect, var, value [, seconds]) or (effect, var, from, to, seconds)
    const int n = lua_gettop(L);
    const std::string effect = str_arg(L, 1), var = str_arg(L, 2);
    if (n >= 5) {
        const float from = float(lua_tonumber(L, 3));
        ctx(L).set_post_var(effect, var, float(lua_tonumber(L, 4)), float(lua_tonumber(L, 5)), &from);
    } else {
        ctx(L).set_post_var(effect, var, float(lua_tonumber(L, 3)), n >= 4 ? float(lua_tonumber(L, 4)) : 0.0f);
    }
    return 0;
}
int api_post_set_param(lua_State* L) {   // ("HEIGHTFOG", "FogColor", r, g, b): bytes
    if (str_arg(L, 1) == "HEIGHTFOG" && str_arg(L, 2) == "FogColor")
        ctx(L).post.fog_color = glm::vec3(float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4)), float(lua_tonumber(L, 5))) / 255.0f;
    return 0;
}

// Combat (game/combat.cpp): enemies, damage, the player's health.
int api_get_no_enemies(lua_State* L) {   // CAIController::GetNoOpponentsByTeams: living hostile actors
    Combat* c = ctx(L).combat;
    lua_pushinteger(L, c ? c->enemies_alive() : 0);
    return 1;
}
int api_get_enemies(lua_State* L) {
    lua_newtable(L);
    if (Combat* c = ctx(L).combat) {
        int i = 1;
        for (int id : c->enemy_ids()) {
            lua_pushinteger(L, id);
            lua_rawseti(L, -2, i++);
        }
    }
    return 1;
}
int api_set_invulnerable(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->set_invulnerable(int(lua_tointeger(L, 1)), lua_toboolean(L, 2) != 0);
    return 0;
}
int api_give_damage(lua_State* L) {   // GiveDamage(id, amount)
    if (Combat* c = ctx(L).combat) c->damage(int(lua_tointeger(L, 1)), float(lua_tonumber(L, 2)));
    return 0;
}
int api_kill_all_enemies(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->kill_all_enemies();
    return 0;
}
int api_enable_shoot_control(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->shoot_enabled = lua_toboolean(L, 1) != 0;
    return 0;
}
// Enable<Kind>Control(bool): CBaseControlScheme::Enable*Control and the interface's own
// buttons (weapons, items, powers).
template <bool LevelLogic::Controls::*Flag>
int api_enable_control(lua_State* L) {
    ctx(L).controls.*Flag = lua_toboolean(L, 1) != 0;
    return 0;
}
int api_enable_hud(lua_State* L) {
    ctx(L).hud_enabled = lua_toboolean(L, 1) != 0;
    return 0;
}
int api_force_interface_in_cinematic(lua_State* L) {
    ctx(L).interface_in_cinematic = lua_toboolean(L, 1) != 0;
    return 0;
}
// Test_distort_start(): the weapon object's materials become hand_mesh_new_distort.bdae's
// armsDistort at BlendFactor -0.3. Test_change_blend(value, ms) eases there over ms;
// Test_distort_stop() puts its own materials back.
int api_test_distort_start(lua_State* L) {
    LevelLogic::ArmsDistort& d = ctx(L).arms_distort;
    d.on = true, d.target = 0, d.ms = 0, ++d.serial;
    return 0;
}

int api_test_distort_stop(lua_State* L) {
    LevelLogic::ArmsDistort& d = ctx(L).arms_distort;
    d.on = false, ++d.serial;
    return 0;
}

int api_test_change_blend(lua_State* L) {
    LevelLogic::ArmsDistort& d = ctx(L).arms_distort;
    d.target = float(lua_tonumber(L, 1)), d.ms = float(lua_tonumber(L, 2)), ++d.serial;
    return 0;
}

// SetCrtWeaponState(state): play a state of the current weapon (GetUp, GetDown, Idle,
// UnAim, GetWeapon, Weapon_Show_Off).
int api_set_crt_weapon_state(lua_State* L) {
    ctx(L).weapon_state = str_arg(L, 1);
    return 0;
}
// Weapons by name: UnlockWeapon, LockWeapon, UseWeapon(name); GiveAmmo(name, amount).
int api_unlock_weapon(lua_State* L) {
    if (Combat* c = ctx(L).combat; c && !c->unlock_weapon(str_arg(L, 1))) ctx(L).log("UnlockWeapon: no weapon " + str_arg(L, 1));
    return 0;
}
int api_lock_weapon(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->lock_weapon(str_arg(L, 1));
    return 0;
}
int api_use_weapon(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->use_weapon(str_arg(L, 1));
    return 0;
}
int api_give_ammo(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->give_ammo(str_arg(L, 1), int(lua_tointeger(L, 2)));
    return 0;
}
int api_get_health_mc(lua_State* L) {   // the player ("MC": main character)
    Combat* c = ctx(L).combat;
    lua_pushnumber(L, c ? c->player_health : 100);
    return 1;
}
int api_give_damage_mc(lua_State* L) {   // GiveDamageMC(amount, ...)
    if (Combat* c = ctx(L).combat) c->damage_player(float(lua_tonumber(L, 1)), ctx(L).player.center);
    return 0;
}
// GiveDamageToScriptTarget(trigger, amount, ...): what set the trigger off takes the damage
// (only the player sets them off here). IsTriggerTarget(trigger, who): 0 the player, -1 anyone.
int api_give_damage_to_script_target(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->damage_player(float(lua_tonumber(L, 2)), ctx(L).player.center);
    return 0;
}
int api_is_trigger_target(lua_State* L) {
    const lua_Integer who = lua_tointeger(L, 2);
    lua_pushboolean(L, who == 0 || who == -1);
    return 1;
}
int api_get_random(lua_State* L) {   // GetRandom(n) -> 0..n-1, GetRandom(a, b) -> a..b
    static std::mt19937 rng(12345);
    lua_Integer lo = 0, hi = lua_tointeger(L, 1) - 1;
    if (lua_gettop(L) >= 2) lo = lua_tointeger(L, 1), hi = lua_tointeger(L, 2);
    if (hi < lo) hi = lo;
    lua_pushinteger(L, std::uniform_int_distribution<lua_Integer>(lo, hi)(rng));
    return 1;
}

// --- messages (text ids into the string tables) --------------------------------------

// ShowMessage(text, face, [text, face, ...], ?, ms, flag): (n - 3) / 2 lines (empty ones
// skipped), each shown for ms or while its voice lasts; "MessageEnd" after the last.
int api_show_message(lua_State* L) {
    const int n = lua_gettop(L);
    std::vector<std::string> lines;
    for (int k = 0; k < (n - 3) / 2; ++k)
        if (lua_type(L, 1 + 2 * k) == LUA_TSTRING && lua_tostring(L, 1 + 2 * k)[0]) lines.push_back(lua_tostring(L, 1 + 2 * k));
    if (lines.empty() && n >= 1 && lua_type(L, 1) == LUA_TSTRING && lua_tostring(L, 1)[0]) lines.push_back(lua_tostring(L, 1));
    const float ms = n >= 2 && lua_isnumber(L, n - 1) ? float(lua_tonumber(L, n - 1)) : 0.0f;
    ctx(L).show_message(lines, ms / 1000.0f);
    return 0;
}
// ShowDialogMessage(line, line, ..., flag): voice-over lines played in order.
int api_show_dialog(lua_State* L) {
    std::vector<std::string> lines;
    for (int i = 1; i < lua_gettop(L); ++i)
        if (lua_type(L, i) == LUA_TSTRING && lua_tostring(L, i)[0]) lines.push_back(lua_tostring(L, i));
    ctx(L).dialog(lines);
    return 0;
}

// EnablePath(waypoint[, text id, flag]): CPlayerBaseComponent's guide waypoint.
int api_enable_path(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* wp = c.object(int(lua_tointeger(L, 1)));
    c.path_marker.on = wp != nullptr;
    if (wp) c.path_marker.position = wp->position;
    return 0;
}

// LockTarget(npc, target, on), IgnoreTarget(npc, target, on): target 0 is the player.
// Npc_SetPrimaryTarget(npc, target) is taken as a lock.
int api_lock_target(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->lock_target(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)), lua_toboolean(L, 3) != 0);
    return 0;
}
int api_ignore_target(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->ignore_target(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)), lua_toboolean(L, 3) != 0);
    return 0;
}
int api_set_primary_target(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->lock_target(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)), true);
    return 0;
}
int api_set_global_awareness_factor(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->awareness_factor = float(lua_tonumber(L, 1));
    return 0;
}

// SetDSPPreset(name[, ms]): the reverb's preset for the area (sounds on the AUX1 bus).
int api_set_dsp_preset(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.audio) c.audio->set_reverb_preset(str_arg(L, 1), lua_isnumber(L, 2) ? float(lua_tonumber(L, 2)) : 0.0f);
    return 0;
}
// Fog_AnimationStart(density %, near %, far %, min ms, max ms) (integers), Fog_AnimationStop().
int api_fog_animation_start(lua_State* L) {
    ctx(L).fog_animation(true, float(lua_tointeger(L, 1)), float(lua_tointeger(L, 2)), float(lua_tointeger(L, 3)),
                         float(lua_tointeger(L, 4)), float(lua_tointeger(L, 5)));
    return 0;
}
int api_fog_animation_stop(lua_State* L) {
    ctx(L).fog_animation(false);
    return 0;
}
// SetMinEnemiesOnPlayer(n) / RestoreMinEnemiesOnPlayer(): how many hostile actors at least
// fight the player (CAIController).
int api_set_min_enemies_on_player(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->min_enemies_on_player = int(lua_tointeger(L, 1));
    return 0;
}
int api_restore_min_enemies_on_player(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->min_enemies_on_player = 1;
    return 0;
}
int api_set_color_grading(lua_State* L) {   // SetColorGrading(lut, ms)
    ctx(L).set_color_grading(str_arg(L, 1), float(lua_tonumber(L, 2)));
    return 0;
}
int api_disable_color_grading(lua_State* L) {
    ctx(L).disable_color_grading();
    return 0;
}

int api_set_fov(lua_State* L) {   // SetFOV(fov, ms)
    ctx(L).set_fov(float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)));
    return 0;
}
int api_reset_fov(lua_State* L) {   // ResetFOV(ms): back to 70
    ctx(L).set_fov(70, float(lua_tonumber(L, 1)));
    return 0;
}
int api_set_zfar(lua_State* L) {
    ctx(L).zfar = std::max(float(lua_tonumber(L, 1)), 10.0f);
    return 0;
}
int api_set_move_sensitivity(lua_State* L) {
    ctx(L).move_sensitivity = std::clamp(float(lua_tonumber(L, 1)), 0.0f, 1.0f);
    return 0;
}
int api_player_enable_rush(lua_State* L) {
    ctx(L).player_rush = lua_toboolean(L, 1) != 0;
    return 0;
}

// QTEAddSlowTime(factor, ms), QTEStart(kind), QTEStop(); QTE_Fire() spends a round of
// the current weapon (the cinematic shows the shot).
int api_qte_add_slow_time(lua_State* L) {
    ctx(L).qte_add_slow_time(float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)));
    return 0;
}
int api_qte_start(lua_State* L) {
    ctx(L).qte_start(str_arg(L, 1));
    return 0;
}
int api_qte_stop(lua_State* L) {
    ctx(L).qte_stop();
    return 0;
}
int api_qte_fire(lua_State* L) {
    if (Combat* c = ctx(L).combat) c->spend_round();
    return 0;
}

// GiveSplashDamage(object, damage, radius): a blast at the object.
int api_give_splash_damage(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (const LevelLogic::Object* o = obj(L, 1); o && c.combat)
        c.combat->splash_damage(o->position + glm::vec3(0, 0, 0.5f), float(lua_tonumber(L, 2)), std::max(float(lua_tonumber(L, 3)), 0.1f), false);
    return 0;
}
// PlayAnimation(object, clip): play the clip once and wait for its length.
int api_play_animation(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = obj(L, 1);
    if (!o || !c.scene.play_clip(o->index, str_arg(L, 2), false)) return 0;
    Scene::ClipState cs;
    lua_pushinteger(L, 2);
    lua_pushinteger(L, c.scene.clip_state(o->index, cs) ? lua_Integer(cs.length) : 0);
    return lua_yield(L, 2);
}
int api_ignore(lua_State*) { return 0; }   // LoadTexture (preloading), SetDSPPreset (reverb)

// Vehicles: Vehicle_MoveOnPath(vehicle, waypoint), Vehicle_Stop(vehicle),
// Vehicle_SetMaxSpeed(vehicle, km/h), Vehicle_SetSteeringMax(vehicle, degrees),
// Vehicle_SetSteeringSpeed(vehicle, degrees/s), Vehicle_SetYawLimits(vehicle, min, max, ms),
// Vehicle_Activate() (the player gets into the vehicle whose script this is),
// SwitchToMachineGun([gun]), SwitchToPlayerOnFoot().
int api_vehicle_move_on_path(lua_State* L) {
    ctx(L).vehicle_move_on_path(int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)));
    return 0;
}
int api_vehicle_stop(lua_State* L) {
    ctx(L).vehicle_stop(int(lua_tointeger(L, 1)));
    return 0;
}
template <int Which>
int api_vehicle_set(lua_State* L) {
    const float v = float(lua_tonumber(L, 2));
    ctx(L).vehicle_set(int(lua_tointeger(L, 1)), Which == 0 ? v : -1, Which == 1 ? v : -1, Which == 2 ? v : -1);
    return 0;
}
int api_vehicle_yaw_limits(lua_State* L) {
    ctx(L).vehicle_yaw_limits(int(lua_tointeger(L, 1)), float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4)));
    return 0;
}
int api_vehicle_activate(lua_State* L) {
    lua_getglobal(L, "this");
    const int owner = int(lua_tointeger(L, -1));
    lua_pop(L, 1);
    LevelLogic& c = ctx(L);
    if (const LevelLogic::Object* o = c.object(owner); o && o->type.find("_Player") != std::string::npos) c.enter_vehicle(owner);
    return 0;
}
int api_switch_to_machine_gun(lua_State* L) {   // without a gun: the one whose script this is
    int gun = int(lua_tointeger(L, 1));
    if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
        lua_getglobal(L, "this");
        gun = int(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    ctx(L).man_gun(gun);
    return 0;
}
int api_switch_to_mech(lua_State* L) {   // SwitchToMech(): the mech whose script this is
    lua_getglobal(L, "this");
    const int owner = int(lua_tointeger(L, -1));
    lua_pop(L, 1);
    ctx(L).enter_mech(owner);
    return 0;
}
int api_switch_to_player_on_foot(lua_State* L) {
    ctx(L).leave_vehicle();
    return 0;
}

int api_set_max_active_enemies(lua_State* L) {   // SetMaxActiveEnemies(n[, allow 0]): attack tokens
    const int n = int(lua_tointeger(L, 1));
    if (Combat* c = ctx(L).combat) c->set_max_shooters(n < 1 && !lua_toboolean(L, 2) ? 1 : n);   // CAIController::SetMaxActiveEnemies
    return 0;
}
int api_enable_fall_damage(lua_State* L) {
    ctx(L).fall_damage = lua_toboolean(L, 1) != 0;
    return 0;
}
int api_hide_message(lua_State* L) {
    ctx(L).hide_message("");
    return 0;
}
int api_reset_trigger_run_count(lua_State* L) {
    ctx(L).reset_trigger_runs(int(lua_tointeger(L, 1)));
    return 0;
}
// IsPlayerLookingAt(object, degrees): the object within that angle of the view.
int api_is_player_looking_at(lua_State* L) {
    LevelLogic& c = ctx(L);
    bool looking = false;
    if (const LevelLogic::Object* o = obj(L, 1)) {
        const glm::vec3 to = o->position + glm::vec3(0, 0, 1) - c.player.eye();
        const float d = glm::length(to);
        looking = d > 1e-3f && glm::degrees(std::acos(std::clamp(glm::dot(to / d, c.player.view_forward()), -1.0f, 1.0f))) <=
                                   float(lua_tonumber(L, 2));
    }
    lua_pushboolean(L, looking);
    return 1;
}
// Enter/ExitInvisibility(npc): cloaked NPCs are not drawn.
template <bool Visible>
int api_invisibility(lua_State* L) {
    LevelLogic& c = ctx(L);
    // Cloaked (refracting the scene), or not drawn at all for an NPC without a cloak.
    const LevelLogic::Object* o = obj(L, 1);
    if (o && !(c.combat && c.combat->script_invisibility(o->id, !Visible))) c.scene.set_object_visible(o->index, Visible && o->visible);
    return 0;
}
int api_npc_crouch(lua_State* L) {   // NpcCmd_Crouch(npc, id, on): crouches where it is (holds there)
    if (Combat* c = ctx(L).combat) c->npc_order(int(lua_tointeger(L, 1)), "lock_in_cover", 0, lua_toboolean(L, 3) != 0);
    return 0;
}

// ChangeLightMap(object, texture, ms): the object's lightmap is swapped (at once).
int api_change_light_map(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (const LevelLogic::Object* o = obj(L, 1); o && !c.scene.set_object_lightmap(o->index, str_arg(L, 2)))
        c.log("ChangeLightMap: no lightmap " + str_arg(L, 2) + " for " + std::to_string(o->id));
    return 0;
}
// SetFogZColor(object, ms, r, g, b): the object's FogZColor material parameter (0-255).
int api_set_fog_z_color(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (const LevelLogic::Object* o = obj(L, 1))
        c.scene.set_object_param(o->index, "FogZColor",
                                 glm::vec4(float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4)), float(lua_tonumber(L, 5)), 255) / 255.0f, 3);
    return 0;
}

// HUD_StartFrost(ms), HUD_StopFrost(ms), HUD_SetFrostMaxFactor(f): the frost overlay.
template <bool On>
int api_hud_frost(lua_State* L) {
    LevelLogic::Frost& f = ctx(L).frost;
    f.on = On ? 1.0f : 0.0f, f.ms = float(lua_tonumber(L, 1)), ++f.serial;
    return 0;
}
int api_hud_frost_max(lua_State* L) {
    ctx(L).frost.max = std::clamp(float(lua_tonumber(L, 1)), 0.0f, 1.0f);
    return 0;
}

// Special powers by name: UnlockSpecialPower, LockSpecialPower, UseSpecialPower (the
// jetpack is the player's; Repel, Paralyze and Slow go through the power button).
template <int Op>   // 0 unlock, 1 lock, 2 use
int api_special_power(lua_State* L) {
    LevelLogic& c = ctx(L);
    std::string name = str_arg(L, 1);
    for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (name == "jetpack") {
        if (Op == 0) c.jetpack_unlocked = true;
        if (Op == 1) c.jetpack_unlocked = false, c.player.jetpack = false;
        if (Op == 2) c.jetpack_unlocked = true, c.player.jetpack = true;
        return 0;
    }
    if (Combat* cb = c.combat) {
        if (Op == 0) cb->unlock_power(name, true);
        if (Op == 1) cb->unlock_power(name, false);
        if (Op == 2) cb->use_power_now(name);
    }
    return 0;
}

int api_start_bullet_time(lua_State* L) {   // StartBulletTime(ms, factor)
    ctx(L).bullet_time(float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)));
    return 0;
}

// Objectives by string id (Application::GetStringIdFromName).
int api_set_primary_objective(lua_State* L) {
    ctx(L).set_primary_objective(str_arg(L, 1));
    return 0;
}
int api_add_secondary_objective(lua_State* L) {
    ctx(L).add_secondary_objective(str_arg(L, 1));
    return 0;
}
int api_secondary_objective_complete(lua_State* L) {
    ctx(L).secondary_objective_complete(str_arg(L, 1));
    return 0;
}
int api_primary_objective_complete(lua_State* L) {   // the game ignores any argument
    ctx(L).primary_objective_complete();
    return 0;
}

struct Entry {
    const char* name;
    ScriptRuntime::Function fn;
};
const Entry kImplemented[] = {
    {"Wait", api_wait},
    {"WaitForEvent", api_wait_for_event},
    {"WaitForEnemiesDead", api_wait_for_enemies_dead},
    {"WaitNpcCmdFinished", api_wait_npc_cmd},
    {"StartFadeIn", api_fade<true>},
    {"GameObject_MotionOnPathStart", api_motion_start},
    {"GameObject_MotionOnPathStop", api_motion_stop},
    {"GameObject_MotionOnPathStopAtWaypoint", api_motion_stop_at},
    {"GameObject_MotionOnPathChangeLinearVelocityMax", api_motion_speed},
    {"StartFadeOut", api_fade<false>},
    {"CameraShake", api_camera_shake},
    {"TeleportToWaypoint", api_teleport_to_waypoint},
    {"NpcCmd_MoveToWp", api_npc_move},
    {"NpcCmd_TakeCover", api_npc_cover},
    {"NpcCmd_JumpToWp", api_npc_jump},
    {"NpcCmd_TurnToWp", api_npc_turn},
    {"NpcCmd_TeleportToWp", api_npc_teleport},
    {"NpcCmd_DoAction", api_npc_action},
    {"NpcCmd_CancelAll", api_npc_cancel},
    {"WaitForServer", api_return_now},
    {"SendEvent", api_send_event},
    {"Enable", api_enable},
    {"Disable", api_disable},
    {"ShowObject", api_show},
    {"HideObject", api_hide},
    {"IsDead", api_is_dead},
    {"GetHealth", api_get_health},
    {"SetHealth", api_set_health},
    {"SetState", api_set_state},
    {"GetState", api_get_state},
    {"Object_GetPosition", api_get_position},
    {"Object_SetPosition", api_set_position},
    {"GetPlayerPositionFromCameraPosition", api_player_pos_from_camera},
    {"EnableInteract", api_enable_interact},
    {"LockDoor", api_lock_door},
    {"UnlockDoor", api_unlock_door},
    {"KeepDoorUnlocked", api_keep_door_unlocked},
    {"OpenDoor", api_open_door},
    {"CloseDoor", api_close_door},
    {"LockAllDoors", api_all_doors<true>},
    {"UnlockAllDoors", api_all_doors<false>},
    {"CloseAllDoors", api_close_all_doors},
    {"SetSkybox", api_set_skybox},
    {"Fog_SetColor", api_fog_color},
    {"Fog_SetNear", api_fog_value<1>},
    {"Fog_SetFar", api_fog_value<2>},
    {"Fog_SetDensity", api_fog_value<3>},
    {"TeleportPlayerToZone", api_teleport_player},
    {"PlayerMoveToSpawnpoint", api_teleport_player},
    {"EndLevel", api_end_level},
    {"StartEndLevelEffect", api_start_end_level_effect},
    {"IsLowDevice", api_false},
    {"IsLowDeviceForEffects", api_false},
    {"IsServer", api_false},
    {"GetNoEnemies", api_get_no_enemies},
    {"PostEffect_Enable", api_post_enable},
    {"StartEffect", api_start_effect},
    {"StartSound", api_start_sound},
    {"StopSound", api_stop_sound},
    {"StartSoundAtWaypoint", api_start_sound_at_waypoint},
    {"StartSoundFromEvent", api_start_sound_from_event},
    {"StopSoundFromEvent", api_stop_sound_from_event},
    {"StartMusic", api_start_music},
    {"StopMusic", api_stop_music},
    {"StopEffect", api_stop_effect},
    {"PostEffect_SetVar", api_post_set_var},
    {"PostEffect_SetParam", api_post_set_param},
    {"GetEnemies", api_get_enemies},
    {"KillAllEnemies", api_kill_all_enemies},
    {"GiveDamage", api_give_damage},
    {"EnableShootControl", api_enable_shoot_control},
    {"EnableMoveControl", api_enable_control<&LevelLogic::Controls::move>},
    {"EnableRotateControl", api_enable_control<&LevelLogic::Controls::rotate>},
    {"EnableJumpControl", api_enable_control<&LevelLogic::Controls::jump>},
    {"EnableIronSightControl", api_enable_control<&LevelLogic::Controls::aim>},
    {"EnableRushControl", api_enable_control<&LevelLogic::Controls::rush>},
    {"EnablePowersControl", api_enable_control<&LevelLogic::Controls::powers>},
    {"EnableWeaponsControl", api_enable_control<&LevelLogic::Controls::weapons>},
    {"EnableItemsControl", api_enable_control<&LevelLogic::Controls::items>},
    {"EnableHud", api_enable_hud},
    {"ForceInterfaceInCinematic", api_force_interface_in_cinematic},
    {"SetCrtWeaponState", api_set_crt_weapon_state},
    {"Test_distort_start", api_test_distort_start},
    {"Test_distort_stop", api_test_distort_stop},
    {"Test_change_blend", api_test_change_blend},
    {"UnlockWeapon", api_unlock_weapon},
    {"LockWeapon", api_lock_weapon},
    {"UseWeapon", api_use_weapon},
    {"GiveAmmo", api_give_ammo},
    {"GetHealthMC", api_get_health_mc},
    {"GiveDamageMC", api_give_damage_mc},
    {"GiveDamageToScriptTarget", api_give_damage_to_script_target},
    {"IsTriggerTarget", api_is_trigger_target},
    {"GetRandom", api_get_random},
    {"ShowMessage", api_show_message},
    {"ShowMessageEx", api_show_message},
    {"ShowDialogMessage", api_show_dialog},
    {"AddSecondaryObjective", api_add_secondary_objective},
    {"SetPrimaryObjective", api_set_primary_objective},
    {"StartBulletTime", api_start_bullet_time},
    {"EnablePath", api_enable_path},
    {"UnlockSpecialPower", api_special_power<0>},
    {"LockSpecialPower", api_special_power<1>},
    {"UseSpecialPower", api_special_power<2>},
    {"HUD_StartFrost", api_hud_frost<true>},
    {"HUD_StopFrost", api_hud_frost<false>},
    {"HUD_SetFrostMaxFactor", api_hud_frost_max},
    {"ChangeLightMap", api_change_light_map},
    {"SetFogZColor", api_set_fog_z_color},
    {"LoadObjectType", api_ignore},
    {"SetMaxActiveEnemies", api_set_max_active_enemies},
    {"SetMinEnemiesOnPlayer", api_set_min_enemies_on_player},
    {"RestoreMinEnemiesOnPlayer", api_restore_min_enemies_on_player},
    {"Player_EnableFallDamage", api_enable_fall_damage},
    {"HideMessage", api_hide_message},
    {"ResetTriggerRunCount", api_reset_trigger_run_count},
    {"IsPlayerLookingAt", api_is_player_looking_at},
    {"EnterInvisibility", api_invisibility<false>},
    {"ExitInvisibility", api_invisibility<true>},
    {"NpcCmd_Crouch", api_npc_crouch},
    {"Fog_AnimationStart", api_fog_animation_start},
    {"Fog_AnimationStop", api_fog_animation_stop},
    {"Vehicle_MoveOnPath", api_vehicle_move_on_path},
    {"Vehicle_Stop", api_vehicle_stop},
    {"Vehicle_SetMaxSpeed", api_vehicle_set<0>},
    {"Vehicle_SetSteeringMax", api_vehicle_set<1>},
    {"Vehicle_SetSteeringSpeed", api_vehicle_set<2>},
    {"Vehicle_SetYawLimits", api_vehicle_yaw_limits},
    {"Vehicle_Activate", api_vehicle_activate},
    {"Vehicle_SetDebug", api_ignore},
    {"SwitchToMachineGun", api_switch_to_machine_gun},
    {"SwitchToPlayerOnFoot", api_switch_to_player_on_foot},
    {"SwitchToMech", api_switch_to_mech},
    {"GiveSplashDamage", api_give_splash_damage},
    {"PlayAnimation", api_play_animation},
    {"LoadTexture", api_ignore},
    {"SetDSPPreset", api_set_dsp_preset},
    {"QTEAddSlowTime", api_qte_add_slow_time},
    {"QTEStart", api_qte_start},
    {"QTEStop", api_qte_stop},
    {"QTE_Fire", api_qte_fire},
    {"SetFOV", api_set_fov},
    {"ResetFOV", api_reset_fov},
    {"SetZFar", api_set_zfar},
    {"SetMoveSensitivity", api_set_move_sensitivity},
    {"PlayerEnableRush", api_player_enable_rush},
    {"SetColorGrading", api_set_color_grading},
    {"DisableColorGrading", api_disable_color_grading},
    {"LockTarget", api_lock_target},
    {"IgnoreTarget", api_ignore_target},
    {"Npc_SetPrimaryTarget", api_set_primary_target},
    {"SetGlobalAwarenessFactor", api_set_global_awareness_factor},
    {"SecondaryObjectiveComplete", api_secondary_objective_complete},
    {"PrimaryObjectiveComplete", api_primary_objective_complete},
    // Accepted without effect: saving, persistence of state the engine does not have yet.
    {"SaveGame", api_save_game},
    {"StartRotateTutorial", api_tutorial_step<kRotate>},
    {"StartMoveTutorial", api_tutorial_step<kMove>},
    {"StartJumpTutorial", api_tutorial_step<kJump>},
    {"StartReloadTutorial", api_tutorial_step<kReload>},
    {"StartShootTutorial", api_tutorial_step<kShoot>},
    {"StartChangeWeaponTutorial", api_tutorial_step<kChangeWeapon>},
    {"StartRushTutorial", api_tutorial_step<kRush>},
    {"StartIronSightTutorial", api_tutorial_step<kIronSight>},
    {"StartGrenadeTutorial", api_tutorial_step<kGrenade>},
    {"StartSpecialPowerTutorial", api_tutorial_step<kSpecialPower>},
    {"StartParalyzeTutorial", api_tutorial_step<kParalyze>},
    {"StartDetonateTutorial", api_tutorial_step<kDetonate>},
    {"StartShieldTutorial", api_tutorial_step<kShield>},
    {"TutorialMessage", api_tutorial_message},
    {"StartTutorial", api_return_now},
    {"StopTutorial", api_return_now},
    {"EndTutorial", api_return_now},
    {"StartCinematicObject", api_start_cinematic_object},
    {"EndCinematicObject", api_end_cinematic_object},
    {"StartCinematic", api_cinematic_mode<true>},
    {"EndCinematic", api_cinematic_mode<false>},
    {"PlayCameraAnim", api_play_camera_anim},
    {"PlayerLookTo", api_player_look_to},
    {"PlayerLookToDirection", api_player_look_to_direction},
    {"CameraLookToWaypoint", api_camera_look_to_waypoint},
    {"CameraSwitchToPlayer", api_camera_to_player<false>},
    {"ResetPlayerCamera", api_camera_to_player<true>},
    {"SetControlSensitivity", api_set_control_sensitivity},
    {"Npc_SetCustomIdleState", api_npc_custom_idle},
    {"Vehicle_FollowObject", api_vehicle_follow},
    {"IsParalyzed", api_is_paralyzed},
    {"GetZone", api_get_zone},
    {"GetObjType", api_get_obj_type},
    {"Npc_SetBulletType", api_npc_set_bullet_type},
    {"Npc_Link", api_npc_link},
    {"Npc_UnLink", api_npc_unlink},
    {"StartTimer", api_start_timer},
    {"PauseTimer", api_pause_timer},
    {"NpcCmd_TeleportToTarget", api_npc_teleport_to_target},
    {"SwitchToPlayerLimitedControl", api_limited_begin},
    {"PlayerLimitedControlSetState", api_limited_state},
    {"PlayerLimitedControlStartHang", api_limited_hang},
    {"PlayerLimitedControlUseGravity", api_limited_gravity},
    {"PlayerLimitedControlSetLimitYaw", api_limited_limit<0>},
    {"PlayerLimitedControlSetLimitPitch", api_limited_limit<1>},
    {"SetLimitedControlStrafeSpeed", api_limited_strafe},
    {"PlayerSetPosition", api_player_set_position},
    {"Npc_GetPosture", api_npc_posture},
    {"LockEnemyAggro", api_lock_enemy_aggro},
    {"Npc_GetAccuracy", api_npc_accuracy},
    {"Npc_SetAttackBehavior", api_npc_attack_behaviour},
    {"LockElevator", api_lock_elevator<true>},
    {"UnlockElevator", api_lock_elevator<false>},
    {"PostEffect_FlashbackStart", api_flashback_start},
    {"PostEffect_FlashbackStop", api_flashback_stop},
    {"StartFlashEffect", api_start_flash},
    {"PlayerStartCrystalInteract", api_ignore},   // empty in the game too
    {"SetZNear", api_ignore},                     // reads its argument and does nothing
    {"HackTurret", api_ignore},                   // Therrius 2 calls it on turrets the level does not have
    {"AddObjective", api_add_objective},
    {"GameObject_Material_SetTechnique", api_material_technique},
    {"GameObject_Material_SetParameterFloat", api_material_float},
    {"GameObject_Material_SetParameterVector3", api_material_vector3},
    {"GameObject_Material_SetParameterAnimatedFloat", api_material_animated_float},
    {"SetAttractionDirection", api_set_attraction},
    {"Vehicle_Deactivate", api_vehicle_deactivate},
    {"NPCSuperSpeedToWaypoint", api_npc_super_speed},
    {"PostEffect_HeatHazeStart", api_heat_haze_start},
    {"PostEffect_HeatHazeStop", api_heat_haze_stop},
    {"RemoveObjective", api_remove_objective},
    {"GameObject_MotionOnPathSetFollowTarget", api_motion_follow},
    {"ShowProgressSavedMessage", api_progress_saved},
    {"Fog_SetLock", api_fog_lock},
    {"EnablePortal", api_portal<true>},
    {"LoadLevelSections", api_load_level_sections},
    {"DisablePortal", api_portal<false>},
    {"SetInvulnerable", api_set_invulnerable},
    {"SetInteractiveMusicState", api_set_interactive_music_state},
    {"EnableMotionBlur", api_enable_motion_blur},
};

}  // namespace

void register_game_api(LevelLogic& logic) {
    lua_State* L = logic.runtime_.state();
    for (const char* name : kGameFunctions) {
        lua_pushlightuserdata(L, &logic);
        lua_pushstring(L, name);
        lua_pushcclosure(L, not_implemented, 2);
        lua_setglobal(L, name);
    }
    for (const Entry& e : kImplemented) logic.runtime_.register_function(e.name, e.fn, &logic);
    // The NPC constants the AI scripts compare with (the order of the game's posture names).
    logic.runtime_.load("NPC = { PostureStand = 0, PostureCover = 1, PostureCoverFire = 2, PostureMachineGun = 3, "
                        "PostureHiCover = 4, PostureCrouch = 5 }", "NPC constants");
}

}  // namespace nova
