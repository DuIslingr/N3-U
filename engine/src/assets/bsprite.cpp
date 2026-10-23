#include "assets/bsprite.h"

#include <cstring>

#include "core/bytes.h"

namespace nova {

Sprite load_sprite(const Bytes& d) {
    size_t p = 0;
    auto need = [&](size_t n) {
        if (p + n > d.size()) throw FormatError("bsprite: truncated");
    };
    auto u8 = [&] { need(1); return int(d[p++]); };
    auto u16 = [&] { need(2); const int v = d[p] | d[p + 1] << 8; p += 2; return v; };
    auto s16 = [&] { return int(int16_t(uint16_t(u16()))); };
    if (u16() != 0xA9D2) throw FormatError("bsprite: bad magic");
    u16();   // flags
    const int ni = u16(), nm = u16(), nf = u16(), nfr = u16();
    u16(), u16(), u16();   // anim frames, anims, extra (not needed for still frames)
    Sprite s;
    std::vector<int> lengths;
    for (int i = 0; i < ni; ++i) lengths.push_back(u8());   // all name lengths, then the names
    for (int n : lengths) {
        need(size_t(n));
        s.images.emplace_back(reinterpret_cast<const char*>(d.data()) + p, size_t(n));
        p += size_t(n);
    }
    // Per-module columns: image, flags, a u32, x, y, w, h.
    s.modules.resize(size_t(nm));
    for (auto& m : s.modules) m.image = u8();
    for (int i = 0; i < nm; ++i) u8();
    need(size_t(nm) * 4), p += size_t(nm) * 4;
    for (auto& m : s.modules) m.x = s16();
    for (auto& m : s.modules) m.y = s16();
    for (auto& m : s.modules) m.w = u16();
    for (auto& m : s.modules) m.h = u16();
    // Frame modules: module, flags, x, y.
    s.fmodules.resize(size_t(nf));
    for (auto& f : s.fmodules) f.module = u16();
    for (auto& f : s.fmodules) f.flags = u8();
    for (auto& f : s.fmodules) f.x = s16();
    for (auto& f : s.fmodules) f.y = s16();
    // Frames: count, first.
    s.frames.resize(size_t(nfr));
    for (auto& f : s.frames) f.count = u8();
    for (auto& f : s.frames) f.first = u16();
    return s;
}

}  // namespace nova
