#include "render/material_system.h"

#include <algorithm>
#include <cstdio>

#include "render/renderer.h"

namespace nova {

MaterialSystem::MaterialSystem(const Vfs& vfs, Renderer& renderer, TextureLookup textures, std::string shader_cache_dir)
    : vfs_(vfs), renderer_(renderer), textures_(std::move(textures)), compiler_(std::move(shader_cache_dir)) {}

const Effect* MaterialSystem::effect(const std::string& name) {
    const std::string key = to_lower(name);
    if (auto it = effects_.find(key); it != effects_.end()) return it->second.get();
    std::unique_ptr<Effect> fx;
    if (const auto& hits = vfs_.find(key); !hits.empty()) {
        try {
            fx = std::make_unique<Effect>(load_effect(vfs_.read(hits.front())));
        } catch (const FormatError& ex) {
            std::fprintf(stderr, "effect %s: %s\n", name.c_str(), ex.what());
        }
    }
    if (!fx) ++stats_.missing_effects;
    return (effects_[key] = std::move(fx)).get();
}

const std::string* MaterialSystem::source(const std::string& name) {
    const std::string key = to_lower(name);
    if (auto it = sources_.find(key); it != sources_.end()) return it->second.get();
    std::unique_ptr<std::string> text;
    if (const auto& hits = vfs_.find(key); !hits.empty()) {
        const Bytes b = vfs_.read(hits.front());
        text = std::make_unique<std::string>(b.begin(), b.end());
    }
    return (sources_[key] = std::move(text)).get();
}

const GpuProgram* MaterialSystem::program(const EffectPass& pass, bool skinned) {
    std::string key = pass.vertex_shader + "|" + pass.fragment_shader + (skinned ? "|S" : "|");
    for (const auto& d : pass.vertex_defines) key += "|v" + d;
    for (const auto& d : pass.fragment_defines) key += "|f" + d;
    if (auto it = programs_.find(key); it != programs_.end()) return it->second;
    const GpuProgram* result = nullptr;
    const std::string* vs = source(pass.vertex_shader);
    const std::string* fs = source(pass.fragment_shader);
    if (vs && fs) {
        try {
            result = renderer_.create_program(compiler_.compile(*vs, pass.vertex_defines, *fs, pass.fragment_defines, skinned));
            ++stats_.programs;
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "shader %s / %s: %s\n", pass.vertex_shader.c_str(), pass.fragment_shader.c_str(), ex.what());
        }
    }
    if (!result) ++stats_.failed_programs;
    return programs_[key] = result;
}

void MaterialSystem::fill_stage(const Effect& fx, const EffectPass& pass, const Material& m, const ShaderInterface& iface,
                                std::vector<uint8_t>& block, std::vector<SemanticSlot>& slots,
                                std::vector<const GpuTexture*>& textures, std::vector<GpuMaterial::ParamField>& fields,
                                bool fragment) {
    auto material_value = [&](const std::string& name) -> const MaterialParam* {
        for (const MaterialParam& p : m.params)
            if (p.name == name) return &p;
        return nullptr;
    };
    // The effect parameter a uniform is bound to, if any.
    auto bound_param = [&](const std::string& uniform) -> const EffectParam* {
        for (const ParamBinding& b : pass.bindings) {
            if (b.uniform == uniform && b.kind == 0 && b.index < fx.params.size()) return &fx.params[b.index];
        }
        return nullptr;
    };
    auto write = [&](const UniformField& u, const std::vector<float>& v) {
        const size_t count = std::max<uint32_t>(u.array, 1);
        const size_t per = v.size() / count;
        for (size_t i = 0; i < count && per; ++i)
            write_uniform(block, u.offset + uint32_t(i) * u.stride, u.type, v.data() + i * per, per);
    };

    block.assign(iface.uniform_size, 0);
    for (const UniformField& u : iface.uniforms) {
        Semantic sem;
        int light = 0;
        const bool is_semantic = semantic_for(u.name, sem, light);
        if (is_semantic && sem == Semantic::Time) {   // animated even when bound to a parameter
            slots.push_back({u.offset, u.type, u.array, sem, light});
            continue;
        }
        const EffectParam* ep = bound_param(u.name);
        Semantic over;
        if (object_override_for(ep ? ep->name : u.name, over)) {
            slots.push_back({u.offset, u.type, u.array, over, 0});   // per-object value, when there is one
        }
        fields.push_back({ep ? ep->name : u.name, fragment, u});
        if (ep) {
            const MaterialParam* mp = material_value(ep->name);
            write(u, mp && !mp->value.empty() ? mp->value : ep->value);
            continue;
        }
        if (u.name == "L0_PosW") {   // set by game code; equals L0_Pos for level geometry (World = identity)
            if (const MaterialParam* mp = material_value("L0_Pos")) write(u, mp->value);
            continue;
        }
        if (is_semantic) {
            slots.push_back({u.offset, u.type, u.array, sem, light});
            continue;
        }
        if (const MaterialParam* mp = material_value(u.name); mp && !mp->value.empty()) write(u, mp->value);
    }

    textures.clear();
    for (size_t i = 0; i < iface.samplers.size(); ++i) {
        const bool cube = iface.sampler_types[i] == "samplerCube";
        const GpuTexture* tex = nullptr;
        if (!cube) {
            if (const EffectParam* ep = bound_param(iface.samplers[i])) {
                if (const MaterialParam* mp = material_value(ep->name); mp && !mp->texture.empty()) tex = textures_(mp->texture);
            }
        }
        textures.push_back(tex ? tex : cube ? renderer_.white_cube() : renderer_.white());
    }
}

const GpuMaterial* MaterialSystem::get(const Material& m, bool skinned) {
    auto& cache = materials_[skinned ? 1 : 0];
    if (auto it = cache.find(&m); it != cache.end()) return it->second;
    const GpuMaterial* result = nullptr;
    const Effect* fx = m.effect.empty() ? nullptr : effect(m.effect);
    const Technique* tech = fx ? fx->technique(m.technique) : nullptr;
    // Screen-reading techniques come in one per device orientation (NV_Distortion's Opaque,
    // Opaque_O90, ...); _O180 reads the screen the right way up for an unrotated view.
    if (tech && !tech->passes.empty()) {
        const auto& defs = tech->passes.front().fragment_defines;
        if (std::find(defs.begin(), defs.end(), "O0") != defs.end())
            if (const Technique* upright = fx->technique(m.technique + "_O180")) tech = upright;
    }
    if (tech && !tech->passes.empty()) {
        const EffectPass& pass = tech->passes.front();
        if (const GpuProgram* prog = program(pass, skinned)) {
            GpuMaterial& gm = storage_.emplace_back();
            gm.program = prog;
            gm.state = pass.state;
            gm.name = fx->id + "/" + tech->name;
            fill_stage(*fx, pass, m, prog->vertex_iface, gm.vertex_uniforms, gm.vertex_semantics, gm.vertex_textures,
                       gm.param_fields, false);
            fill_stage(*fx, pass, m, prog->fragment_iface, gm.fragment_uniforms, gm.fragment_semantics,
                       gm.fragment_textures, gm.param_fields, true);
            for (size_t k = 0; k < prog->fragment_iface.samplers.size(); ++k)
                if (prog->fragment_iface.samplers[k] == "ScreenTextureSampler") gm.screen_slot = int(k);
            result = &gm;
            ++stats_.materials;
        }
    }
    if (!result) ++stats_.fallback;
    return cache[&m] = result;
}

}  // namespace nova
