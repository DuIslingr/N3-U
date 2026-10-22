#include "assets/dict_stream.h"

#include <sstream>
#include <unordered_map>

namespace nova {

DictStream::DictStream(Bytes data) : data_(std::move(data)) {
    if (data_.size() < 9 || std::memcmp(data_.data(), "DICT", 4) != 0) throw FormatError("not a DICT stream");
    end = read_be<uint32_t>(data_, 4);
    if (data_[8] != 0) throw FormatError("wide-string streams are not supported");
    const uint32_t n = read_be<uint32_t>(data_, end);
    size_t p = end + 4;
    strings_.reserve(n);
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t len = read_be<uint32_t>(data_, p);
        check_range(data_.size(), p + 4, len);
        strings_.emplace_back(reinterpret_cast<const char*>(&data_[p + 4]), len);
        p += 4 + len;
    }
}

int32_t DictStream::i() {
    check_range(end, pos, 4);
    const auto v = load_be<int32_t>(&data_[pos]);
    pos += 4;
    return v;
}

float DictStream::f() {
    check_range(end, pos, 4);
    const auto v = load_be<float>(&data_[pos]);
    pos += 4;
    return v;
}

int16_t DictStream::h() {
    check_range(end, pos, 2);
    const auto v = load_be<int16_t>(&data_[pos]);
    pos += 2;
    return v;
}

uint16_t DictStream::H() {
    check_range(end, pos, 2);
    const auto v = load_be<uint16_t>(&data_[pos]);
    pos += 2;
    return v;
}

int8_t DictStream::c() {
    check_range(end, pos, 1);
    return int8_t(data_[pos++]);
}

const std::string& DictStream::s() {
    const int32_t idx = i();
    if (idx == -1) return empty_;     // GetStringC returns an empty string for missing indices
    if (idx < 0 || size_t(idx) >= strings_.size()) {
        throw FormatError("string index " + std::to_string(idx) + " out of range at " + std::to_string(pos - 4));
    }
    return strings_[idx];
}

void DictStream::skip(size_t n) {
    check_range(end, pos, n);
    pos += n;
}

Value DictStream::spec(const std::string& spec) {
    // Specs are reused for thousands of objects: tokenize once.
    static std::unordered_map<std::string, std::vector<std::string>> cache;
    auto it = cache.find(spec);
    if (it == cache.end()) {
        std::string spaced;
        for (char ch : spec) {
            if (ch == '[' || ch == ']') (spaced += ' ', spaced += ch, spaced += ' ');
            else spaced += ch;
        }
        std::istringstream in(spaced);
        std::vector<std::string> tokens;
        for (std::string t; in >> t;) tokens.push_back(t);
        it = cache.emplace(spec, std::move(tokens)).first;
    }
    size_t k = 0;
    return run(it->second, k);
}

Value DictStream::run(const std::vector<std::string>& tokens, size_t& k) {
    Value out;
    while (k < tokens.size()) {
        const std::string& t = tokens[k];
        if (t == "]") return out;
        if (t == "[") {
            const int32_t n = i();
            if (n < 0 || n > 100000) throw FormatError("implausible count " + std::to_string(n) + " at " + std::to_string(pos - 4));
            Value group;
            const size_t body = k + 1;
            size_t after = body;
            if (n == 0) {
                // Skip to the matching bracket without reading.
                int depth = 0;
                while (!(tokens[after] == "]" && depth == 0)) {
                    depth += tokens[after] == "[" ? 1 : tokens[after] == "]" ? -1 : 0;
                    ++after;
                }
            }
            for (int32_t r = 0; r < n; ++r) {
                size_t kk = body;
                group.list.push_back(run(tokens, kk));
                after = kk;
            }
            out.list.push_back(std::move(group));
            k = after + 1;
            continue;
        }
        switch (t[0]) {
            case 'i': out.list.push_back(Value::make_int(i())); break;
            case 'f': out.list.push_back(Value::make_float(f())); break;
            case 'h': out.list.push_back(Value::make_int(h())); break;
            case 'H': out.list.push_back(Value::make_int(H())); break;
            case 'c': out.list.push_back(Value::make_int(c())); break;
            case 's': out.list.push_back(Value::make_string(s())); break;
            case 'v': {
                Value v3;
                for (int a = 0; a < 3; ++a) v3.list.push_back(Value::make_float(f()));
                out.list.push_back(std::move(v3));
                break;
            }
            default: throw FormatError("bad spec token '" + t + "'");
        }
        ++k;
    }
    return out;
}

}  // namespace nova
