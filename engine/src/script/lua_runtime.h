// Lua 5.1 runtime for the level scripts, modelled on the game's CLuaScriptManager.
//
// Script chunks define global functions named Obj<id>_<Event>. Starting one runs it
// on its own coroutine (at most 100 at once; a function that is still running is not
// started again). C functions suspend the coroutine by yielding:
//   (2, ms)          Wait: resume after `ms` milliseconds
//   (4, name, ids...) WaitForEvent: resume when the event is sent (for each listed id,
//                    e.g. WaitForEnemiesDead's objects; none: any)
//   (3, code, obj)   wait for an effect (teleport-in); resumed after a short delay
// The global `this` holds the owning object's id while a coroutine runs.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace nova {

class ScriptRuntime {
public:
    ScriptRuntime();
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    using Function = int (*)(lua_State*);
    // Register a global C function; `context` is available to it as upvalue 1.
    void register_function(const std::string& name, Function fn, void* context);

    // Run a chunk (normally just function definitions). False on a syntax/runtime error.
    bool load(const std::string& source, const std::string& chunk_name);
    bool has_function(const std::string& name) const;

    // Queue `function` to start on a new coroutine with `this` = owner. False if it does
    // not exist, is already running, or the thread limit is reached.
    bool start(const std::string& function, int owner);
    bool running(const std::string& function) const;

    // Advance timers and resume coroutines that are ready.
    void update(float dt_ms);
    // Resume coroutines waiting for `event`: those that waited for any id, and those
    // whose last outstanding id this is.
    void send_event(const std::string& event, int id = -1);

    void stop_all();
    size_t thread_count() const;

    lua_State* state() const { return L_; }
    std::function<void(const std::string&)> log = [](const std::string&) {};

    static constexpr int kMaxThreads = 100;

private:
    struct Thread {
        enum class State { Free, Ready, Sleeping, WaitingEvent } state = State::Free;
        lua_State* L = nullptr;
        int ref = -1;                 // registry reference keeping the coroutine alive
        std::string function;
        int owner = 0;
        float sleep_ms = 0;
        std::string event;
        std::vector<int> event_ids;   // ids still to come; empty: any
        bool started = false;         // resumed at least once (arguments consumed)
    };
    void resume(Thread& t);
    void finish(Thread& t);

    lua_State* L_ = nullptr;
    std::vector<Thread> threads_;
};

}  // namespace nova
