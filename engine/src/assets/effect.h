// Glitch effect files (effects/*.bdae): techniques, passes, render states and
// parameters. See tools/bdae_effect.py for the format.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/bytes.h"

namespace nova {

// Effect/material parameter types.
enum ParamType : uint32_t {
    kParamFloat = 4, kParamVec2 = 5, kParamVec3 = 6, kParamVec4 = 7, kParamMat4 = 0x0B,
    kParamSamplerFirst = 0x0C, kParamSamplerCube = 0x10, kParamSamplerLast = 0x10, kParamTechnique = 22,
};
inline bool is_sampler(uint32_t type) { return type >= kParamSamplerFirst && type <= kParamSamplerLast; }

// Blend factors, in the order of the driver's table (libNOVA3_neon.so 0xc79e30).
enum class BlendFactor : uint8_t {
    Zero, One, SrcColor, OneMinusSrcColor, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
    DstColor, OneMinusDstColor, ConstantColor, OneMinusConstantColor, ConstantAlpha, OneMinusConstantAlpha,
    SrcAlphaSaturate,
};

struct RenderState {
    bool blend = false, cull = true, depth_test = true, depth_write = true;
    BlendFactor src_color = BlendFactor::One, dst_color = BlendFactor::Zero;
    BlendFactor src_alpha = BlendFactor::One, dst_alpha = BlendFactor::Zero;

    // Packed form, for use as a cache key.
    uint32_t key() const;
};

struct ParamBinding {
    std::string uniform;
    uint16_t kind = 0;    // 0: effect parameter `index`; 1: engine global
    uint16_t index = 0;
};

struct EffectPass {
    std::string vertex_shader, fragment_shader;
    std::vector<std::string> vertex_defines, fragment_defines;   // "NAME" or "NAME VALUE"
    RenderState state;
    std::vector<ParamBinding> bindings;
};

struct Technique {
    std::string name;
    std::vector<EffectPass> passes;
};

struct EffectParam {
    std::string name;
    uint32_t type = 0, count = 1;
    std::vector<float> value;   // default (empty for samplers)
};

struct Effect {
    std::string id, name;
    std::vector<Technique> techniques;
    std::vector<EffectParam> params;

    // Technique by name; the first one if there is no such technique (as the game does).
    const Technique* technique(const std::string& name) const;
};

// Parse an effect file (ZIP-wrapped or a bare BRES image). Throws FormatError.
Effect load_effect(const Bytes& data);

}  // namespace nova
