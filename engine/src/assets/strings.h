// Localised text (strings/<table>_<LANG>.bin/.off with <table>_map.bin): string ids
// (STR_...) to text. Tables: MAIN, the level's (level properties), NPC barks.
#pragma once

#include <string>
#include <unordered_map>

#include "core/bytes.h"

namespace nova {

class Vfs;

class StringTable {
public:
    // Add a table; false if its files are missing.
    bool load(const Vfs& vfs, const std::string& table, const std::string& lang = "EN");
    // Text for an id as scripts use them ("VO_KAL_TH1_001" or "STR_..."); empty if none.
    std::string find(const std::string& id) const;
    size_t size() const { return text_.size(); }

private:
    std::unordered_map<std::string, std::string> text_;
};

}  // namespace nova
