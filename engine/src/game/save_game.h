// The save file (Application::Save / Load): where the campaign stands, kept between runs in
// the preferences folder. A checkpoint writes the level and its checkpoint; a level's end
// writes the next level with the arsenal carried into it. Continue picks it up.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/combat.h"

namespace nova {

struct SaveGame {
    std::string level;                 // "levels/<file>.lvc"
    std::vector<uint8_t> checkpoint;   // LevelLogic::checkpoint_data(); empty: from the level's start
    bool has_arsenal = false;          // without a checkpoint: what the player carries in
    Combat::Saved arsenal;

    static std::string default_path();   // empty if there is no preferences folder
    bool write(const std::string& path) const;
    static bool read(const std::string& path, SaveGame& out);
    static void remove(const std::string& path);
};

// Combat::Saved as bytes and back (also inside a checkpoint).
void write_combat(std::vector<uint8_t>& out, const Combat::Saved& s);
bool read_combat(const std::vector<uint8_t>& in, size_t& at, Combat::Saved& s);

}  // namespace nova
