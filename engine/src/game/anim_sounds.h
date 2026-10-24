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
        std::vector<float> speeds;                           // each clip's speed (negative: backwards)
        std::vector<Event> sounds;
        bool loop = false;
        std::string next;                                    // the state after its clip ("" none)
        std::vector<std::pair<float, std::string>> scripts;  // Script tracks: (ms, function)
    };
    const State* state(int type, const std::string& name);
    // Play (or stop) a state's sounds at `at`; returns the handles of those started.
    std::vector<int> play_state(Audio& audio, int type, const std::string& name, const glm::vec3& at);
    // CActorSoundsComponent (0x5a): the type's sound for a footstep and the like on ground of
    // collision material `material` (its first set if it has none for it), played at `at`.
    enum class Step { Walk, Run, Jump, Land, Roll, Fall, Sprint };
    void footstep(Audio& audio, int type, int material, Step kind, const glm::vec3* at);
    // The collision material under `p` (within 2 m), -1 if none.
    static int ground_material(const Scene& scene, const glm::vec3& p);

private:
    using ClipEvents = std::unordered_map<std::string, std::vector<Event>>;   // by lowercase clip name
    struct EffectEvent {
        float ms;
        std::string effect, node;
    };
    // A clip's GameEvents an actor's sounds answer (CActorSoundsComponent::OnEvent: 37 a
    // footstep, its argument running; 43 a roll; 44 landing, or jumping with an argument).
    struct ActorEvent {
        float ms;
        int id, arg;
    };
    struct StepSet {
        int material = 0;
        std::vector<std::string> sounds[7];   // by Step
    };
    struct TypeInfo {
        ClipEvents clips;
        std::unordered_map<std::string, std::vector<ActorEvent>> actor_events;   // by lowercase clip name
        std::vector<StepSet> steps;
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
