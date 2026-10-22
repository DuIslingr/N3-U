// Irradiance volumes (<level>_<object id>.irr): grids of spherical-harmonic light
// probes that light characters and props (the ird_* uniforms of the game's shaders).
//
// File: "MIhS", u32 version 3, u32 volume count, then per volume: AABB (6 floats,
// relative to the owning object), f32 grid spacing, f32 fade distance, i32 set
// count, then per set nx*ny*nz points of 0x84 bytes (index x + nx * (y + ny * z)).
// A point: 27 floats of order-2 SH (9 coefficients for R, then G, then B), a peak
// direction (3 floats) and a peak intensity (3 floats).
// Sampling follows glitch::irradiance::CIrradianceVolume / CustomIrradianceManager.
#pragma once

#include <array>
#include <vector>

#include <glm/glm.hpp>

#include "core/bytes.h"

namespace nova {

using IrradiancePoint = std::array<float, 33>;

struct IrradianceVolume {
    glm::vec3 min{0}, max{0};
    float spacing = 1, fade = 0;
    glm::ivec3 dims{1};
    std::vector<std::vector<IrradiancePoint>> sets;

    // Trilinear sample (positions outside are clamped to the grid).
    IrradiancePoint sample(const glm::vec3& p, int set = 0) const;
    float distance(const glm::vec3& p) const;   // 0 inside
};

// The shader-side form (CIrradiancePoint::getParameters): polynomial coefficients
// for ird_cAr/cAg/cAb/cBr/cBg/cBb/cC, plus the peak direction and intensity.
struct IrradianceParams {
    glm::vec4 a[3]{};      // cAr, cAg, cAb
    glm::vec4 b[3]{};      // cBr, cBg, cBb
    glm::vec4 c{0};        // cC (rgb)
    glm::vec4 peak_direction{0}, peak_intensity{0};
};

// Volumes of a file, offset by the owning object's position; empty for versions other than 3.
std::vector<IrradianceVolume> load_irradiance(const Bytes& data, const glm::vec3& offset);

// Light probe at `p`: inside a volume, its trilinear sample; otherwise the nearest
// volume's, fading to black over its fade distance; with no volumes, the game's
// default sky/ground light.
IrradiancePoint sample_irradiance(const std::vector<IrradianceVolume>& volumes, const glm::vec3& p);
IrradianceParams irradiance_parameters(const IrradiancePoint& point);

}  // namespace nova
