#include "assets/effects.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "assets/dict_stream.h"

namespace nova {

namespace {

uint32_t le32(const Bytes& d, size_t p) {
    if (p + 4 > d.size()) throw std::runtime_error("effects.bin: truncated table");
    return uint32_t(d[p]) | uint32_t(d[p + 1]) << 8 | uint32_t(d[p + 2]) << 16 | uint32_t(d[p + 3]) << 24;
}

float as_float(int32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

glm::u8vec4 argb(int32_t v) {   // SColor: A R G B from the top byte down
    const uint32_t u = uint32_t(v);
    return {uint8_t(u >> 16), uint8_t(u >> 8), uint8_t(u), uint8_t(u >> 24)};
}

// spark::CEmitterInstance::Deserialize, stream version 0x20014.
EffectEmitter read_emitter(DictStream& st) {
    EffectEmitter e;
    e.name = st.s();
    st.s();                                   // effect file
    const std::string material = st.s();      // e.g. DiffuseVC-fx, Distortion-fx
    e.technique = st.s();
    if (material != "DiffuseVC-fx") e.technique.clear();
    const int nparams = st.i();
    st.i();                                   // mask
    for (int k = 0; k < nparams; ++k) {
        const std::string name = st.s();
        const int kind = st.i(), type = st.i();
        if (kind == 2) {
            const std::string tex = st.s();
            if (name == "DiffuseTexture") e.texture = tex;
        } else if (type == 5) {
            st.f();
        } else {
            st.i();
        }
    }
    const int shape = st.i();
    e.shape = EffectEmitter::Shape(shape >= 0 && shape <= 4 ? shape : 0);
    e.center = {st.f(), st.f(), st.f()};
    switch (shape) {
        case 1: e.extent = {st.f(), st.f(), st.f()}; break;
        case 2:
        case 3: e.extent = {st.f(), st.f(), 0}; break;
        case 4: e.extent = {st.f(), st.f(), st.f()}; break;
        default: break;
    }
    st.s();                                   // model (mesh particles)
    e.global = st.c() == 1;
    e.direction = {st.f(), st.f(), st.f()};
    e.speed_variation = st.i();
    e.rate_min = st.i();
    e.rate_max = st.i();
    e.life_min = st.i();
    e.life_max = st.i();
    e.restart_min = st.i();
    e.restart_max = st.i();
    e.start_delay = st.i();
    e.spread.x = st.i();                      // XZ
    e.spread.y = st.i();                      // YZ
    e.spread.z = st.i();                      // XY
    st.c();
    st.c();
    e.billboard = st.c();
    st.i();                                   // axis alignment
    e.width = st.f();
    e.height = st.f();
    st.f(), st.f(), st.f();                   // initial rotation XYZ
    e.size_variation = st.i();
    e.color_min = argb(st.i());
    e.color_max = argb(st.i());
    e.particle_life_min = st.i();
    e.particle_life_max = st.i();
    e.rotation_min = st.i();
    e.rotation_max = st.i();
    e.uv_anim = st.c() != 0;
    e.uv_cols = std::max<int>(uint8_t(st.c()), 1);
    e.uv_rows = std::max<int>(uint8_t(st.c()), 1);
    e.uv_period = std::max(st.i(), 1);
    const int nframes = st.i();
    for (int k = 0; k < nframes; ++k) {
        const glm::vec4 r{st.f(), st.f(), st.f(), st.f()};
        st.f(), st.f();
        if (k == 0) e.uv = r;
    }
    st.c();                                   // directional rotation
    st.i();
    st.s();
    const int nsounds = st.i();
    for (int k = 0; k < nsounds; ++k) {
        e.sounds.push_back(st.s());
        st.i(), st.i(), st.i(), st.c();
    }
    if (st.c() == 1) {
        const int n = st.i();
        for (int k = 0; k < n; ++k) {
            const float t = as_float(st.i());
            e.color_keys.push_back({t, {st.f(), st.f(), st.f(), st.f()}});
        }
    }
    if (st.c() == 1) {
        const int n = st.i();
        for (int k = 0; k < n; ++k) {
            const float t = as_float(st.i());
            e.size_keys.push_back({t, {st.f(), st.f()}});
        }
    }
    if (st.c() == 1) {
        const int n = st.i();
        for (int k = 0; k < n; ++k) st.i(), st.f(), st.f(), st.f();
    }
    const int nfields = st.i();
    for (int k = 0; k < nfields; ++k) {
        switch (st.i()) {
            case 1:   // attraction: point, strength, affect x/y/z, attract, start/end
                st.f(), st.f(), st.f(), st.f(), st.c(), st.c(), st.c(), st.c(), st.f(), st.f();
                break;
            case 2:   // gravity: vector, start/end percent
                e.gravity = {st.f(), st.f(), st.f()};
                st.i(), st.i();
                break;
            case 3:   // rotation: pivot, min/max speed, ..., start/end
                for (int f = 0; f < 9; ++f) st.f();
                st.i(), st.i();
                break;
            case 4:   // spin: min, max, start/end
                e.spin_min = st.i();
                e.spin_max = st.i();
                st.i(), st.i();
                break;
            default: throw std::runtime_error("effects.bin: unknown force field");
        }
    }
    for (int c = 0; c < 3; ++c) {   // influence curves
        const int n = st.i();
        for (int k = 0; k < n; ++k) st.i(), st.f();
    }
    return e;
}

}  // namespace

std::unordered_map<std::string, ParticleEffect> load_effects(const Bytes& data) {
    // Little-endian table: version, ?, count, then (name length, name, offset into the DICT).
    const uint32_t count = le32(data, 8);
    size_t p = 12;
    std::vector<std::pair<std::string, uint32_t>> table;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t n = le32(data, p);
        if (p + 8 + n > data.size()) throw std::runtime_error("effects.bin: truncated table");
        std::string name(reinterpret_cast<const char*>(data.data()) + p + 4, n);
        table.push_back({std::move(name), le32(data, p + 4 + n)});
        p += 8 + n;
    }
    size_t dict = p;
    while (dict + 4 <= data.size() && std::memcmp(data.data() + dict, "DICT", 4) != 0) ++dict;
    if (dict + 4 > data.size()) throw std::runtime_error("effects.bin: no DICT stream");
    DictStream st(Bytes(data.begin() + long(dict), data.end()));
    std::unordered_map<std::string, ParticleEffect> out;
    for (const auto& [name, offset] : table) {
        st.pos = offset;
        ParticleEffect fx;
        fx.name = st.s();
        const int n = st.i();
        for (int k = 0; k < n; ++k) fx.emitters.push_back(read_emitter(st));
        out[name] = std::move(fx);
    }
    return out;
}

}  // namespace nova
