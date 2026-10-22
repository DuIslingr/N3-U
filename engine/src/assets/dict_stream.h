// Big-endian CMemoryStream "DICT" files (.lvc levels, gol.bin, .col collision) and the
// read-spec interpreter used for the game's generated component loaders.
//
// Layout: "DICT", u32 string-table offset, u8 wide-strings flag; data from byte 9.
// String table: u32 count, then (u32 length, bytes). In the data a string is a u32
// index into the table (-1 = empty).
#pragma once

#include <string>
#include <vector>

#include "core/bytes.h"

namespace nova {

// A value read by a spec: an int, float, string, or a list of values.
struct Value {
    enum Type : uint8_t { Int, Float, String, List } type = List;
    int32_t i = 0;
    float f = 0;
    std::string s;
    std::vector<Value> list;

    static Value make_int(int32_t v) { Value x; x.type = Int; x.i = v; return x; }
    static Value make_float(float v) { Value x; x.type = Float; x.f = v; return x; }
    static Value make_string(std::string v) { Value x; x.type = String; x.s = std::move(v); return x; }

    size_t size() const { return list.size(); }
    const Value& operator[](size_t k) const { return list.at(k); }
    float as_float() const { return type == Float ? f : float(i); }
};

class DictStream {
public:
    explicit DictStream(Bytes data);

    int32_t i();
    float f();
    int16_t h();
    uint16_t H();
    int8_t c();
    const std::string& s();
    void skip(size_t n);

    // Execute a spec: i f h H c s v (vec3), [ ... ] for an int-count-prefixed group.
    // Returns the values read as a List.
    Value spec(const std::string& spec);

    size_t pos = 9;
    size_t end = 0;                        // start of the string table
    const std::vector<std::string>& strings() const { return strings_; }

private:
    Value run(const std::vector<std::string>& tokens, size_t& k);
    Bytes data_;
    std::vector<std::string> strings_;
    std::string empty_;
};

}  // namespace nova
