#include "game/anim_sounds.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>

#include "audio/audio.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompStatesSet = 0x0e;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const AnimSounds::TypeInfo& AnimSounds::info(int type) {
    if (auto it = by_type_.find(type); it != by_type_.end()) return it->second;
    TypeInfo& out = by_type_[type];
    auto g = gol_.find(type);
    const Value* v = g == gol_.end() ? nullptr : resolved_template(g->second, gol_, kCompStatesSet);
    if (!v || v->size() < 2) return out;
    // States: [name, ..., [clips]] (clips at [11]); a clip: [name, speed, track groups...];
    // a group: [name, kind, [events]]; a sound event: [name, start ms, end ms, ..., [[sound]...] at 12].
    std::unordered_map<std::string, bool> ambiguous;     // clips whose states disagree on events
    for (const Value& st : (*v)[1].list) {
        if (st.size() < 12 || st[0].type != Value::String) continue;
        State& state = out.states[lower(st[0].s)];
        for (const Value& clip : st[11].list) {
            if (clip.size() < 3 || clip[0].type != Value::String) continue;
            if (state.clip.empty()) state.clip = clip[0].s;
            std::vector<Event> list;
            for (size_t gi = 2; gi < clip.size(); ++gi)
                for (const Value& group : clip[gi].list) {
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
    for (const Value& st : (*v)[1].list) {
        if (st.size() < 12) continue;
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

void AnimSounds::update(Scene& scene, Audio* audio, size_t index, int type) {
    const TypeInfo& ti = info(type);
    const ClipEvents& all = ti.clips;
    if (all.empty() && ti.effects.empty()) return;
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
