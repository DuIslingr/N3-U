// Materials drawn with the game's own shaders: a compiled effect pass (program),
// its render state, and the uniform values and textures one material gives it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "assets/effect.h"
#include "shader/glsl_translate.h"

namespace nova {

struct GpuTexture;

// A vertex/fragment shader pair on the GPU, with the interfaces the translator reported.
struct GpuProgram {
    SDL_GPUShader* vertex = nullptr;
    SDL_GPUShader* fragment = nullptr;
    ShaderInterface vertex_iface, fragment_iface;
    bool skinned = false;
};

// Values the engine supplies (Glitch "semantic" parameters, matched by name).
enum class Semantic : uint8_t {
    WorldViewProjection, World, WorldInverse, WorldInverseTransposed, WorldView, WorldViewTransposed,
    WorldViewInverseTransposed, WorldViewInverse, View, ViewInverse, ViewTransposed, Projection, ViewProjection,
    CameraPositionObject, CameraPositionWorld, CameraDirectionWorld, Time,
    FogColor, FogStartEnd, FogDensity, SceneAmbient,
    LightPositionObject, LightPositionWorld, LightAttenuation, LightDiffuse, LightAmbient, LightSpecular,
    IrradianceA, IrradianceB, IrradianceC, IrradiancePeakDirection, IrradiancePeakIntensity,   // `light`: channel
    Zero,
};

// Recognise a uniform name as an engine semantic; `light` gets the light index (or the
// colour channel of an irradiance coefficient).
bool semantic_for(const std::string& uniform, Semantic& semantic, int& light);

struct SemanticSlot {
    uint32_t offset;
    std::string type;
    uint32_t array;
    Semantic semantic;
    int light;
};

struct GpuMaterial {
    const GpuProgram* program = nullptr;
    RenderState state;
    std::vector<uint8_t> vertex_uniforms, fragment_uniforms;   // material values filled in
    std::vector<SemanticSlot> vertex_semantics, fragment_semantics;
    std::vector<const GpuTexture*> vertex_textures, fragment_textures;   // per sampler binding
    std::string name;                                          // effect/technique, for diagnostics
};

// Write `n` floats into a std140 field of `type` (columns of matrices padded to 16 bytes).
void write_uniform(std::vector<uint8_t>& block, uint32_t offset, const std::string& type, const float* v, size_t n);

}  // namespace nova
