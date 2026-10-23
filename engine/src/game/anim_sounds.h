// Sounds on animation clips (CStatesSetComponent "Sound" track events): each object type's
// states list clips with timed events naming sounds (e.g. spider footsteps, door motors).
// Played as an object's current clip passes their times.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "assets/level.h"

namespace nova {

class Scene;
class Audio;

class AnimSounds {
public:
    explicit AnimSounds(const Gol& gol) : gol_(gol) {}
    // Play the events object `index` (of gol type `type`) has passed since the last call.
    void update(Scene& scene, Audio& audio, size_t index, int type);
    void clear() { last_.clear(); }

private:
    struct Event {
        float ms;
        std::vector<std::string> sounds;
    };
    using ClipEvents = std::unordered_map<std::string, std::vector<Event>>;   // by lowercase clip name
    const ClipEvents& events(int type);

    const Gol& gol_;
    std::unordered_map<int, ClipEvents> by_type_;
    struct Last {
        uint32_t serial = 0;
        float ms = 0;
    };
    std::unordered_map<size_t, Last> last_;
};

}  // namespace nova
