#include "assets/effect.h"

#include <algorithm>

#include "assets/bres.h"
#include "core/zipfile.h"

namespace nova {

namespace {

constexpr Coll kEffects{0x70, 0x74, 0x104};
constexpr Coll kTechniques{0x38, 0x3C, 0x0C};
constexpr Coll kParams{0x40, 0x44, 0x18};
constexpr uint32_t kPassSize = 0x74, kBindingSize = 0x10;

// Render state words at pass +0x1C: word 0 holds the blend factors (src/dst colour,
// src/dst alpha nibbles), word 3 the enables.
RenderState decode_state(const Bres& b, uint32_t p) {
    const uint32_t factors = b.u32(p + 0x1C), enables = b.u32(p + 0x28);
    RenderState s;
    auto factor = [&](int shift) {
        const uint32_t f = (factors >> shift) & 0xF;
        return f <= uint32_t(BlendFactor::SrcAlphaSaturate) ? BlendFactor(f) : BlendFactor::One;
    };
    s.src_color = factor(0);
    s.dst_color = factor(4);
    s.src_alpha = factor(8);
    s.dst_alpha = factor(12);
    s.blend = enables & 0x00080000;
    s.cull = enables & 0x00100000;
    s.depth_test = enables & 0x00400000;
    s.depth_write = enables & 0x00800000;
    return s;
}

std::vector<std::string> defines(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("#define ", 0) == 0) out.push_back(line.substr(8));
        pos = end + 1;
    }
    return out;
}

size_t components(uint32_t type) {
    switch (type) {
        case kParamFloat: return 1;
        case kParamVec2: return 2;
        case kParamVec3: return 3;
        case kParamVec4: return 4;
        case kParamMat4: return 16;
        default: return 0;
    }
}

}  // namespace

uint32_t RenderState::key() const {
    return uint32_t(blend) | uint32_t(cull) << 1 | uint32_t(depth_test) << 2 | uint32_t(depth_write) << 3 |
           uint32_t(src_color) << 8 | uint32_t(dst_color) << 12 | uint32_t(src_alpha) << 16 | uint32_t(dst_alpha) << 20;
}

const Technique* Effect::technique(const std::string& name) const {
    for (const Technique& t : techniques)
        if (t.name == name) return &t;
    return techniques.empty() ? nullptr : &techniques.front();
}

Effect load_effect(const Bytes& data) {
    Bytes inner;
    if (is_zip(data)) {
        for (const char* name : {"little_endian_not_quantized.bdae", "little_endian_quantized.bdae"}) {
            inner = unzip_member(data, name);
            if (!inner.empty()) break;
        }
        if (inner.empty()) throw FormatError("ZIP-wrapped effect without a little-endian build");
    }
    const Bres b(inner.empty() ? data : inner);
    const auto effects = b.array(b.root, kEffects);
    if (effects.empty()) throw FormatError("no effect in file");
    const uint32_t e = effects.front();

    Effect fx;
    fx.id = b.cstr(e);
    fx.name = b.cstr(e + 4);
    for (uint32_t p : b.array(e, kParams)) {
        EffectParam param;
        param.name = b.cstr(p);
        param.type = b.u32(p + 4);
        param.count = std::max<uint32_t>(b.u32(p + 0xC), 1);
        const size_t n = components(param.type) * param.count;
        const uint32_t value = b.ptr(p + 0x14);
        if (n && value && value + 4 * n <= b.data.size()) {
            for (size_t k = 0; k < n; ++k) param.value.push_back(b.f32(value + 4 * k));
        }
        fx.params.push_back(std::move(param));
    }
    for (uint32_t t : b.array(e, kTechniques)) {
        Technique tech;
        tech.name = b.cstr(t);
        uint32_t p = b.ptr(t + 8);
        for (uint32_t i = 0, n = b.u32(t + 4); i < n && p; ++i) {
            EffectPass pass;
            pass.vertex_shader = b.cstr(p + 4);
            pass.vertex_defines = defines(b.cstr(p + 0xC));
            pass.fragment_shader = b.cstr(p + 0x10);
            pass.fragment_defines = defines(b.cstr(p + 0x18));
            pass.state = decode_state(b, p);
            const uint32_t nb = b.u32(p + 0x6C);
            for (uint32_t k = 0; k < nb; ++k) {
                const uint32_t o = p + kPassSize + k * kBindingSize;
                pass.bindings.push_back({b.cstr(o + 4), uint16_t(b.u32(o + 8) & 0xFFFF), uint16_t(b.u32(o + 0xC) & 0xFFFF)});
            }
            tech.passes.push_back(std::move(pass));
            p += kPassSize + nb * kBindingSize;   // the next pass follows the bindings
        }
        fx.techniques.push_back(std::move(tech));
    }
    return fx;
}

}  // namespace nova
