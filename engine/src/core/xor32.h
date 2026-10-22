// The game's DECODE_XOR32: an LCG-keyed rotate+XOR used by custom pak archives.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nova {

constexpr uint32_t kPakSeed = 0x3857A;

// Decode `n` bytes in place. The trailing 1-3 bytes use the same LCG via getRand.
void decode_xor32(uint8_t* data, size_t n, uint32_t seed = kPakSeed);

}  // namespace nova
