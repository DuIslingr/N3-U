#include "game/anim_sounds.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <unordered_set>

#include "audio/audio.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompStatesSet = 0x0e;
constexpr int kCompActorSounds = 0x5a;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const AnimSounds::TypeInfo& AnimSounds::info(int type) {
    if (auto it = by_type_.find(type); it != by_type_.end()) return it->second;
    TypeInfo& out = by_type_[type];
    // Its footsteps: [0] {{material, {walk}, {run}, {jump}, {land}, {roll}, {fall}, {sprint}}...}.
    if (auto g = gol_.find(type); g != gol_.end())
        if (const Value* v = resolved_template(g->second, gol_, kCompActorSounds); v && v->size() > 0)
            for (const Value& e : (*v)[0].list) {
                if (e.size() < 2) continue;
                StepSet set;
                set.material = e[0].i;
                for (size_t k = 0; k < 7 && k + 1 < e.size(); ++k)
                    for (const Value& snd : e[k + 1].list)
                        if (snd.size() > 0 && snd[0].type == Value::String) set.sounds[k].push_back(snd[0].s);
                out.steps.push_back(std::move(set));
            }
    // The type's states and those of the state sets it derives from (marines keep theirs in
    // a StateSet* parent; the type's own come first).
    std::vector<const Value*> states;
    std::unordered_set<std::string> named;
    std::unordered_set<int> seen;
    std::function<void(int)> gather = [&](int id) {
        auto g = gol_.find(id);
        if (g == gol_.end() || !seen.insert(id).second) return;
        if (auto t = g->second.templates.find(kCompStatesSet); t != g->second.templates.end() && t->second.size() > 1)
            for (const Value& st : t->second[1].list)
                if (st.size() >= 12 && st[0].type == Value::String && named.insert(lower(st[0].s)).second) states.push_back(&st);
        for (int p : g->second.parents) gather(p);
    };
    gather(type);
    if (states.empty()) return out;
    // States: [name, ..., [clips]] (clips at [11]); a clip: [name, speed, track groups...];
    // a group: [name, kind, [events]]; a sound event: [name, start ms, end ms, ..., [[sound]...] at 12].
    std::unordered_map<std::string, bool> ambiguous;     // clips whose states disagree on events
    for (const Value* sp : states) {
        const Value& st = *sp;
        State& state = out.states[lower(st[0].s)];
        // [2]: it loops; [6]: the state that follows when its clip has played (0: none).
        state.loop = st[2].i != 0;
        if (const int next = st[6].i; next > 0 && size_t(next) < states.size()) state.next = states[size_t(next)]->list[0].s;
        for (const Value& clip : st[11].list) {
            if (clip.size() < 3 || clip[0].type != Value::String) continue;
            if (state.clip.empty()) state.clip = clip[0].s;
            state.clips.push_back(clip[0].s);
            state.speeds.push_back(clip[1].as_float());
            std::vector<Event> list;
            for (size_t gi = 2; gi < clip.size(); ++gi)
                for (const Value& group : clip[gi].list) {
                    // Script tracks: [name, start ms, end ms, code (function Templ<type>_..._Run), ...].
                    if (group.size() >= 3 && group[1].s == "Script")
                        for (const Value& ev : group[2].list) {
                            if (ev.size() < 4 || ev[3].type != Value::String) continue;
                            const std::string& code = ev[3].s;
                            const size_t f = code.find("function ");
                            if (f == std::string::npos) continue;
                            const size_t b = f + 9, e = code.find('(', b);
                            if (e != std::string::npos) state.scripts.push_back({ev[1].as_float(), code.substr(b, e - b)});
                        }
                    if (group.size() >= 3 && group[1].s == "GameEvent")   // [name, start ms, end ms, id, argument]
                        for (const Value& ev : group[2].list) {
                            if (ev.size() < 5 || (ev[3].i != 37 && ev[3].i != 43 && ev[3].i != 44)) continue;
                            std::vector<ActorEvent>& list = out.actor_events[lower(clip[0].s)];
                            const ActorEvent e{ev[1].as_float(), ev[3].i, ev[4].i};
                            if (std::none_of(list.begin(), list.end(), [&](const ActorEvent& x) { return x.ms == e.ms && x.id == e.id; }))
                                list.push_back(e);
                        }
                    if (group.size() < 3 || group[1].s != "Sound") continue;
                    for (const Value& ev : group[2].list) {
                        if (ev.size() < 13) continue;
                        Event e;
                        e.ms = ev[1].as_float();
                        e.stop = ev[3].i == 0;   // [3]: 1 plays the sound, 0 stops it
                        for (const Value& snd : ev[12].list)
                            if (snd.size() > 0 && snd[0].type == Value::String) e.sounds.push_back(snd[0].s);
                        if (e.sounds.empty()) continue;
                        state.sounds.push_back(e);
                        if (std::none_of(list.begin(), list.end(), [&](const Event& x) { return x.ms == e.ms && x.sounds == e.sounds; }))
                            list.push_back(std::move(e));
                    }
                }
            // A clip several states share plays the sounds of all of them, unless one of
            // them plays it silently (the elevator's "idle" is silent when idle and starts
            // the motor in MoveStart): then the clip alone says nothing.
            const std::string key = lower(clip[0].s);
            if (list.empty()) ambiguous[key] = true;
            std::vector<Event>& merged = out.clips[key];
            for (Event& e : list)
                if (std::none_of(merged.begin(), merged.end(), [&](const Event& x) { return x.ms == e.ms && x.sounds == e.sounds; }))
                    merged.push_back(std::move(e));
        }
    }
    // Effect tracks (a clip's [5]): [start ms, end ms, effect, node, scale x, y, z, ...].
    for (const Value* sp : states) {
        const Value& st = *sp;
        for (const Value& clip : st[11].list) {
            if (clip.size() < 6 || clip[0].type != Value::String) continue;
            std::vector<EffectEvent>& list = out.effects[lower(clip[0].s)];
            std::function<void(const Value&)> walk = [&](const Value& x) {
                if (x.type != Value::List) return;
                if (x.size() >= 4 && x[2].type == Value::String && x[3].type == Value::String && x[0].type != Value::List) {
                    EffectEvent e{x[0].as_float(), x[2].s, x[3].s};
                    if (!e.effect.empty() &&
                        std::none_of(list.begin(), list.end(), [&](const EffectEvent& y) { return y.ms == e.ms && y.effect == e.effect && y.node == e.node; }))
                        list.push_back(e);
                    return;
                }
                for (const Value& y : x.list) walk(y);
            };
            walk(clip[5]);
            if (list.empty()) out.effects.erase(lower(clip[0].s));
        }
    }
    for (const auto& [key, bad] : ambiguous) out.clips.erase(key);
    for (auto it = out.clips.begin(); it != out.clips.end();) it = it->second.empty() ? out.clips.erase(it) : std::next(it);
    return out;
}

std::vector<int> AnimSounds::play_state(Audio& audio, int type, const std::string& name, const glm::vec3& at) {
    std::vector<int> started;
    const State* st = state(type, name);
    if (!st) return started;
    Audio::Params p;
    p.position = &at;
    for (const Event& e : st->sounds) {
        if (e.stop) {
            for (const std::string& s : e.sounds) audio.stop(s, 200);
        } else if (const int h = audio.play_one_of(e.sounds, p); h >= 0) {
            started.push_back(h);
        }
    }
    return started;
}

const AnimSounds::State* AnimSounds::state(int type, const std::string& name) {
    const TypeInfo& t = info(type);
    auto it = t.states.find(lower(name));
    return it == t.states.end() ? nullptr : &it->second;
}

int AnimSounds::ground_material(const Scene& scene, const glm::vec3& p) {
    float z = 0;
    uint32_t tri = 0;
    if (!scene.collision().ray_down(p + glm::vec3(0, 0, 0.5f), 2.5f, z, nullptr, &tri)) return -1;
    return scene.collision().material(tri);
}

void AnimSounds::footstep(Audio& audio, int type, int material, Step kind, const glm::vec3* at) {
    const TypeInfo& ti = info(type);
    if (ti.steps.empty()) return;
    // CActorSoundsComponent::StepsSoundsIdx: the set for the material, else the metal one (1).
    const StepSet* set = &ti.steps[0];
    for (const StepSet& s : ti.steps)
        if (s.material == 1) set = &s;
    for (const StepSet& s : ti.steps)
        if (s.material == material) set = &s;
    const std::vector<std::string>& sounds = set->sounds[int(kind)];
    if (sounds.empty()) return;
    Audio::Params p;
    p.position = at;
    audio.play_one_of(sounds, p);
}

void AnimSounds::update(Scene& scene, Audio* audio, size_t index, int type) {
    const TypeInfo& ti = info(type);
    const ClipEvents& all = ti.clips;
    if (all.empty() && ti.effects.empty() && ti.actor_events.empty()) return;
    Scene::ClipState cs;
    if (!scene.clip_state(index, cs) || cs.reverse) return;
    Last& last = last_[index];
    float from = last.ms;
    if (last.serial != cs.serial) from = -1;   // a new clip: events at its start count
    last.serial = cs.serial;
    last.ms = cs.ms;
    const std::string clip = lower(cs.name);
    auto passed = [&](float ms) { return cs.ms >= from ? (ms > from && ms <= cs.ms) : (ms > from || ms <= cs.ms); };
    if (auto fx = ti.effects.find(clip); fx != ti.effects.end())
        for (const EffectEvent& e : fx->second)
            if (passed(e.ms)) scene.particles().start(e.effect, scene.object_matrix(index, e.node));
    if (auto ae = ti.actor_events.find(clip); audio && ae != ti.actor_events.end() && !ti.steps.empty()) {
        const glm::vec3 feet(scene.object_matrix(index)[3]);
        int material = -2;
        for (const ActorEvent& e : ae->second) {
            if (!passed(e.ms)) continue;
            if (material == -2) material = ground_material(scene, feet);
            const Step kind = e.id == 37 ? (e.arg > 0 ? Step::Run : Step::Walk) : e.id == 43 ? Step::Roll : e.arg != 0 ? Step::Jump : Step::Land;
            footstep(*audio, type, material, kind, &feet);
        }
    }
    auto it = all.find(clip);
    if (it == all.end() || !audio) return;
    const glm::vec3 at(scene.object_matrix(index)[3]);
    Audio::Params p;
    p.position = &at;
    for (const Event& e : it->second) {
        // Passed since last time: (from, ms], or across the loop's end.
        const bool hit = cs.ms >= from ? (e.ms > from && e.ms <= cs.ms) : (e.ms > from || e.ms <= cs.ms);
        if (!hit) continue;
        if (e.stop)
            for (const std::string& s : e.sounds) audio->stop(s, 200);
        else
            audio->play_one_of(e.sounds, p);
    }
}

}  // namespace nova
