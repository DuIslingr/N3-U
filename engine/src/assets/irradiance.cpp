#include "assets/irradiance.h"

#include <cmath>
#include <cstring>

namespace nova {

namespace {

constexpr size_t kPointSize = 0x84;
constexpr size_t kShFloats = 27;

// CIrradiancePoint::lerp: coefficients and intensity linearly, direction renormalised.
IrradiancePoint lerp(const IrradiancePoint& a, const IrradiancePoint& b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    IrradiancePoint out;
    for (size_t i = 0; i < out.size(); ++i) out[i] = a[i] + t * (b[i] - a[i]);
    glm::vec3 dir(out[27], out[28], out[29]);
    if (const float len = glm::length(dir); len > 0) dir /= len;
    out[27] = dir.x;
    out[28] = dir.y;
    out[29] = dir.z;
    return out;
}

// CustomIrradianceManager::InitDefaultLight colours (libNOVA3_neon.so 0xc6dcc0): sky above,
// ground below. Expressed directly as an SH that gives sky at +Z and ground at -Z.
IrradiancePoint default_light() {
    const glm::vec3 sky(0.6f, 0.6f, 0.65f), ground(0.4f, 0.4f, 0.35f);
    // E(n) = C0' * L0 + C1' * L1z * n.z with the constants getParameters applies; solve
    // for L0 and L1z so that E(+Z) = sky and E(-Z) = ground (the shader scales by 1.4).
    const float sqrt_pi = std::sqrt(3.14159265f);
    const float c0 = 0.5f / sqrt_pi, c1 = std::sqrt(3.0f) / (3 * sqrt_pi);
    IrradiancePoint p{};
    for (int ch = 0; ch < 3; ++ch) {
        const float mean = (sky[ch] + ground[ch]) * 0.5f / 1.4f, half = (sky[ch] - ground[ch]) * 0.5f / 1.4f;
        p[size_t(ch) * 9 + 0] = mean / c0;
        p[size_t(ch) * 9 + 2] = half / c1;
    }
    p[29] = 1;
    return p;
}

}  // namespace

float IrradianceVolume::distance(const glm::vec3& p) const {
    const glm::vec3 d = glm::max(glm::max(min - p, p - max), glm::vec3(0));
    return glm::length(d);
}

IrradiancePoint IrradianceVolume::sample(const glm::vec3& pos, int set) const {
    const auto& points = sets.at(size_t(set));
    const glm::vec3 g = (glm::clamp(pos, min, max) - min) / spacing;
    const glm::ivec3 i0 = glm::ivec3(g);
    const glm::vec3 f = g - glm::vec3(i0);
    auto at = [&](int x, int y, int z) -> const IrradiancePoint& {
        x = glm::clamp(x, 0, dims.x - 1);
        y = glm::clamp(y, 0, dims.y - 1);
        z = glm::clamp(z, 0, dims.z - 1);
        return points[size_t(x + dims.x * (y + dims.y * z))];
    };
    // Same order as CIrradianceVolume::getLinearPoint: along z, then y, then x.
    const IrradiancePoint x0y0 = lerp(at(i0.x, i0.y, i0.z), at(i0.x, i0.y, i0.z + 1), f.z);
    const IrradiancePoint x1y0 = lerp(at(i0.x + 1, i0.y, i0.z), at(i0.x + 1, i0.y, i0.z + 1), f.z);
    const IrradiancePoint x0y1 = lerp(at(i0.x, i0.y + 1, i0.z), at(i0.x, i0.y + 1, i0.z + 1), f.z);
    const IrradiancePoint x1y1 = lerp(at(i0.x + 1, i0.y + 1, i0.z), at(i0.x + 1, i0.y + 1, i0.z + 1), f.z);
    return lerp(lerp(x0y0, x0y1, f.y), lerp(x1y0, x1y1, f.y), f.x);
}

std::vector<IrradianceVolume> load_irradiance(const Bytes& data, const glm::vec3& offset) {
    if (data.size() < 12 || std::memcmp(data.data(), "MIhS", 4) != 0) throw FormatError("not an irradiance file");
    if (read_le<int32_t>(data, 4) != 3) return {};   // the game skips other versions (20 files are version 2)
    const int32_t count = read_le<int32_t>(data, 8);
    size_t o = 12;
    std::vector<IrradianceVolume> out;
    for (int32_t v = 0; v < count; ++v) {
        check_range(data.size(), o, 36);
        IrradianceVolume vol;
        float box[6];
        for (int k = 0; k < 6; ++k) box[k] = read_le<float>(data, o + 4 * size_t(k));
        vol.min = glm::vec3(box[0], box[1], box[2]) + offset;
        vol.max = glm::vec3(box[3], box[4], box[5]) + offset;
        vol.spacing = read_le<float>(data, o + 24);
        vol.fade = read_le<float>(data, o + 28);
        const int32_t sets = read_le<int32_t>(data, o + 32);
        o += 36;
        if (!(vol.spacing > 0) || sets < 0 || sets > 16) throw FormatError("bad irradiance volume");
        for (int k = 0; k < 3; ++k) vol.dims[k] = int((box[3 + k] - box[k]) / vol.spacing + 0.5f) + 1;
        const size_t n = size_t(vol.dims.x) * size_t(vol.dims.y) * size_t(vol.dims.z);
        for (int32_t s = 0; s < sets; ++s) {
            check_range(data.size(), o, n * kPointSize);
            auto& pts = vol.sets.emplace_back(n);
            for (size_t i = 0; i < n; ++i) std::memcpy(pts[i].data(), &data[o + i * kPointSize], kPointSize);
            o += n * kPointSize;
        }
        if (!vol.sets.empty()) out.push_back(std::move(vol));
    }
    return out;
}

IrradiancePoint sample_irradiance(const std::vector<IrradianceVolume>& volumes, const glm::vec3& p) {
    if (volumes.empty()) return default_light();
    const IrradianceVolume* nearest = nullptr;
    float best = 0;
    for (const IrradianceVolume& v : volumes) {
        const float d = v.distance(p);
        if (d <= 0) return v.sample(p);
        if (!nearest || d < best) nearest = &v, best = d;
    }
    const float t = nearest->fade > 0 ? std::min(best / nearest->fade, 1.0f) : 1.0f;
    IrradiancePoint black{};
    black[29] = 1;
    return lerp(nearest->sample(p), black, t);
}

IrradianceParams irradiance_parameters(const IrradiancePoint& in) {
    const float sqrt_pi = std::sqrt(3.14159265f);
    const float c0 = 0.5f / sqrt_pi;
    const float c1 = std::sqrt(3.0f) / (3 * sqrt_pi);
    const float c2 = std::sqrt(15.0f) / (8 * sqrt_pi);
    const float c3 = std::sqrt(5.0f) / (16 * sqrt_pi);
    const float c4 = 0.5f * c2;
    IrradianceParams out;
    for (int ch = 0; ch < 3; ++ch) {
        const float* l = &in[size_t(ch) * 9];
        out.a[ch] = {-c1 * l[3], -c1 * l[1], c1 * l[2], c0 * l[0] - c3 * l[6]};
        out.b[ch] = {c2 * l[4], -c2 * l[5], 3 * c3 * l[6], -c2 * l[7]};
        out.c[ch] = c4 * l[8];
    }
    out.peak_direction = {in[27], in[28], in[29], 0};
    out.peak_intensity = {in[30], in[31], in[32], 0};
    return out;
}

}  // namespace nova
