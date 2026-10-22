// Builds GPU materials from model materials: loads the effect, picks the technique,
// compiles its shaders and resolves every uniform to a material value, an effect
// default or an engine semantic.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "assets/bdae.h"
#include "assets/effect.h"
#include "core/vfs.h"
#include "render/material.h"
#include "shader/shader_compiler.h"

namespace nova {

class Renderer;

struct MaterialStats {
    size_t materials = 0, fallback = 0, programs = 0, failed_programs = 0, missing_effects = 0;
};

class MaterialSystem {
public:
    using TextureLookup = std::function<const GpuTexture*(const std::string& image_name)>;

    MaterialSystem(const Vfs& vfs, Renderer& renderer, TextureLookup textures, std::string shader_cache_dir);

    // The GPU material for `m` (stable pointer), or nullptr to draw it with the
    // built-in shader (missing effect or a shader that does not compile).
    const GpuMaterial* get(const Material& m, bool skinned);

    const MaterialStats& stats() const { return stats_; }
    const ShaderCompiler& compiler() const { return compiler_; }

private:
    const Effect* effect(const std::string& name);
    const GpuProgram* program(const EffectPass& pass, bool skinned);
    const std::string* source(const std::string& name);
    void fill_stage(const Effect& fx, const EffectPass& pass, const Material& m, const ShaderInterface& iface,
                    std::vector<uint8_t>& block, std::vector<SemanticSlot>& slots,
                    std::vector<const GpuTexture*>& textures, std::vector<GpuMaterial::ParamField>& fields,
                    bool fragment);

    const Vfs& vfs_;
    Renderer& renderer_;
    TextureLookup textures_;
    ShaderCompiler compiler_;
    std::unordered_map<std::string, std::unique_ptr<Effect>> effects_;     // lowercase name; null if missing
    std::unordered_map<std::string, std::unique_ptr<std::string>> sources_;
    std::unordered_map<std::string, const GpuProgram*> programs_;          // null if it failed
    std::unordered_map<const Material*, const GpuMaterial*> materials_[2]; // [skinned]
    std::deque<GpuMaterial> storage_;
    MaterialStats stats_;
};

}  // namespace nova
