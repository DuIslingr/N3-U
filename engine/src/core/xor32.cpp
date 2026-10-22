#include "core/xor32.h"

namespace nova {

namespace {
constexpr uint32_t kA = 0x19660D;
constexpr uint32_t kC = 0x3C6EF35F;

inline uint32_t step(uint32_t s) { return s * kA + kC; }
inline uint32_t rotr(uint32_t v, unsigned r) { return r ? (v >> r) | (v << (32 - r)) : v; }
}  // namespace

void decode_xor32(uint8_t* data, size_t n, uint32_t seed) {
    uint32_t s = seed;
    const size_t n4 = n / 4 * 4;
    for (size_t i = 0; i < n4; i += 4) {
        const uint32_t s1 = step(s), s2 = step(s1);
        unsigned rot = (s2 & 0x1FFFFF) >> 16;
        if (!(s1 & 0x10000)) rot = 32 - rot;
        rot &= 31;
        const uint32_t s3 = step(s2), s4 = step(s3), s5 = step(s4);
        s = step(s5);
        uint32_t w = uint32_t(data[i]) | uint32_t(data[i + 1]) << 8 | uint32_t(data[i + 2]) << 16 |
                     uint32_t(data[i + 3]) << 24;
        w = rotr(w, rot);
        const uint32_t k = ((s3 >> 16) & 0xFF) << 24 | (s4 & 0xFF0000) | ((s5 >> 16) & 0xFF) << 8 | ((s >> 16) & 0xFF);
        w ^= k;
        data[i] = uint8_t(w);
        data[i + 1] = uint8_t(w >> 8);
        data[i + 2] = uint8_t(w >> 16);
        data[i + 3] = uint8_t(w >> 24);
    }
    const size_t rest = n - n4;
    if (!rest) return;
    auto rand = [&](uint32_t m) {
        s = step(s);
        return (s >> 16) % m;
    };
    const unsigned bits = unsigned(8 * rest);
    const uint32_t mask = (1u << bits) - 1;
    const bool left = rand(2) == 0;
    const unsigned r = rand(bits);
    uint32_t v = 0;
    for (size_t i = 0; i < rest; ++i) v |= uint32_t(data[n4 + i]) << (8 * i);
    // Shifts by the full width are no-ops on the game's 32-bit maths; guard them here.
    auto shl = [](uint32_t x, unsigned k) { return k >= 32 ? 0u : x << k; };
    auto shr = [](uint32_t x, unsigned k) { return k >= 32 ? 0u : x >> k; };
    v = left ? (shr(v, bits - r) | shl(v, r)) & mask : (shl(v, bits - r) | shr(v, r)) & mask;
    uint32_t k = 0;
    for (int sh = int(bits) - 8; sh >= 0; sh -= 8) k |= rand(256) << sh;   // key bytes high to low
    v ^= k;
    for (size_t i = 0; i < rest; ++i) data[n4 + i] = uint8_t(v >> (8 * i));
}

}  // namespace nova
