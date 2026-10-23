#include "game/anim_sounds.h"

#include <algorithm>
#include <cctype>

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

const AnimSounds::ClipEvents& AnimSounds::events(int type) {
    if (auto it = by_type_.find(type); it != by_type_.end()) return it->second;
    ClipEvents& out = by_type_[type];
    auto g = gol_.find(type);
    const Value* v = g == gol_.end() ? nullptr : resolved_template(g->second, gol_, kCompStatesSet);
    if (!v || v->size() < 2) return out;
    // States: [name, ..., [clips]] (clips at [11]); a clip: [name, speed, track groups...];
    // a group: [kind, kind, [events]]; a sound event: [name, start ms, end ms, ..., [[sound]...] at 12].
    for (const Value& st : (*v)[1].list) {
        if (st.size() < 12) continue;
        for (const Value& clip : st[11].list) {
            if (clip.size() < 3 || clip[0].type != Value::String) continue;
            std::vector<Event>& list = out[lower(clip[0].s)];
            for (size_t gi = 2; gi < clip.size(); ++gi)
                for (const Value& group : clip[gi].list) {
                    if (group.size() < 3 || group[0].s != "Sound") continue;
                    for (const Value& ev : group[2].list) {
                        if (ev.size() < 13) continue;
                        Event e;
                        e.ms = ev[1].as_float();
                        for (const Value& snd : ev[12].list)
                            if (snd.size() > 0 && snd[0].type == Value::String) e.sounds.push_back(snd[0].s);
                        if (!e.sounds.empty() &&
                            std::none_of(list.begin(), list.end(), [&](const Event& x) { return x.ms == e.ms && x.sounds == e.sounds; }))
                            list.push_back(std::move(e));
                    }
                }
        }
    }
    return out;
}

void AnimSounds::update(Scene& scene, Audio& audio, size_t index, int type) {
    const ClipEvents& all = events(type);
    if (all.empty()) return;
    Scene::ClipState cs;
    if (!scene.clip_state(index, cs) || cs.reverse) return;
    Last& last = last_[index];
    float from = last.ms;
    if (last.serial != cs.serial) from = -1;   // a new clip: events at its start count
    last.serial = cs.serial;
    last.ms = cs.ms;
    auto it = all.find(lower(cs.name));
    if (it == all.end()) return;
    const glm::vec3 at(scene.object_matrix(index)[3]);
    Audio::Params p;
    p.position = &at;
    for (const Event& e : it->second) {
        // Passed since last time: (from, ms], or across the loop's end.
        const bool hit = cs.ms >= from ? (e.ms > from && e.ms <= cs.ms) : (e.ms > from || e.ms <= cs.ms);
        if (hit) audio.play_one_of(e.sounds, p);
    }
}

}  // namespace nova
