// The particle effect library, levels/effects.bin (CEffectsManager / CPSEffect /
// spark::CEmitterInstance). See tools/effects.py for the layout.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "core/bytes.h"

namespace nova {

struct EffectEmitter {
    std::string name;
    std::string technique;            // DiffuseVC technique: "Additive", "Alpha", ...; "" for other effects
    std::string texture;              // DiffuseTexture
    enum class Shape { Point, Box, Sphere, Disc, Cylinder } shape = Shape::Point;
    glm::vec3 center{0};
    glm::vec3 extent{0};              // box size; sphere/disc/cylinder: radii and height in x/y/z
    bool global = false;              // particles stay where they were emitted
    glm::vec3 direction{0};           // initial velocity, metres per millisecond
    int speed_variation = 0;          // percent
    int rate_min = 0, rate_max = 0;   // particles per second
    int life_min = -1, life_max = -1; // emitter lifetime (ms), -1: forever
    int restart_min = -1, restart_max = -1;
    int start_delay = 0;              // ms
    glm::ivec3 spread{0};             // max angles XZ, YZ, XY (degrees)
    int billboard = 1;                // 1: faces the camera, 2: stretched along the velocity
    float width = 1, height = 1;
    int size_variation = 0;           // percent
    glm::u8vec4 color_min{255}, color_max{255};   // start colour (RGBA)
    int particle_life_min = 1000, particle_life_max = 1000;   // ms
    int rotation_min = 0, rotation_max = 0;                   // degrees
    bool uv_anim = false;
    int uv_cols = 1, uv_rows = 1;     // sprite sheet: cells the size of `uv`, stepping right then down
    int uv_period = 1000;             // ms per pass through the sheet
    glm::vec4 uv{0, 0, 1, 1};         // atlas rectangle u0, v0, u1, v1 (the first cell)
    // Curves over the particle's life (0..1); the first two keys hold the start value.
    std::vector<std::pair<float, glm::vec4>> color_keys;   // RGBA 0..255
    std::vector<std::pair<float, glm::vec2>> size_keys;
    glm::vec3 gravity{0};             // m/ms per second
    int spin_min = 0, spin_max = 0;   // degrees per second
};

struct ParticleEffect {
    std::string name;
    std::vector<EffectEmitter> emitters;
};

std::unordered_map<std::string, ParticleEffect> load_effects(const Bytes& data);

}  // namespace nova
