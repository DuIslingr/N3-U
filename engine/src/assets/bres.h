// Random access to Glitch "BRES" resource images (.bdae): a memory image whose
// pointers are stored as file offsets, listed in a relocation table.
#pragma once

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/bytes.h"

namespace nova {

// A (count, pointer) collection inside a struct, with its element stride.
struct Coll {
    uint32_t count, ptr, stride;
};

class Bres {
public:
    explicit Bres(const Bytes& d) : data(d) {
        if (data.size() < 0x3C || std::memcmp(data.data(), "BRES", 4) != 0) throw FormatError("not a BRES file");
        const uint32_t n = u32(0x10), table = u32(0x18);
        for (uint32_t i = 0; i < n; ++i) relocs.insert(u32(table + 4 * i));
        root = u32(0x20);
    }
    uint32_t u32(size_t off) const { return read_le<uint32_t>(data, off); }
    int32_t i32(size_t off) const { return read_le<int32_t>(data, off); }
    float f32(size_t off) const { return read_le<float>(data, off); }
    uint32_t ptr(size_t off) const {
        const uint32_t v = u32(off);
        return v && relocs.count(uint32_t(off)) ? v : 0;
    }
    std::string cstr(size_t off) const {
        const uint32_t p = ptr(off);
        if (!p) return {};
        check_range(data.size(), p, 0);
        const auto* begin = reinterpret_cast<const char*>(data.data() + p);
        return std::string(begin, strnlen(begin, data.size() - p));
    }
    std::vector<uint32_t> array(uint32_t base, Coll c) const {
        const int32_t n = i32(base + c.count);
        const uint32_t p = ptr(base + c.ptr);
        std::vector<uint32_t> out;
        if (n <= 0 || !p) return out;
        for (int32_t i = 0; i < n; ++i) out.push_back(p + uint32_t(i) * c.stride);
        return out;
    }

    const Bytes& data;
    std::unordered_set<uint32_t> relocs;
    uint32_t root = 0;
};

}  // namespace nova
