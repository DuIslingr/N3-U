// The game's script functions (the C functions libNOVA3_neon.so registers with Lua).
// Every name the game registers exists; those not implemented yet log their first use
// and return nothing. Behaviour follows the decompiled originals where noted.
#include <cstdlib>
#include <random>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

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

// WaitForEvent(name): yield (4, name, id). "AllEnemiesDead" returns at once when there are
// no enemies, "SpawnEnd_<id>" when that spawn manager has finished (none run yet).
int api_wait_for_event(lua_State* L) {
    const std::string name = str_arg(L, 1);
    if (name == "AllEnemiesDead" || name.rfind("SpawnEnd_", 0) == 0) return 0;
    lua_pushinteger(L, 4);
    lua_pushstring(L, name.c_str());
    lua_pushinteger(L, -1);
    return lua_yield(L, 3);
}

int api_return_now(lua_State*) { return 0; }   // waits on things that have already happened

// SendEvent(id, name, ...): wake scripts waiting for it, and run Obj<id>_<name> if defined.
int api_send_event(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int(lua_tointeger(L, 1));
    const std::string name = str_arg(L, 2);
    if (c.trace) c.log("SendEvent " + std::to_string(id) + " " + name);
    c.runtime_.send_event(name, id);
    c.start_script("Obj" + std::to_string(id) + "_" + name, id);
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
    if (auto* o = obj(L, 1)) o->state = str_arg(L, 2);
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
int api_set_position(lua_State* L) {
    if (auto* o = obj(L, 1)) o->position = {lua_tonumber(L, 2), lua_tonumber(L, 3), lua_tonumber(L, 4)};
    return 0;
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
int api_end_level(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.level_ended = true;
    c.message("Level complete", 10);
    return 0;
}
int api_false(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
int api_zero(lua_State* L) {
    lua_pushinteger(L, 0);
    return 1;
}
int api_empty_table(lua_State* L) {   // GetEnemies(): the active enemies' ids (none yet)
    lua_newtable(L);
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

// --- messages (text ids into the string tables; shown raw for now) ------------------

// ShowMessage(text, ?, ?, ms, ?): "MessageEnd" is sent when it has been shown.
int api_show_message(lua_State* L) {
    LevelLogic& c = ctx(L);
    float seconds = 3;
    for (int i = 2; i <= lua_gettop(L); ++i)
        if (lua_type(L, i) == LUA_TNUMBER && lua_tonumber(L, i) >= 500) seconds = float(lua_tonumber(L, i)) / 1000.0f;
    c.message(str_arg(L, 1), seconds);
    c.send_event_later("MessageEnd", seconds);
    return 0;
}
int api_objective(lua_State* L) {
    ctx(L).message("Objective: " + str_arg(L, 1), 5);
    return 0;
}

struct Entry {
    const char* name;
    ScriptRuntime::Function fn;
};
const Entry kImplemented[] = {
    {"Wait", api_wait},
    {"WaitForEvent", api_wait_for_event},
    {"WaitForEnemiesDead", api_return_now},
    {"WaitNpcCmdFinished", api_return_now},
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
    {"IsLowDevice", api_false},
    {"IsLowDeviceForEffects", api_false},
    {"IsServer", api_false},
    {"GetNoEnemies", api_zero},
    {"GetEnemies", api_empty_table},
    {"GetRandom", api_get_random},
    {"ShowMessage", api_show_message},
    {"ShowMessageEx", api_show_message},
    {"ShowDialogMessage", api_show_message},
    {"AddObjective", api_objective},
    {"AddSecondaryObjective", api_objective},
    {"SetPrimaryObjective", api_objective},
    // Accepted without effect: saving, persistence of state the engine does not have yet.
    {"SaveGame", api_return_now},
    {"ShowProgressSavedMessage", api_return_now},
    {"Fog_SetLock", api_fog_lock},
    {"EnablePortal", api_portal<true>},
    {"LoadLevelSections", api_load_level_sections},
    {"DisablePortal", api_portal<false>},
    {"SetInvulnerable", api_return_now},
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
}

}  // namespace nova
