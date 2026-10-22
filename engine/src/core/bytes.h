// Endian-aware reads from byte buffers.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace nova {

using Bytes = std::vector<uint8_t>;

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void check_range(size_t size, size_t off, size_t n) {
    if (off > size || n > size - off) {
        throw FormatError("read past end of data at offset " + std::to_string(off));
    }
}

template <typename T>
inline T load_le(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <typename T>
inline T load_be(const uint8_t* p) {
    uint8_t tmp[sizeof(T)];
    for (size_t i = 0; i < sizeof(T); ++i) tmp[i] = p[sizeof(T) - 1 - i];
    T v;
    std::memcpy(&v, tmp, sizeof(T));
    return v;
}

template <typename T>
inline T read_le(const Bytes& b, size_t off) {
    check_range(b.size(), off, sizeof(T));
    return load_le<T>(b.data() + off);
}

template <typename T>
inline T read_be(const Bytes& b, size_t off) {
    check_range(b.size(), off, sizeof(T));
    return load_be<T>(b.data() + off);
}

inline std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

}  // namespace nova
