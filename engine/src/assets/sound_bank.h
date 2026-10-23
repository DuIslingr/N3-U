// The game's sound design table, sounds_hi/NOVA3_VOX_SDD.xml (Gameloft VOX): sounds
// (file, loop, 3D distances, bank, group), random events over sounds, groups (2D/3D,
// volume slider) and banks (how many play at once, which one is stolen).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace nova {

struct SoundDef {
    int uid = -1;
    std::string label, filename;   // filename: <label>.wav when the table names none
    std::string format;            // adpcm, pcm, vxn
    int bank = 0, group = 0, priority = 1;
    bool loop = false;
    float ref_distance = 0, max_distance = 0;   // 3D: full volume within ref, inverse distance beyond
};

struct SoundEvent {
    std::string label;
    std::vector<int> sounds;       // uids; one is picked at random
};

struct SoundGroup {
    std::string name, slider;      // slider: MUSIC, SFX, VFX
    bool positional = false;
};

struct SoundBankLimit {
    std::string name;
    int max_playbacks = 8;
    bool steal_lowest_priority = false;   // else steal the oldest
};

struct SoundTable {
    std::vector<SoundDef> sounds;                         // by uid
    std::vector<SoundEvent> events;
    std::unordered_map<int, SoundGroup> groups;           // by uid
    std::unordered_map<int, SoundBankLimit> banks;        // by uid

    // A sound by label (case-insensitive; a trailing .wav/.vxn is ignored) or file name.
    const SoundDef* sound(const std::string& name) const;
    const SoundEvent* event(const std::string& label) const;

    std::unordered_map<std::string, int> by_label_, by_file_, event_by_label_;
};

SoundTable parse_sound_table(const std::string& xml);

}  // namespace nova
