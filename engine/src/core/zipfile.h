// Members of plain (unscrambled) ZIP files, as nested inside some game files
// (.etc colour+alpha texture pairs, effect .bdae files).
#pragma once

#include <string>

#include "core/bytes.h"

namespace nova {

inline bool is_zip(const Bytes& data) {
    return data.size() >= 4 && std::memcmp(data.data(), "PK\x03\x04", 4) == 0;
}

// The named member's contents, or an empty buffer if absent.
Bytes unzip_member(const Bytes& zip, const std::string& name);

}  // namespace nova
