#include "assets/strings.h"

#include "core/vfs.h"

namespace nova {

namespace {

uint16_t u16(const Bytes& d, size_t p) { return p + 2 <= d.size() ? uint16_t(d[p] | d[p + 1] << 8) : 0; }

}  // namespace

bool StringTable::load(const Vfs& vfs, const std::string& table, const std::string& lang) {
    const std::string map = table + "_map.bin", off = table + "_" + lang + ".off", bin = table + "_" + lang + ".bin";
    if (!vfs.exists(map) || !vfs.exists(off) || !vfs.exists(bin)) return false;
    // _map.bin: u16 count, then (u16 length, id with its NUL); .off: u16 count, then the
    // end offset of each string in .bin (NUL-terminated UTF-8).
    const Bytes m = vfs.read(map), o = vfs.read(off), b = vfs.read(bin);
    const size_t n = std::min<size_t>(u16(m, 0), u16(o, 0));
    size_t p = 2, start = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t len = u16(m, p);
        if (p + 2 + len > m.size()) break;
        std::string id(reinterpret_cast<const char*>(m.data()) + p + 2, len);
        while (!id.empty() && id.back() == '\0') id.pop_back();
        p += 2 + len;
        const size_t end = std::min<size_t>(u16(o, 2 + 2 * i), b.size());
        std::string text(reinterpret_cast<const char*>(b.data()) + start, end > start ? end - start : 0);
        while (!text.empty() && text.back() == '\0') text.pop_back();
        start = end;
        text_[id] = std::move(text);
    }
    return true;
}

std::string StringTable::find(const std::string& id) const {
    if (auto it = text_.find(id); it != text_.end()) return it->second;
    if (auto it = text_.find("STR_" + id); it != text_.end()) return it->second;
    return {};
}

}  // namespace nova
