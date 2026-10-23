#include "script/lua_runtime.h"

#include <algorithm>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace nova {

namespace {

int lua_print(lua_State* L) {
    auto* rt = static_cast<ScriptRuntime*>(lua_touserdata(L, lua_upvalueindex(1)));
    std::string line;
    for (int i = 1, n = lua_gettop(L); i <= n; ++i) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        if (const char* s = lua_tostring(L, -1)) line += (i > 1 ? "\t" : "") + std::string(s);
        lua_pop(L, 1);
    }
    rt->log("print: " + line);
    return 0;
}

}  // namespace

ScriptRuntime::ScriptRuntime() {
    L_ = luaL_newstate();
    luaL_openlibs(L_);
    threads_.resize(kMaxThreads);
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, lua_print, 1);
    lua_setglobal(L_, "print");
}

ScriptRuntime::~ScriptRuntime() { lua_close(L_); }

void ScriptRuntime::register_function(const std::string& name, Function fn, void* context) {
    lua_pushlightuserdata(L_, context);
    lua_pushcclosure(L_, fn, 1);
    lua_setglobal(L_, name.c_str());
}

bool ScriptRuntime::load(const std::string& source, const std::string& chunk_name) {
    if (luaL_loadbuffer(L_, source.data(), source.size(), chunk_name.c_str()) != 0 || lua_pcall(L_, 0, 0, 0) != 0) {
        log(std::string("error in ") + chunk_name + ": " + (lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?"));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool ScriptRuntime::has_function(const std::string& name) const {
    lua_getglobal(L_, name.c_str());
    const bool ok = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    return ok;
}

bool ScriptRuntime::running(const std::string& function) const {
    for (const Thread& t : threads_)
        if (t.state != Thread::State::Free && t.function == function) return true;
    return false;
}

size_t ScriptRuntime::thread_count() const {
    size_t n = 0;
    for (const Thread& t : threads_) n += t.state != Thread::State::Free;
    return n;
}

bool ScriptRuntime::start(const std::string& function, int owner) {
    if (!has_function(function) || running(function)) return false;
    for (Thread& t : threads_) {
        if (t.state != Thread::State::Free) continue;
        t.L = lua_newthread(L_);
        t.ref = luaL_ref(L_, LUA_REGISTRYINDEX);
        lua_getglobal(t.L, function.c_str());
        t.function = function;
        t.owner = owner;
        t.state = Thread::State::Ready;
        t.started = false;
        t.event.clear();
        t.event_ids.clear();
        return true;
    }
    log("too many script threads; not starting " + function);
    return false;
}

void ScriptRuntime::finish(Thread& t) {
    luaL_unref(L_, LUA_REGISTRYINDEX, t.ref);
    t = Thread{};
}

void ScriptRuntime::resume(Thread& t) {
    lua_pushinteger(t.L, t.owner);
    lua_setglobal(t.L, "this");
    const int status = lua_resume(t.L, 0);
    t.started = true;
    if (status == LUA_YIELD) {
        // Yielded values: a code, then its arguments (see the header).
        const int n = lua_gettop(t.L);
        const int code = n >= 1 ? int(lua_tointeger(t.L, 1)) : 0;
        if (code == 2) {
            t.state = Thread::State::Sleeping;
            t.sleep_ms = n >= 2 ? float(lua_tonumber(t.L, 2)) : 0.0f;
        } else if (code == 4) {
            t.state = Thread::State::WaitingEvent;
            t.event = n >= 2 && lua_isstring(t.L, 2) ? lua_tostring(t.L, 2) : "";
            t.event_ids.clear();
            for (int k = 3; k <= n; ++k)
                if (const int id = int(lua_tointeger(t.L, k)); id >= 0) t.event_ids.push_back(id);
        } else if (code == 3) {
            t.state = Thread::State::Sleeping;   // teleport-in effect
            t.sleep_ms = 500;
        } else {
            t.state = Thread::State::Sleeping;   // plain coroutine.yield: next update
            t.sleep_ms = 0;
        }
        lua_settop(t.L, 0);
        return;
    }
    if (status != 0) log("error in " + t.function + ": " + (lua_tostring(t.L, -1) ? lua_tostring(t.L, -1) : "?"));
    finish(t);
}

void ScriptRuntime::update(float dt_ms) {
    // Resume in slot order; coroutines started during this update wait for the next one.
    std::vector<size_t> ready;
    for (size_t i = 0; i < threads_.size(); ++i) {
        Thread& t = threads_[i];
        if (t.state == Thread::State::Sleeping) {
            t.sleep_ms -= dt_ms;
            if (t.sleep_ms <= 0) t.state = Thread::State::Ready;
        }
        if (t.state == Thread::State::Ready) ready.push_back(i);
    }
    for (size_t i : ready)
        if (threads_[i].state == Thread::State::Ready) resume(threads_[i]);
}

void ScriptRuntime::send_event(const std::string& event, int id) {
    for (Thread& t : threads_) {
        if (t.state != Thread::State::WaitingEvent || t.event != event) continue;
        if (!t.event_ids.empty() && id >= 0) {
            auto it = std::find(t.event_ids.begin(), t.event_ids.end(), id);
            if (it == t.event_ids.end()) continue;
            t.event_ids.erase(it);
            if (!t.event_ids.empty()) continue;
        }
        t.state = Thread::State::Ready;
    }
}

void ScriptRuntime::stop_all() {
    for (Thread& t : threads_)
        if (t.state != Thread::State::Free) finish(t);
}

}  // namespace nova
