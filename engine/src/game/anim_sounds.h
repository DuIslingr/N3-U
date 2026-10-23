// Sounds and particle effects on animation clips (CStatesSetComponent "Sound" track
// events and each clip's effect tracks): each object type's states list clips with timed
// events naming sounds (spider footsteps, door motors) or effects at nodes (a ship's
// explosion, a jeep's wreck smoke). Played as an object's current clip passes their times.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"

namespace nova {

class Scene;
class Audio;

class AnimSounds {
public:
    explicit AnimSounds(const Gol& gol) : gol_(gol) {}
    // Play the events object `index` (of gol type `type`) has passed since the last call
    // (sounds only with `audio`).
    void update(Scene& scene, Audio* audio, size_t index, int type);
    void clear() { last_.clear(); }
    // A state of the type's state set: its first clip and the sounds of its events
    // (each a list to pick one from). Null if the type has no such state.
    // A timed sound event of a clip.
    struct Event {
        float ms;
        std::vector<std::string> sounds;
        bool stop = false;   // stops the sound instead of playing it
    };
    struct State {
        std::string clip;
        std::vector<std::string> clips;                      // every clip of it (one is picked)
        std::vector<Event> sounds;
        bool loop = false;
        std::string next;                                    // the state after its clip ("" none)
        std::vector<std::pair<float, std::string>> scripts;  // Script tracks: (ms, function)
    };
    const State* state(int type, const std::string& name);
    // Play (or stop) a state's sounds at `at`; returns the handles of those started.
    std::vector<int> play_state(Audio& audio, int type, const std::string& name, const glm::vec3& at);

private:
    using ClipEvents = std::unordered_map<std::string, std::vector<Event>>;   // by lowercase clip name
    struct EffectEvent {
        float ms;
        std::string effect, node;
    };
    struct TypeInfo {
        ClipEvents clips;
        std::unordered_map<std::string, std::vector<EffectEvent>> effects;   // by lowercase clip name
        std::unordered_map<std::string, State> states;   // by lowercase name
    };
    const TypeInfo& info(int type);
    const ClipEvents& events(int type) { return info(type).clips; }

    const Gol& gol_;
    std::unordered_map<int, TypeInfo> by_type_;
    struct Last {
        uint32_t serial = 0;
        float ms = 0;
    };
    std::unordered_map<size_t, Last> last_;
};

}  // namespace nova
