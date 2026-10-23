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

// Particle effects: StartEffect(id[, index]) / StopEffect(id[, index]); id 0 is the player.
int api_start_effect(lua_State* L) {
    LevelLogic& g = ctx(L);
    const int id = int(lua_tointeger(L, 1)), index = int(lua_tointeger(L, 2));
    if (LevelLogic::Object* o = id ? g.object(id) : nullptr) g.start_effect(*o, index);
    else if (g.warned_.insert("StartEffect " + std::to_string(id)).second) g.log("StartEffect: no effects on object " + std::to_string(id));
    return 0;
}

int api_stop_effect(lua_State* L) {
    LevelLogic& g = ctx(L);
    if (LevelLogic::Object* o = g.object(int(lua_tointeger(L, 1)))) g.stop_effect(*o, int(lua_tointeger(L, 2)));
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
int api_get_health_mc(lua_State* L) {   // the player ("MC": main character)
    Combat* c = ctx(L).combat;
    lua_pushnumber(L, c ? c->player_health : 100);
    return 1;
}
int api_give_damage_mc(lua_State* L) {   // GiveDamageMC(amount, ...)
    if (Combat* c = ctx(L).combat) c->damage_player(float(lua_tonumber(L, 1)), ctx(L).player.center);
    return 0;
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
// ShowDialogMessage(line, line, ..., flag): voice-over lines played in order.
int api_show_dialog(lua_State* L) {
    std::vector<std::string> lines;
    for (int i = 1; i < lua_gettop(L); ++i)
        if (lua_type(L, i) == LUA_TSTRING && lua_tostring(L, i)[0]) lines.push_back(lua_tostring(L, i));
    ctx(L).dialog(lines);
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
    {"GetHealthMC", api_get_health_mc},
    {"GiveDamageMC", api_give_damage_mc},
    {"GetRandom", api_get_random},
    {"ShowMessage", api_show_message},
    {"ShowMessageEx", api_show_message},
    {"ShowDialogMessage", api_show_dialog},
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
    {"SetInvulnerable", api_set_invulnerable},
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
