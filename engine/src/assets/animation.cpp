#include "assets/animation.h"

#include <algorithm>
#include <cmath>

#include "assets/bres.h"
#include "core/zipfile.h"

namespace nova {

namespace {

constexpr Coll kAnimations{0x24, 0x28, 0x20};
constexpr Coll kClips{0x38, 0x3C, 0x0C};

// SChannel types (CColladaDatabase::getAnimation / animation_track interpreters).
struct ChannelInfo {
    AnimTrack::Property property;
    int component;
    bool axis_angle;
};
bool channel_info(uint32_t type, ChannelInfo& out) {
    using P = AnimTrack::Property;
    switch (type) {
        case 1: out = {P::Translation, -1, false}; return true;
        case 2: out = {P::Translation, 0, false}; return true;
        case 3: out = {P::Translation, 1, false}; return true;
        case 4: out = {P::Translation, 2, false}; return true;
        case 5: out = {P::Rotation, -1, false}; return true;    // quaternion (x, y, z, w)
        case 9: out = {P::Rotation, -1, true}; return true;     // angle about the default axis
        case 10: out = {P::Scale, -1, false}; return true;
        case 11: out = {P::Scale, 0, false}; return true;
        case 12: out = {P::Scale, 1, false}; return true;
        case 13: out = {P::Scale, 2, false}; return true;
        default: return false;                                  // visibility, materials, morphs...
    }
}

struct Segment {
    std::vector<std::pair<uint32_t, uint32_t>> buffers;   // (file offset, size)
};

// Index of the key at or before t (clamped), and the blend factor to the next key.
inline void locate(const std::vector<float>& times, float t, size_t& i, float& f) {
    if (t <= times.front()) {
        i = 0;
        f = 0;
        return;
    }
    if (t >= times.back()) {
        i = times.size() - 1;
        f = 0;
        return;
    }
    i = size_t(std::upper_bound(times.begin(), times.end(), t) - times.begin()) - 1;
    const float span = times[i + 1] - times[i];
    f = span > 0 ? (t - times[i]) / span : 0;
}

float interp(const AnimTrack& tr, float t, int c) {
    size_t i;
    float f;
    locate(tr.times, t, i, f);
    const float a = tr.values[i * tr.stride + c];
    if (f == 0) return a;
    return a + (tr.values[(i + 1) * tr.stride + c] - a) * f;
}

}  // namespace

const AnimClip* AnimationLibrary::clip(const std::string& name) const {
    const std::string want = to_lower(name);
    for (const auto& c : clips) {
        if (to_lower(c.name) == want) return &c;
    }
    return nullptr;
}

AnimationLibrary load_animation(const Bytes& raw) {
    Bytes inner;
    const Bytes* data = &raw;
    if (is_zip(raw)) {
        inner = unzip_member(raw, "little_endian_not_quantized.bdae");
        data = &inner;
    }
    const Bres b(*data);
    AnimationLibrary lib;
    for (uint32_t o : b.array(b.root, kClips)) lib.clips.push_back({b.cstr(o), float(b.i32(o + 4)), float(b.i32(o + 8))});

    // Segments: each holds an SAnimationData buffer table.
    std::vector<Segment> segments;
    if (const uint32_t table = b.ptr(b.root + 0x30)) {
        const int32_t n = b.i32(table);
        const uint32_t base = b.ptr(table + 4);
        for (int32_t k = 0; k < n && base; ++k) {
            const uint32_t s = base + 0x1C * uint32_t(k);
            if (b.i32(s + 12) != 0) throw FormatError("animation segment with relocations");
            const uint32_t off = b.u32(s + 16);
            Segment seg;
            const uint32_t count = b.u32(off);
            for (uint32_t j = 0; j < count; ++j) {
                const uint32_t field = off + 8 + 8 * j;
                seg.buffers.emplace_back(uint32_t(int64_t(field) + b.i32(field)), b.u32(off + 4 + 8 * j));
            }
            segments.push_back(std::move(seg));
        }
    }

    // Keys of a sampler across all segments: times (ms) and `comps` values per key,
    // floats (value type 6) or bytes scaled to 0..1 (type 1).
    auto read_keys = [&](const uint32_t s[8], std::vector<float>& times, std::vector<float>& values) {
        size_t tsize;
        float tscale;
        switch (s[1]) {
            case 1: tsize = 1; tscale = 1000.0f / 30.0f; break;
            case 3: tsize = 2; tscale = 1000.0f / 30.0f; break;
            case 4: tsize = 4; tscale = 1.0f; break;
            default: return;
        }
        if (s[4] != 6 && s[4] != 1) return;
        const uint32_t vstride = s[7] >> 16, voff = s[7] & 0xFFFF, comps = s[5];
        for (const Segment& seg : segments) {
            if (std::max(s[3], s[6]) >= seg.buffers.size()) continue;
            const auto [tb, tlen] = seg.buffers[s[3]];
            const uint32_t vb = seg.buffers[s[6]].first;
            for (size_t k = 0; k < tlen / tsize; ++k) {
                const size_t at = tb + k * tsize;
                float t;
                if (tsize == 1) t = float(read_le<uint8_t>(*data, at)) * tscale;
                else if (tsize == 2) t = float(read_le<uint16_t>(*data, at)) * tscale;
                else t = float(read_le<int32_t>(*data, at)) * tscale;
                if (!times.empty() && t <= times.back()) continue;   // segments repeat their boundary key
                times.push_back(t);
                for (uint32_t c = 0; c < comps; ++c) {
                    const size_t v = vb + voff + k * vstride;
                    values.push_back(s[4] == 6 ? read_le<float>(*data, v + 4 * c) : float(read_le<uint8_t>(*data, v + c)) / 255.0f);
                }
            }
        }
    };

    bool any = false;
    auto extend_timeline = [&](const std::vector<float>& times) {
        if (!any || times.front() < lib.timeline_start) lib.timeline_start = times.front();
        if (!any || times.back() > lib.timeline_end) lib.timeline_end = times.back();
        any = true;
    };
    for (uint32_t a : b.array(b.root, kAnimations)) {
        const uint32_t channel = b.ptr(a + 0x10), sampler = b.ptr(a + 0x08);
        if (!channel || !sampler) continue;
        uint32_t s[8];
        for (int k = 0; k < 8; ++k) s[k] = b.u32(sampler + 4 * k);
        const uint32_t ctype = b.u32(channel + 4);
        if (ctype == 114 || ctype == 19) {
            ValueTrack vt;
            vt.light = ctype == 19;
            vt.target = b.cstr(channel);
            if (!vt.light) {
                vt.param = b.cstr(channel + 8);
                const uint32_t mask = b.u32(channel + 0xC);
                while (vt.first < 3 && !(mask & (1u << vt.first))) ++vt.first;
            }
            vt.stride = int(s[5]);
            read_keys(s, vt.times, vt.values);
            if (vt.times.empty() || vt.stride <= 0) continue;
            extend_timeline(vt.times);
            lib.values.push_back(std::move(vt));
            continue;
        }
        ChannelInfo info;
        if (!channel_info(ctype, info)) continue;
        if (s[4] != 6) continue;   // transforms are always floats
        AnimTrack tr;
        tr.property = info.property;
        tr.component = info.component;
        tr.axis_angle = info.axis_angle;
        tr.stride = int(s[5]);
        read_keys(s, tr.times, tr.values);
        if (tr.times.empty()) continue;
        if (const uint32_t dp = b.ptr(a + 0x18); dp && b.ptr(dp + 8)) {
            const uint32_t v = b.ptr(dp + 8);
            tr.defaults = {b.f32(v), b.f32(v + 4), b.f32(v + 8)};
            tr.has_defaults = true;
        }
        if (tr.property == AnimTrack::Property::Rotation && !tr.axis_angle && tr.stride == 4) {
            // Normalize and keep neighbouring keys in the same hemisphere so lerping takes the short way.
            for (size_t k = 0; k < tr.times.size(); ++k) {
                float* q = &tr.values[k * 4];
                const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                for (int c = 0; c < 4 && len > 0; ++c) q[c] /= len;
                if (k > 0) {
                    const float* p = q - 4;
                    if (p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3] < 0)
                        for (int c = 0; c < 4; ++c) q[c] = -q[c];
                }
            }
        }
        extend_timeline(tr.times);
        lib.tracks[b.cstr(channel)].push_back(std::move(tr));
    }
    return lib;
}

void sample_tracks(const std::vector<AnimTrack>& tracks, float t, NodePose& pose) {
    for (const AnimTrack& tr : tracks) {
        switch (tr.property) {
            case AnimTrack::Property::Rotation:
                if (tr.axis_angle) {
                    const glm::vec3 axis = glm::length(tr.defaults) > 0 ? glm::normalize(tr.defaults) : glm::vec3(0, 0, 1);
                    pose.rotation = glm::angleAxis(interp(tr, t, 0), axis);
                } else {
                    size_t i;
                    float f;
                    locate(tr.times, t, i, f);
                    const float* a = &tr.values[i * 4];
                    glm::quat q(a[3], a[0], a[1], a[2]);
                    if (f > 0) {
                        const float* b = &tr.values[(i + 1) * 4];
                        q = glm::normalize(q * (1 - f) + glm::quat(b[3], b[0], b[1], b[2]) * f);
                    }
                    pose.rotation = q;
                }
                break;
            case AnimTrack::Property::Translation:
            case AnimTrack::Property::Scale: {
                glm::vec3& v = tr.property == AnimTrack::Property::Translation ? pose.translation : pose.scale;
                if (tr.component < 0) {
                    for (int c = 0; c < 3; ++c) v[c] = interp(tr, t, c);
                } else {
                    for (int c = 0; c < 3 && tr.has_defaults; ++c) {
                        if (c != tr.component) v[c] = tr.defaults[c];
                    }
                    v[tr.component] = interp(tr, t, 0);
                }
                break;
            }
        }
    }
}

void sample_value(const ValueTrack& track, float t, float* out) {
    size_t i;
    float f;
    locate(track.times, t, i, f);
    for (int c = 0; c < track.stride; ++c) {
        const float a = track.values[i * size_t(track.stride) + size_t(c)];
        out[c] = f == 0 ? a : a + (track.values[(i + 1) * size_t(track.stride) + size_t(c)] - a) * f;
    }
}

}  // namespace nova
