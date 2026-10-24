#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifdef NOVA3_SHADERCROSS
#include <SDL3_shadercross/SDL_shadercross.h>
#endif
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace nova {

namespace {

constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
constexpr SDL_GPUTextureFormat kOffscreenFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

struct DrawUniforms {
    glm::mat4 view_proj;
    glm::mat4 model;
};

struct MaterialUniforms {
    glm::vec4 flags;
    glm::vec4 sun_dir;
    glm::vec4 sun_color;
    glm::vec4 ambient;
    glm::vec4 fog;
    glm::vec4 camera;
};

void fail(const std::string& what) { throw std::runtime_error(what + ": " + SDL_GetError()); }

SDL_GPUBlendFactor blend_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero: return SDL_GPU_BLENDFACTOR_ZERO;
        case BlendFactor::One: return SDL_GPU_BLENDFACTOR_ONE;
        case BlendFactor::SrcColor: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SrcAlpha: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::DstColor: return SDL_GPU_BLENDFACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::ConstantColor:
        case BlendFactor::ConstantAlpha: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
        case BlendFactor::OneMinusConstantColor:
        case BlendFactor::OneMinusConstantAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::SrcAlphaSaturate: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    }
    return SDL_GPU_BLENDFACTOR_ONE;
}

// Every vertex attribute, at the locations the shader translator assigns.
constexpr SDL_GPUVertexAttribute kAllAttributes[] = {
    {kLocPosition, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, position))},
    {kLocNormal, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, normal))},
    {kLocUv0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv0))},
    {kLocUv1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv1))},
    {kLocColor, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(Vertex, color))},
    {kLocJoints, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, uint32_t(offsetof(Vertex, joints))},
    {kLocWeights, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(Vertex, weights))},
    {kLocUv2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv2))},
    {kLocTangent, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, tangent))},
    {kLocBinormal, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, binormal))},
};

// Per-draw values for engine semantics.
struct DrawContext {
    const FrameParams& frame;
    glm::mat4 world{1};
    glm::mat4 view_proj{1};
    const ObjectLighting* lighting = nullptr;
    glm::vec3 center{0};                        // draw bounds centre (world)
    glm::vec2 viewport{1};                      // the target's size, pixels
    int lights[2] = {-2, -2};                   // Light0/Light1: indices into frame.lights, -1 none, -2 not chosen yet

    // The game's driver lights for an object: the nearest non-fake lights in range.
    const PointLight* light(int n) {
        if (lights[0] == -2) {
            lights[0] = lights[1] = -1;
            float best[2] = {0, 0};
            for (size_t i = 0; i < frame.lights.size(); ++i) {
                const PointLight& l = frame.lights[i];
                if (l.fake || glm::vec3(l.diffuse) == glm::vec3(0)) continue;
                const float d = glm::distance(l.position, center);
                if (d > l.radius) continue;
                if (lights[0] < 0 || d < best[0]) {
                    lights[1] = lights[0], best[1] = best[0];
                    lights[0] = int(i), best[0] = d;
                } else if (lights[1] < 0 || d < best[1]) {
                    lights[1] = int(i), best[1] = d;
                }
            }
        }
        return n < 2 && lights[n] >= 0 ? &frame.lights[size_t(lights[n])] : nullptr;
    }
    bool have_inverse = false;
    glm::mat4 inverse{1};

    const IrradianceParams& probe() const {
        static const IrradianceParams kDefault = irradiance_parameters(sample_irradiance({}, glm::vec3(0)));
        return lighting ? lighting->probe : kDefault;
    }

    const glm::mat4& world_inverse() {
        if (!have_inverse) inverse = glm::inverse(world), have_inverse = true;
        return inverse;
    }

    void write(std::vector<uint8_t>& block, const SemanticSlot& s) {
        float v[16] = {};
        size_t n = 16;
        auto mat = [&](const glm::mat4& m) { std::memcpy(v, glm::value_ptr(m), sizeof v); };
        auto vec = [&](const glm::vec4& x) {
            std::memcpy(v, glm::value_ptr(x), sizeof x);
            n = 4;
        };
        const PointLight* light = nullptr;
        if (s.semantic >= Semantic::LightPositionObject && s.semantic <= Semantic::LightSpecular) light = this->light(s.light);
        switch (s.semantic) {
            case Semantic::WorldViewProjection: mat(view_proj * world); break;
            case Semantic::World: mat(world); break;
            case Semantic::WorldInverse: mat(world_inverse()); break;
            case Semantic::WorldInverseTransposed: mat(glm::transpose(world_inverse())); break;
            case Semantic::WorldView: mat(frame.view * world); break;
            case Semantic::WorldViewTransposed: mat(glm::transpose(frame.view * world)); break;
            case Semantic::WorldViewInverseTransposed: mat(glm::transpose(glm::inverse(frame.view * world))); break;
            case Semantic::WorldViewInverse: mat(glm::inverse(frame.view * world)); break;
            case Semantic::View: mat(frame.view); break;
            case Semantic::ViewInverse: mat(glm::inverse(frame.view)); break;
            case Semantic::ViewTransposed: mat(glm::transpose(frame.view)); break;
            case Semantic::Projection: mat(frame.proj); break;
            case Semantic::ViewProjection: mat(view_proj); break;
            case Semantic::CameraPositionObject: vec(world_inverse() * glm::vec4(frame.camera, 1)); break;
            case Semantic::CameraPositionWorld: vec(glm::vec4(frame.camera, 1)); break;
            case Semantic::CameraDirectionWorld: vec(-glm::inverse(frame.view)[2]); break;
            case Semantic::Time: vec(glm::vec4(frame.time, 0, 0, 0)); break;
            case Semantic::ViewportSizeInverse: vec(glm::vec4(1.0f / glm::max(viewport, glm::vec2(1)), 0, 0)); break;
            case Semantic::FogColor: vec(frame.fog_color); break;
            case Semantic::FogStartEnd: {
                const float range = std::max(frame.fog_end - frame.fog_start, 1e-3f);
                vec(glm::vec4(frame.fog_start, frame.fog_end, 1.0f / range, 0));
                break;
            }
            case Semantic::FogDensity: vec(glm::vec4(frame.fog_density, 0, 0, 0)); break;
            case Semantic::SceneAmbient: vec(glm::vec4(frame.ambient, 1)); break;
            // No light: far away and black, so the shaders' divisions stay finite.
            case Semantic::LightPositionObject:
                vec(world_inverse() * glm::vec4(light ? light->position : glm::vec3(0, 0, 1e5f), 1));
                break;
            case Semantic::LightPositionWorld: vec(glm::vec4(light ? light->position : glm::vec3(0, 0, 1e5f), 1)); break;
            case Semantic::LightAttenuation: vec(glm::vec4(light ? light->attenuation : glm::vec3(1, 0, 0), 0)); break;
            case Semantic::LightDiffuse: vec(light ? light->diffuse : glm::vec4(0)); break;
            case Semantic::LightAmbient: vec(light ? light->ambient : glm::vec4(0)); break;
            case Semantic::LightSpecular: vec(light ? light->specular : glm::vec4(0)); break;
            case Semantic::IrradianceA: vec(probe().a[s.light]); break;
            case Semantic::IrradianceB: vec(probe().b[s.light]); break;
            case Semantic::IrradianceC: vec(probe().c); break;
            case Semantic::IrradiancePeakDirection: vec(probe().peak_direction); break;
            case Semantic::IrradiancePeakIntensity: vec(probe().peak_intensity); break;
            case Semantic::Zero: vec(glm::vec4(0)); break;
            // Per-object overrides: leave the material's value when there is nothing to say.
            case Semantic::SpecDirection:
                if (!lighting) return;
                vec(world_inverse() * glm::vec4(glm::vec3(lighting->probe.peak_direction), 0));
                break;
            case Semantic::SpecColor:
                if (!lighting) return;
                vec(glm::vec4(glm::vec3(lighting->probe.peak_intensity) * 3.0f, 1));
                break;
            case Semantic::RimFactor: {
                if (!lighting) return;
                // CNovaSceneManager::s_RimDistance*: the rim fades in with camera distance; its
                // range moves out with the probe's specular brightness.
                const glm::vec3 spec = glm::vec3(lighting->probe.peak_intensity) * 3.0f;
                const float lum = glm::dot(spec, glm::vec3(0.2126f, 0.7152f, 0.0722f));
                float lo = 3.6835f, hi = 30.5273f;
                if (spec != glm::vec3(0)) lo = 0.0f + lum * (lo - 0.0f), hi = 20.5273f + lum * (hi - 20.5273f);
                const float d = glm::distance(lighting->center, frame.camera);
                const float k = d > hi ? 1.0f : (std::max(d, lo) - lo) / std::max(hi - lo, 1e-3f);
                vec(glm::vec4(k, k, k, 0));
                break;
            }
            case Semantic::L0PositionObject:
                if (!lighting || !lighting->has_light) return;
                vec(world_inverse() * glm::vec4(lighting->l0_position, 1));
                break;
            case Semantic::L0PositionWorld:
                if (!lighting || !lighting->has_light) return;
                vec(glm::vec4(lighting->l0_position, 1));
                break;
            case Semantic::L0Color:
                if (!lighting || !lighting->has_light) return;
                vec(lighting->l0_color);
                break;
            case Semantic::L0Attenuation:
                if (!lighting || !lighting->has_light) return;
                vec(glm::vec4(lighting->l0_attenuation, 0));
                break;
            case Semantic::L0Scale:
                if (!lighting || !lighting->has_light) return;
                vec(glm::vec4(lighting->l0_scale, 0));
                break;
        }
        write_uniform(block, s.offset, s.type, v, n);
    }
};

std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open shader " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

}  // namespace

Renderer::Renderer(SDL_Window* window, const std::string& shader_dir) : window_(window) {
    // Vulkan takes the SPIR-V as it is; Direct3D 12 and Metal get it through SDL_shadercross.
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef NOVA3_SHADERCROSS
    if (SDL_ShaderCross_Init()) formats |= SDL_ShaderCross_GetSPIRVShaderFormats();
#endif
    device_ = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!device_) fail("SDL_CreateGPUDevice");
    spirv_ = (SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_SPIRV) != 0;
    std::printf("gpu: %s\n", SDL_GetGPUDeviceDriver(device_));
    if (window_ && !SDL_ClaimWindowForGPUDevice(device_, window_)) fail("SDL_ClaimWindowForGPUDevice");
    if (window_) SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    vs_ = load_shader(shader_dir + "/mesh.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    vs_skinned_ = load_shader(shader_dir + "/mesh_skinned.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
    fs_ = load_shader(shader_dir + "/mesh.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    vs_color_ = load_shader(shader_dir + "/color.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    fs_color_ = load_shader(shader_dir + "/color.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    vs_post_ = load_shader(shader_dir + "/post.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    vs_particle_ = load_shader(shader_dir + "/particle.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    fs_particle_ = load_shader(shader_dir + "/particle.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    fs_post_blur_ = load_shader(shader_dir + "/post_blur.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    fs_post_final_ = load_shader(shader_dir + "/post_final.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 5, 1);

    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.max_lod = 1000.0f;
    si.max_anisotropy = 8.0f;
    si.enable_anisotropy = true;
    sampler_ = SDL_CreateGPUSampler(device_, &si);
    if (!sampler_) fail("SDL_CreateGPUSampler");
    SDL_GPUSamplerCreateInfo csi{};   // post effects: screen images, no wrapping or mips
    csi.min_filter = csi.mag_filter = SDL_GPU_FILTER_LINEAR;
    csi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    csi.address_mode_u = csi.address_mode_v = csi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    clamp_sampler_ = SDL_CreateGPUSampler(device_, &csi);
    if (!clamp_sampler_) fail("SDL_CreateGPUSampler (clamp)");

    Image white{1, 1, {255, 255, 255, 255}};
    white_ = *upload_texture(white);

    // 1x1 white cube map for samplerCube uniforms without a texture.
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_CUBE;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = ci.height = 1;
    ci.layer_count_or_depth = 6;
    ci.num_levels = 1;
    white_cube_.texture = SDL_CreateGPUTexture(device_, &ci);
    if (!white_cube_.texture) fail("cube texture");
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbi);
    std::memset(SDL_MapGPUTransferBuffer(device_, tb, false), 255, 4);
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (uint32_t face = 0; face < 6; ++face) {
        SDL_GPUTextureTransferInfo src{tb, 0, 1, 1};
        SDL_GPUTextureRegion dst{};
        dst.texture = white_cube_.texture;
        dst.layer = face;
        dst.w = dst.h = dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
}

Renderer::~Renderer() {
    if (!device_) return;
    SDL_WaitForGPUIdle(device_);
    for (auto& m : models_) {
        SDL_ReleaseGPUBuffer(device_, m->vertices);
        if (m->owns_indices) SDL_ReleaseGPUBuffer(device_, m->indices);
    }
    for (auto& t : textures_) SDL_ReleaseGPUTexture(device_, t->texture);
    SDL_ReleaseGPUTexture(device_, white_cube_.texture);
    for (auto& [key, p] : material_pipelines_) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    for (auto& p : programs_) {
        SDL_ReleaseGPUShader(device_, p->vertex);
        SDL_ReleaseGPUShader(device_, p->fragment);
    }
    for (auto& [fmt, p] : pipelines_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, p.opaque);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.blend);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.opaque_skinned);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.blend_skinned);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.effects);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.particles_additive);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.particles_alpha);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.sprites);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.sprites_additive);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.hud);
    }
    if (overlay_buffer_) SDL_ReleaseGPUBuffer(device_, overlay_buffer_);
    if (overlay_transfer_) SDL_ReleaseGPUTransferBuffer(device_, overlay_transfer_);
    SDL_ReleaseGPUShader(device_, vs_color_);
    SDL_ReleaseGPUShader(device_, fs_color_);
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    SDL_ReleaseGPUSampler(device_, sampler_);
    SDL_ReleaseGPUSampler(device_, clamp_sampler_);
    for (auto& [fmt, p] : post_pipelines_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, p.blur);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.final);
    }
    for (SDL_GPUTexture* t : {scene_color_, screen_copy_, blur_a_, blur_b_, grading_})
        if (t) SDL_ReleaseGPUTexture(device_, t);
    if (grading_transfer_) SDL_ReleaseGPUTransferBuffer(device_, grading_transfer_);
    SDL_ReleaseGPUShader(device_, vs_post_);
    SDL_ReleaseGPUShader(device_, vs_particle_);
    SDL_ReleaseGPUShader(device_, fs_particle_);
    for (DynamicBuffer* b : {&particle_buffer_, &sprite_buffer_}) {
        if (b->buffer) SDL_ReleaseGPUBuffer(device_, b->buffer);
        if (b->transfer) SDL_ReleaseGPUTransferBuffer(device_, b->transfer);
    }
    SDL_ReleaseGPUShader(device_, fs_post_blur_);
    SDL_ReleaseGPUShader(device_, fs_post_final_);
    SDL_ReleaseGPUShader(device_, vs_);
    SDL_ReleaseGPUShader(device_, vs_skinned_);
    SDL_ReleaseGPUShader(device_, fs_);
    if (window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
#ifdef NOVA3_SHADERCROSS
    SDL_ShaderCross_Quit();
#endif
}

SDL_GPUShader* Renderer::make_shader(const uint8_t* code, size_t size, SDL_GPUShaderStage stage, uint32_t samplers,
                                    uint32_t uniforms) {
    if (spirv_) {
        SDL_GPUShaderCreateInfo ci{};
        ci.code_size = size;
        ci.code = code;
        ci.entrypoint = "main";
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.stage = stage;
        ci.num_samplers = samplers;
        ci.num_uniform_buffers = uniforms;
        return SDL_CreateGPUShader(device_, &ci);
    }
#ifdef NOVA3_SHADERCROSS
    SDL_ShaderCross_SPIRV_Info info{};
    info.bytecode = code;
    info.bytecode_size = size;
    info.entrypoint = "main";
    info.shader_stage = stage == SDL_GPU_SHADERSTAGE_VERTEX ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
    SDL_ShaderCross_GraphicsShaderResourceInfo resources{};
    resources.num_samplers = samplers;
    resources.num_uniform_buffers = uniforms;
    return SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(device_, &info, &resources, 0);
#else
    SDL_SetError("this build only has Vulkan shaders");
    return nullptr;
#endif
}

SDL_GPUShader* Renderer::load_shader(const std::string& path, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniforms) {
    const auto code = read_binary(path);
    SDL_GPUShader* s = make_shader(reinterpret_cast<const uint8_t*>(code.data()), code.size(), stage, samplers, uniforms);
    if (!s) fail("SDL_CreateGPUShader " + path);
    return s;
}

SDL_GPUShader* Renderer::create_shader(const std::vector<uint32_t>& spirv, SDL_GPUShaderStage stage, uint32_t samplers,
                                      uint32_t uniforms) {
    return make_shader(reinterpret_cast<const uint8_t*>(spirv.data()), spirv.size() * 4, stage, samplers, uniforms);
}

const GpuProgram* Renderer::create_program(const CompiledProgram& c) {
    auto p = std::make_unique<GpuProgram>();
    p->vertex_iface = c.vertex;
    p->fragment_iface = c.fragment;
    p->skinned = c.skinned;
    p->vertex = create_shader(c.vertex_spirv, SDL_GPU_SHADERSTAGE_VERTEX, uint32_t(c.vertex.samplers.size()),
                              c.skinned ? 2 : 1);
    p->fragment = create_shader(c.fragment_spirv, SDL_GPU_SHADERSTAGE_FRAGMENT, uint32_t(c.fragment.samplers.size()), 1);
    if (!p->vertex || !p->fragment) {
        if (p->vertex) SDL_ReleaseGPUShader(device_, p->vertex);
        if (p->fragment) SDL_ReleaseGPUShader(device_, p->fragment);
        throw std::runtime_error(std::string("SDL_CreateGPUShader: ") + SDL_GetError());
    }
    programs_.push_back(std::move(p));
    return programs_.back().get();
}

SDL_GPUGraphicsPipeline* Renderer::material_pipeline(const GpuMaterial& m, SDL_GPUTextureFormat format) {
    char key[64];
    std::snprintf(key, sizeof key, "%p/%08x/%d", static_cast<const void*>(m.program), m.state.key(), int(format));
    if (auto it = material_pipelines_.find(key); it != material_pipelines_.end()) return it->second;

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = sizeof(Vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUColorTargetDescription ct{};
    ct.format = format;
    if (m.state.blend) {
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = blend_factor(m.state.src_color);
        ct.blend_state.dst_color_blendfactor = blend_factor(m.state.dst_color);
        ct.blend_state.src_alpha_blendfactor = blend_factor(m.state.src_alpha);
        ct.blend_state.dst_alpha_blendfactor = blend_factor(m.state.dst_alpha);
        ct.blend_state.color_blend_op = ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = m.program->vertex;
    pi.fragment_shader = m.program->fragment;
    pi.vertex_input_state.vertex_buffer_descriptions = &vb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = kAllAttributes;
    pi.vertex_input_state.num_vertex_attributes = uint32_t(std::size(kAllAttributes));
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = m.state.cull ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;   // as in GL
    pi.depth_stencil_state.enable_depth_test = m.state.depth_test;
    pi.depth_stencil_state.enable_depth_write = m.state.depth_write;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = kDepthFormat;
    pi.target_info.has_depth_stencil_target = true;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &pi);
    if (!p) fail("SDL_CreateGPUGraphicsPipeline (" + m.name + ")");
    return material_pipelines_[key] = p;
}

const Renderer::Pipelines& Renderer::pipelines_for(SDL_GPUTextureFormat format) {
    auto it = pipelines_.find(int(format));
    if (it != pipelines_.end()) return it->second;

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = sizeof(Vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[7] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, position))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, normal))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv0))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv1))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(Vertex, color))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, uint32_t(offsetof(Vertex, joints))},
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(Vertex, weights))},
    };

    auto make = [&](bool blend, bool skinned) {
        SDL_GPUColorTargetDescription ct{};
        ct.format = format;
        if (blend) {
            ct.blend_state.enable_blend = true;
            ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = skinned ? vs_skinned_ : vs_;
        pi.fragment_shader = fs_;
        pi.vertex_input_state.vertex_buffer_descriptions = &vb;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = skinned ? 7 : 5;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;   // TODO: match the game's winding and cull
        pi.depth_stencil_state.enable_depth_test = true;
        pi.depth_stencil_state.enable_depth_write = !blend;
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pi.target_info.depth_stencil_format = kDepthFormat;
        pi.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &pi);
        if (!p) fail("SDL_CreateGPUGraphicsPipeline");
        return p;
    };
    // Overlay: position + colour; effects add light (depth-tested, no depth writes),
    // the HUD is alpha-blended over everything.
    SDL_GPUVertexBufferDescription cvb{};
    cvb.slot = 0;
    cvb.pitch = sizeof(ColorVertex);
    cvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute cattrs[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(ColorVertex, position))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(ColorVertex, rgba))},
    };
    auto make_color = [&](bool additive) {
        SDL_GPUColorTargetDescription ct{};
        ct.format = format;
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = additive ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs_color_;
        pi.fragment_shader = fs_color_;
        pi.vertex_input_state.vertex_buffer_descriptions = &cvb;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = cattrs;
        pi.vertex_input_state.num_vertex_attributes = 2;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.depth_stencil_state.enable_depth_test = additive;
        pi.depth_stencil_state.enable_depth_write = false;
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pi.target_info.depth_stencil_format = kDepthFormat;
        pi.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &pi);
        if (!p) fail("SDL_CreateGPUGraphicsPipeline (overlay)");
        return p;
    };
    SDL_GPUVertexBufferDescription pvb{};
    pvb.slot = 0;
    pvb.pitch = sizeof(ParticleVertex);
    pvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute pattrs[3] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(ParticleVertex, position))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(ParticleVertex, uv))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(ParticleVertex, rgba))},
    };
    auto make_particles = [&](bool additive, bool depth) {   // blend(src_alpha, one) or (src_alpha, 1 - src_alpha)
        SDL_GPUColorTargetDescription ct{};
        ct.format = format;
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = additive ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs_particle_;
        pi.fragment_shader = fs_particle_;
        pi.vertex_input_state.vertex_buffer_descriptions = &pvb;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = pattrs;
        pi.vertex_input_state.num_vertex_attributes = 3;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.depth_stencil_state.enable_depth_test = depth;
        pi.depth_stencil_state.enable_depth_write = false;
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pi.target_info.depth_stencil_format = kDepthFormat;
        pi.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &pi);
        if (!p) fail("SDL_CreateGPUGraphicsPipeline (particles)");
        return p;
    };
    return pipelines_[int(format)] = Pipelines{make(false, false), make(true, false), make(false, true), make(true, true),
                                               make_particles(true, true), make_particles(false, true), make_color(true), make_color(false),
                                               make_particles(false, false), make_particles(true, false)};
}

void Renderer::ensure_depth(uint32_t w, uint32_t h) {
    if (depth_ && depth_w_ == w && depth_h_ == h) return;
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kDepthFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;   // post effects read it
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    depth_ = SDL_CreateGPUTexture(device_, &ti);
    if (!depth_) fail("depth texture");
    depth_w_ = w;
    depth_h_ = h;
}

SDL_GPUBuffer* Renderer::upload_buffer(SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size) {
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = usage;
    bi.size = size;
    SDL_GPUBuffer* buf = SDL_CreateGPUBuffer(device_, &bi);
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = size;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!buf || !tb) fail("buffer upload");
    std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), data, size);
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{tb, 0};
    SDL_GPUBufferRegion dst{buf, 0, size};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    return buf;
}

GpuModel* Renderer::upload_model(const Model& model) {
    auto gm = std::make_unique<GpuModel>();
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    for (const Mesh& mesh : model.meshes) {
        auto& ranges = gm->meshes.emplace_back();
        for (const Primitive& p : mesh.primitives) {
            GpuModel::Range r;
            r.first_index = uint32_t(indices.size());
            r.index_count = uint32_t(p.indices.size());
            r.vertex_offset = int32_t(vertices.size());
            r.material = p.material;
            vertices.insert(vertices.end(), p.vertices.begin(), p.vertices.end());
            indices.insert(indices.end(), p.indices.begin(), p.indices.end());
            ranges.push_back(r);
        }
    }
    if (vertices.empty()) {
        models_.push_back(std::move(gm));
        return models_.back().get();
    }
    gm->vertices = upload_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(), uint32_t(vertices.size() * sizeof(Vertex)));
    gm->indices = upload_buffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), uint32_t(indices.size() * sizeof(uint32_t)));
    models_.push_back(std::move(gm));
    return models_.back().get();
}

GpuModel* Renderer::instance_model(const Model& model, const GpuModel& shared) {
    std::vector<Vertex> vertices;
    for (const Mesh& mesh : model.meshes)
        for (const Primitive& p : mesh.primitives) vertices.insert(vertices.end(), p.vertices.begin(), p.vertices.end());
    auto gm = std::make_unique<GpuModel>(shared);
    gm->owns_indices = false;
    gm->vertices = upload_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(), uint32_t(vertices.size() * sizeof(Vertex)));
    models_.push_back(std::move(gm));
    return models_.back().get();
}

void Renderer::update_vertices(GpuModel* model, int32_t first, const Vertex* vertices, size_t count) {
    if (!model || !count) return;
    vertex_updates_.push_back({model->vertices, uint32_t(first) * uint32_t(sizeof(Vertex)), std::vector<Vertex>(vertices, vertices + count)});
}

void Renderer::flush_vertex_updates(SDL_GPUCommandBuffer* cmd) {
    if (vertex_updates_.empty()) return;
    size_t total = 0;
    for (const auto& u : vertex_updates_) total += u.vertices.size() * sizeof(Vertex);
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = uint32_t(total);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!tb) fail("vertex update");
    auto* dst = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, tb, false));
    size_t at = 0;
    for (const auto& u : vertex_updates_) {
        std::memcpy(dst + at, u.vertices.data(), u.vertices.size() * sizeof(Vertex));
        at += u.vertices.size() * sizeof(Vertex);
    }
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    at = 0;
    for (const auto& u : vertex_updates_) {
        const uint32_t size = uint32_t(u.vertices.size() * sizeof(Vertex));
        SDL_GPUTransferBufferLocation src{tb, uint32_t(at)};
        SDL_GPUBufferRegion region{u.buffer, u.offset, size};
        SDL_UploadToGPUBuffer(copy, &src, &region, false);
        at += size;
    }
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device_, tb);   // freed once the copy has run
    vertex_updates_.clear();
}

void Renderer::set_vsync(bool on) {
    if (!window_) return;
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!on)
        mode = SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX) ? SDL_GPU_PRESENTMODE_MAILBOX
               : SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE) ? SDL_GPU_PRESENTMODE_IMMEDIATE
                                                                                                    : SDL_GPU_PRESENTMODE_VSYNC;
    SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
}

const GpuTexture* Renderer::upload_texture(const Image& image) {
    auto tex = std::make_unique<GpuTexture>();
    tex->has_alpha = image.has_alpha();
    tex->width = image.width, tex->height = image.height;
    const uint32_t w = uint32_t(image.width), h = uint32_t(image.height);
    const uint32_t levels = 1 + uint32_t(std::floor(std::log2(double(std::max(w, h)))));
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;   // colour target: mip generation
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = levels;
    tex->texture = SDL_CreateGPUTexture(device_, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = uint32_t(image.rgba.size());
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbi);
    if (!tex->texture || !tb) fail("texture upload");
    std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), image.rgba.data(), image.rgba.size());
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{tb, 0, w, h};
    SDL_GPUTextureRegion dst{};
    dst.texture = tex->texture;
    dst.w = w;
    dst.h = h;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    if (levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, tex->texture);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    textures_.push_back(std::move(tex));
    return textures_.back().get();
}

void Renderer::encode_scene(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, uint32_t w,
                            uint32_t h, const std::vector<DrawItem>& items, const FrameParams& frame, bool hud) {
    ensure_depth(w, h);
    const Pipelines& pipes = pipelines_for(format);
    const Overlay* overlay = frame.overlay && (!frame.overlay->world.empty() || !frame.overlay->screen.empty()) ? frame.overlay : nullptr;
    flush_vertex_updates(cmd);
    if (overlay) upload_overlay(cmd, *overlay);
    const ParticleList* particles = frame.particles && !frame.particles->vertices.empty() ? frame.particles : nullptr;
    if (particles) upload_vertices(cmd, *particles, particle_buffer_);
    if (frame.sprites && !frame.sprites->vertices.empty()) upload_vertices(cmd, *frame.sprites, sprite_buffer_);

    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = {frame.clear.r, frame.clear.g, frame.clear.b, frame.clear.a};
    color.load_op = hud_pass_ ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_;
    depth.clear_depth = 0.0f;   // reversed depth: near 1, far 0
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    // Materials that read the screen behind them (ScreenTextureSampler) come after a copy of
    // the frame so far: a second pass that keeps the depth.
    if (!hud_pass_) screen_ready_ = false;
    const bool split = target == scene_color_ && !hud_pass_ && reads_screen(items, 0);
    depth.store_op = split ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);

    glm::mat4 view_proj = frame.proj * frame.view;
    const FrameParams* fp = &frame;   // the view model's own projection swaps in for its draws
    FrameParams viewmodel_frame;
    MaterialUniforms mu{};
    mu.sun_dir = glm::vec4(frame.sun_dir, 0);
    mu.sun_color = glm::vec4(frame.sun_color, 1);
    mu.ambient = glm::vec4(frame.ambient, 1);
    mu.fog = frame.fog;
    mu.camera = glm::vec4(frame.camera, 1);

    // Sky first, then opaque, then translucent back to front.
    std::vector<const DrawItem*> opaque, blended, sky, view_opaque, view_blended, refracting;
    for (const auto& it : items) {
        if (!it.visible || it.hidden || it.anim_hidden || it.zone_hidden || it.occluded || !it.model->vertices) continue;
        if (it.hud != hud_pass_) continue;
        const bool blend = it.material ? it.material->state.blend : it.blend;
        if (it.sky) sky.push_back(&it);
        else if (it.viewmodel) (blend ? view_blended : view_opaque).push_back(&it);
        else if (split && it.material && it.material->screen_slot >= 0) refracting.push_back(&it);
        else (blend ? blended : opaque).push_back(&it);
    }
    std::stable_partition(sky.begin(), sky.end(), [](const DrawItem* it) {
        return !(it->material ? it->material->state.blend : it->blend);
    });
    auto back_to_front = [&](const DrawItem* a, const DrawItem* b) {
        return glm::distance(a->center, frame.camera) > glm::distance(b->center, frame.camera);
    };
    std::sort(blended.begin(), blended.end(), back_to_front);
    std::sort(refracting.begin(), refracting.end(), back_to_front);
    // Glows drawn without a depth test (the "...NoZ" techniques: coronas, flares) go over the
    // other translucent surfaces rather than under whatever is sorted after them.
    std::stable_partition(blended.begin(), blended.end(), [](const DrawItem* it) { return !it->material || it->material->state.depth_test; });

    const GpuModel* bound = nullptr;
    SDL_GPUGraphicsPipeline* bound_pipe = nullptr;
    std::vector<uint8_t> vertex_block, fragment_block;
    std::vector<SDL_GPUTextureSamplerBinding> samplers;
    auto draw_material = [&](const DrawItem& it) {
        const GpuMaterial& m = *it.material;
        SDL_GPUGraphicsPipeline* pipe = material_pipeline(m, format);
        if (pipe != bound_pipe) {
            SDL_BindGPUGraphicsPipeline(pass, pipe);
            bound_pipe = pipe;
        }
        if (it.model != bound) {
            SDL_GPUBufferBinding vb{it.model->vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            SDL_GPUBufferBinding ib{it.model->indices, 0};
            SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            bound = it.model;
        }
        DrawContext ctx{*fp, it.world, view_proj, it.lighting, it.center, glm::vec2(float(w), float(h))};
        vertex_block = m.vertex_uniforms;
        for (const SemanticSlot& s : m.vertex_semantics) ctx.write(vertex_block, s);
        fragment_block = m.fragment_uniforms;
        for (const SemanticSlot& s : m.fragment_semantics) ctx.write(fragment_block, s);
        if (it.overrides) {
            for (const ParamOverride& o : *it.overrides) {
                for (const GpuMaterial::ParamField& pf : m.param_fields) {
                    if (pf.param != o.param) continue;
                    uint32_t size = 0, align = 0;
                    if (!std140_type(pf.field.type, size, align)) continue;
                    // Write each animated component (scalar or vector fields only).
                    for (int c = 0; c < o.count; ++c) {
                        const uint32_t at = pf.field.offset + 4 * uint32_t(o.first + c);
                        if (4 * uint32_t(o.first + c) >= size || pf.field.type.rfind("mat", 0) == 0) continue;
                        const float v = o.value[c] * pf.scale;
                        write_uniform(pf.fragment ? fragment_block : vertex_block, at, "float", &v, 1);
                    }
                }
            }
        }
        SDL_PushGPUVertexUniformData(cmd, 0, vertex_block.data(), uint32_t(vertex_block.size()));
        if (m.program->skinned) {
            static const auto kIdentity = [] {
                std::array<glm::mat4, kMaxJoints> a;
                a.fill(glm::mat4(1));
                return a;
            }();
            SDL_PushGPUVertexUniformData(cmd, 1, it.joints ? it.joints : kIdentity.data(), sizeof(glm::mat4) * kMaxJoints);
        }
        SDL_PushGPUFragmentUniformData(cmd, 0, fragment_block.data(), uint32_t(fragment_block.size()));
        auto bind = [&](const std::vector<const GpuTexture*>& textures, bool vertex) {
            if (textures.empty()) return;
            samplers.clear();
            for (const GpuTexture* t : textures) samplers.push_back({t->texture, sampler_});
            if (vertex) SDL_BindGPUVertexSamplers(pass, 0, samplers.data(), uint32_t(samplers.size()));
            else SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(), uint32_t(samplers.size()));
        };
        bind(m.vertex_textures, true);
        bind(m.fragment_textures, false);
        if (m.screen_slot >= 0 && screen_copy_) {   // the frame behind (see the split before the view model)
            const SDL_GPUTextureSamplerBinding screen{screen_ready_ ? screen_copy_ : white_.texture, clamp_sampler_};
            SDL_BindGPUFragmentSamplers(pass, uint32_t(m.screen_slot), &screen, 1);
        }
        SDL_DrawGPUIndexedPrimitives(pass, it.range.index_count, 1, it.range.first_index, it.range.vertex_offset, 0);
    };
    auto draw = [&](const DrawItem& it, SDL_GPUGraphicsPipeline* pipe) {
        if (it.material) return draw_material(it);
        if (it.joints) pipe = pipe == pipes.opaque ? pipes.opaque_skinned : pipes.blend_skinned;
        if (pipe != bound_pipe) {
            SDL_BindGPUGraphicsPipeline(pass, pipe);
            bound_pipe = pipe;
        }
        if (it.model != bound) {
            SDL_GPUBufferBinding vb{it.model->vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            SDL_GPUBufferBinding ib{it.model->indices, 0};
            SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            bound = it.model;
        }
        const DrawUniforms du{view_proj, it.world};
        SDL_PushGPUVertexUniformData(cmd, 0, &du, sizeof du);
        if (it.joints) SDL_PushGPUVertexUniformData(cmd, 1, it.joints, sizeof(glm::mat4) * kMaxJoints);
        mu.flags = glm::vec4(it.lightmap ? 1.0f : 0.0f, it.alpha_test ? 0.5f : 0.0f, 0, 0);
        SDL_PushGPUFragmentUniformData(cmd, 0, &mu, sizeof mu);
        SDL_GPUTextureSamplerBinding tex[2] = {
            {(it.diffuse ? it.diffuse : &white_)->texture, sampler_},
            {(it.lightmap ? it.lightmap : &white_)->texture, sampler_},
        };
        SDL_BindGPUFragmentSamplers(pass, 0, tex, 2);
        SDL_DrawGPUIndexedPrimitives(pass, it.range.index_count, 1, it.range.first_index, it.range.vertex_offset, 0);
    };
    if (!sky.empty()) {
        // CCustomSkyBoxSceneNode::renderInternal: each sky mesh at its own transform plus the
        // camera position, with the depth range squeezed to 0.999999 (behind everything else;
        // with reversed depth that is just above 0).
        SDL_GPUViewport vp{0, 0, float(w), float(h), 0.000001f, 0.000001f};
        SDL_SetGPUViewport(pass, &vp);
        for (const DrawItem* it : sky) {
            DrawItem moved = *it;
            moved.world[3] += glm::vec4(frame.camera, 0);
            const bool blend = it->material ? it->material->state.blend : it->blend;
            draw(moved, blend ? pipes.blend : pipes.opaque);
        }
        vp.min_depth = 0;
        vp.max_depth = 1;
        SDL_SetGPUViewport(pass, &vp);
    }
    for (const DrawItem* it : opaque) draw(*it, pipes.opaque);
    for (const DrawItem* it : blended) draw(*it, pipes.blend);
    if (particles) {
        SDL_GPUBufferBinding vb{particle_buffer_.buffer, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        for (const ParticleBatch& b : particles->batches) {
            SDL_BindGPUGraphicsPipeline(pass, b.additive ? pipes.particles_additive : pipes.particles_alpha);
            SDL_PushGPUVertexUniformData(cmd, 0, &view_proj, sizeof view_proj);
            SDL_GPUTextureSamplerBinding tex{b.texture->texture, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
            SDL_DrawGPUPrimitives(pass, b.count, 1, b.first, 0);
        }
    }
    if (split) {
        // ScreenTexture (CGameObject::BindParamsFromRTT): the frame so far, copied upside down
        // as the game's GL code expects (texture rows from the bottom); then what reads it.
        SDL_EndGPURenderPass(pass);
        copy_screen(cmd, w, h);
        color.load_op = SDL_GPU_LOADOP_LOAD;
        depth.load_op = SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
        bound = nullptr, bound_pipe = nullptr;
        for (const DrawItem* it : refracting) draw(*it, it->material->state.blend ? pipes.blend : pipes.opaque);
    }
    if (!view_opaque.empty() || !view_blended.empty()) {
        // The first-person view model: depth squeezed into [0.9, 1] (reversed depth: the
        // nearest tenth), so it stays in front of walls it would otherwise poke into.
        // Its field of view is its own (CNovaSceneManager::drawWeapons).
        SDL_GPUViewport vp{0, 0, float(w), float(h), 0.9f, 1.0f};
        SDL_SetGPUViewport(pass, &vp);
        if (frame.viewmodel_proj != glm::mat4(0)) {
            viewmodel_frame = frame;
            viewmodel_frame.proj = frame.viewmodel_proj;
            fp = &viewmodel_frame, view_proj = viewmodel_frame.proj * frame.view;
        }
        for (const DrawItem* it : view_opaque) draw(*it, pipes.opaque);
        for (const DrawItem* it : view_blended) draw(*it, pipes.blend);
        fp = &frame, view_proj = frame.proj * frame.view;
        vp.min_depth = 0;
        SDL_SetGPUViewport(pass, &vp);
    }
    if (overlay) {
        SDL_GPUBufferBinding vb{overlay_buffer_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        if (!overlay->world.empty() && !hud_pass_) {
            SDL_BindGPUGraphicsPipeline(pass, pipes.effects);
            SDL_PushGPUVertexUniformData(cmd, 0, &view_proj, sizeof view_proj);
            SDL_DrawGPUPrimitives(pass, uint32_t(overlay->world.size()), 1, 0, 0);
        }
    }
    if (hud) draw_screen_overlay(pass, cmd, pipes, w, h, overlay, frame.sprites);
    SDL_EndGPURenderPass(pass);
    // HUD models that read the screen (Repel, slow motion) see the finished scene.
    if (target == scene_color_ && !hud_pass_ && reads_screen(items, 1)) copy_screen(cmd, w, h);
}

bool Renderer::reads_screen(const std::vector<DrawItem>& items, int which) const {
    for (const DrawItem& it : items)
        if (it.material && it.material->screen_slot >= 0 && it.visible && !it.hidden && !it.anim_hidden && !it.zone_hidden &&
            (which < 0 || it.hud == (which == 1)))
            return true;
    return false;
}

void Renderer::copy_screen(SDL_GPUCommandBuffer* cmd, uint32_t w, uint32_t h) {
    SDL_GPUBlitInfo blit{};
    blit.source.texture = scene_color_;
    blit.source.w = w, blit.source.h = h;
    blit.destination.texture = screen_copy_;
    blit.destination.w = w, blit.destination.h = h;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.flip_mode = SDL_FLIP_VERTICAL;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &blit);
    screen_ready_ = true;
}

void Renderer::draw_screen_overlay(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* cmd, const Pipelines& pipes, uint32_t w,
                                   uint32_t h, const Overlay* o, const ParticleList* sprites) {
    // Pixels, origin top-left (SDL GPU clip space has +Y up).
    const glm::mat4 pixels = glm::mat4(glm::vec4(2.0f / float(w), 0, 0, 0), glm::vec4(0, -2.0f / float(h), 0, 0),
                                       glm::vec4(0, 0, 1, 0), glm::vec4(-1, 1, 0, 1));
    if (o && !o->screen.empty()) {
        SDL_GPUBufferBinding vb{overlay_buffer_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_BindGPUGraphicsPipeline(pass, pipes.hud);
        SDL_PushGPUVertexUniformData(cmd, 0, &pixels, sizeof pixels);
        SDL_DrawGPUPrimitives(pass, uint32_t(o->screen.size()), 1, uint32_t(o->world.size()), 0);
    }
    if (sprites && !sprites->vertices.empty()) {
        SDL_GPUBufferBinding vb{sprite_buffer_.buffer, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_BindGPUGraphicsPipeline(pass, pipes.sprites);
        SDL_PushGPUVertexUniformData(cmd, 0, &pixels, sizeof pixels);
        bool additive = false;
        for (const ParticleBatch& b : sprites->batches) {
            if (b.additive != additive) SDL_BindGPUGraphicsPipeline(pass, (additive = b.additive) ? pipes.sprites_additive : pipes.sprites);
            SDL_GPUTextureSamplerBinding tex{b.texture->texture, clamp_sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
            SDL_DrawGPUPrimitives(pass, b.count, 1, b.first, 0);
        }
    }
}

void Renderer::ensure_post_targets(uint32_t w, uint32_t h) {
    if (scene_color_ && post_w_ == w && post_h_ == h) return;
    for (SDL_GPUTexture** t : {&scene_color_, &screen_copy_, &blur_a_, &blur_b_})
        if (*t) SDL_ReleaseGPUTexture(device_, *t), *t = nullptr;
    auto make = [&](uint32_t tw, uint32_t th) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = kOffscreenFormat;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = std::max(tw, 1u);
        ti.height = std::max(th, 1u);
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &ti);
        if (!t) fail("post-effect target");
        return t;
    };
    scene_color_ = make(w, h);
    screen_copy_ = make(w, h);
    blur_a_ = make(w / 4, h / 4);   // the blur works at a quarter of the screen size
    blur_b_ = make(w / 4, h / 4);
    post_w_ = w, post_h_ = h;
}

const Renderer::PostPipelines& Renderer::post_pipelines_for(SDL_GPUTextureFormat format) {
    auto it = post_pipelines_.find(int(format));
    if (it != post_pipelines_.end()) return it->second;
    auto make = [&](SDL_GPUShader* fs, SDL_GPUTextureFormat target) {
        SDL_GPUColorTargetDescription ct{};
        ct.format = target;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs_post_;
        pi.fragment_shader = fs;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &pi);
        if (!p) fail("SDL_CreateGPUGraphicsPipeline (post)");
        return p;
    };
    return post_pipelines_[int(format)] = PostPipelines{make(fs_post_blur_, kOffscreenFormat), make(fs_post_final_, format)};
}

void Renderer::encode_hud(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, uint32_t w,
                          uint32_t h, const std::vector<DrawItem>& items, const FrameParams& frame) {
    // The HUD scene over the finished frame: its own camera, no fog, then the 2D overlay.
    FrameParams hf = frame;
    hf.view = frame.hud_view, hf.proj = frame.hud_proj;
    hf.camera = glm::vec3(glm::inverse(frame.hud_view)[3]);
    hf.post = nullptr, hf.particles = nullptr;
    hf.fog_density = 0, hf.fog_start = 1e9f, hf.fog_end = 2e9f;
    hud_pass_ = true;
    encode_scene(cmd, target, format, w, h, items, hf, true);
    hud_pass_ = false;
}

void Renderer::encode(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, uint32_t w,
                      uint32_t h, const std::vector<DrawItem>& items, const FrameParams& frame) {
    const PostParams* post = frame.post && frame.post->active() ? frame.post : nullptr;
    // A view model reading the screen behind it needs the scene in a texture it can copy:
    // the offscreen path, with no effect (its final pass is then a plain copy).
    static const PostParams kNoEffects;
    if (!post && reads_screen(items)) post = &kNoEffects;
    if (!post) {
        encode_scene(cmd, target, format, w, h, items, frame, !frame.hud3d);
        if (frame.hud3d) encode_hud(cmd, target, format, w, h, items, frame);
        return;
    }

    // Scene to a texture, then the blur (quarter size, horizontal then vertical, as the
    // game's RenderBlur), then everything combined into the target, then the HUD.
    ensure_post_targets(w, h);
    if (!grading_) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = 256, ti.height = 1, ti.layer_count_or_depth = 1, ti.num_levels = 1;
        grading_ = SDL_CreateGPUTexture(device_, &ti);
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = 256 * 4;
        grading_transfer_ = SDL_CreateGPUTransferBuffer(device_, &tbi);
        if (!grading_ || !grading_transfer_) fail("colour grading texture");
    }
    if (post->grading && post->grading_version != grading_version_) {
        grading_version_ = post->grading_version;
        std::memcpy(SDL_MapGPUTransferBuffer(device_, grading_transfer_, true), post->grading_curve.data(), 256 * 4);
        SDL_UnmapGPUTransferBuffer(device_, grading_transfer_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{grading_transfer_, 0, 256, 1};
        SDL_GPUTextureRegion dst{};
        dst.texture = grading_, dst.w = 256, dst.h = 1, dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
    }
    encode_scene(cmd, scene_color_, kOffscreenFormat, w, h, items, frame, false);
    const PostPipelines& pp = post_pipelines_for(format);
    auto run = [&](SDL_GPUTexture* out, uint32_t ow, uint32_t oh, SDL_GPUGraphicsPipeline* pipe,
                   std::initializer_list<SDL_GPUTexture*> inputs, const void* uniforms, uint32_t size, bool hud) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = out;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        SDL_GPUViewport vp{0, 0, float(ow), float(oh), 0, 1};
        SDL_SetGPUViewport(pass, &vp);
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        std::vector<SDL_GPUTextureSamplerBinding> tex;
        for (SDL_GPUTexture* t : inputs) tex.push_back({t, tex.size() == 4 ? sampler_ : clamp_sampler_});   // the 5th repeats (heat haze)
        SDL_BindGPUFragmentSamplers(pass, 0, tex.data(), uint32_t(tex.size()));
        SDL_PushGPUFragmentUniformData(cmd, 0, uniforms, size);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        if (hud) draw_screen_overlay(pass, cmd, pipelines_for(format), w, h, frame.overlay, frame.sprites);
        SDL_EndGPURenderPass(pass);
    };
    const bool blur = post->dof && post->blur_step > 0;
    if (blur) {
        const float bw = float(std::max(w / 4, 1u)), bh = float(std::max(h / 4, 1u));
        const glm::vec4 hstep(1.3846154f * post->blur_step / bw, 0, 3.2307692f * post->blur_step / bw, 0);
        const glm::vec4 vstep(0, 1.3846154f * post->blur_step / bh, 0, 3.2307692f * post->blur_step / bh);
        run(blur_a_, uint32_t(bw), uint32_t(bh), pp.blur, {scene_color_}, &hstep, sizeof hstep, false);
        run(blur_b_, uint32_t(bw), uint32_t(bh), pp.blur, {blur_a_}, &vstep, sizeof vstep, false);
    }
    struct {
        glm::mat4 inv_view_proj;
        glm::vec4 camera, forward, dof, fog, fog_color, grading, heat, effects, gray;
    } u;
    u.effects = {post->sepia, post->levels_white, post->flash_threshold, post->radial_blur};
    u.gray = glm::vec4(post->gray_tint, post->gray);
    u.inv_view_proj = glm::inverse(frame.proj * frame.view);
    u.camera = glm::vec4(frame.camera, 1);
    u.forward = glm::vec4(-glm::vec3(glm::transpose(frame.view)[2]), 0);   // the view looks down -Z
    u.dof = {post->dof_focus, post->dof_range, post->dof_factor, post->dof ? 1.0f : 0.0f};
    u.fog = {post->fog_top, post->fog_bottom, post->fog_opacity, post->height_fog ? 1.0f : 0.0f};
    u.fog_color = glm::vec4(post->fog_color, 1);
    u.grading = glm::vec4(post->grading ? 1.0f : 0.0f, 0, 0, 0);
    const bool heat = post->heat_haze && post->heat_texture;
    u.heat = heat ? glm::vec4(post->heat_strength, post->heat_overbright, post->heat_blend, post->heat_time) : glm::vec4(0, 1, 1, -1);
    run(target, w, h, pp.final, {scene_color_, blur ? blur_b_ : scene_color_, depth_, grading_, heat ? post->heat_texture->texture : grading_}, &u,
        sizeof u, !frame.hud3d);
    if (frame.hud3d) encode_hud(cmd, target, format, w, h, items, frame);
}

void Renderer::upload_vertices(SDL_GPUCommandBuffer* cmd, const ParticleList& p, DynamicBuffer& b) {
    const uint32_t count = uint32_t(p.vertices.size());
    if (count > b.capacity) {
        if (b.buffer) SDL_ReleaseGPUBuffer(device_, b.buffer);
        if (b.transfer) SDL_ReleaseGPUTransferBuffer(device_, b.transfer);
        b.capacity = std::max(count * 2, 8192u);
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = b.capacity * uint32_t(sizeof(ParticleVertex));
        b.buffer = SDL_CreateGPUBuffer(device_, &bi);
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = bi.size;
        b.transfer = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (!b.buffer || !b.transfer) fail("vertex buffer");
    }
    auto* dst = static_cast<ParticleVertex*>(SDL_MapGPUTransferBuffer(device_, b.transfer, true));
    std::copy(p.vertices.begin(), p.vertices.end(), dst);
    SDL_UnmapGPUTransferBuffer(device_, b.transfer);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{b.transfer, 0};
    SDL_GPUBufferRegion region{b.buffer, 0, count * uint32_t(sizeof(ParticleVertex))};
    SDL_UploadToGPUBuffer(copy, &src, &region, true);
    SDL_EndGPUCopyPass(copy);
}

void Renderer::upload_overlay(SDL_GPUCommandBuffer* cmd, const Overlay& o) {
    const uint32_t count = uint32_t(o.world.size() + o.screen.size());
    if (count > overlay_capacity_) {
        if (overlay_buffer_) SDL_ReleaseGPUBuffer(device_, overlay_buffer_);
        if (overlay_transfer_) SDL_ReleaseGPUTransferBuffer(device_, overlay_transfer_);
        overlay_capacity_ = std::max(count * 2, 4096u);
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = overlay_capacity_ * uint32_t(sizeof(ColorVertex));
        overlay_buffer_ = SDL_CreateGPUBuffer(device_, &bi);
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = bi.size;
        overlay_transfer_ = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (!overlay_buffer_ || !overlay_transfer_) fail("overlay buffer");
    }
    auto* dst = static_cast<ColorVertex*>(SDL_MapGPUTransferBuffer(device_, overlay_transfer_, true));
    std::copy(o.world.begin(), o.world.end(), dst);
    std::copy(o.screen.begin(), o.screen.end(), dst + o.world.size());
    SDL_UnmapGPUTransferBuffer(device_, overlay_transfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{overlay_transfer_, 0};
    SDL_GPUBufferRegion region{overlay_buffer_, 0, count * uint32_t(sizeof(ColorVertex))};
    SDL_UploadToGPUBuffer(copy, &src, &region, true);
    SDL_EndGPUCopyPass(copy);
}

bool Renderer::render_to_window(const std::vector<DrawItem>& items, const FrameParams& frame) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUTexture* swap = nullptr;
    uint32_t w = 0, h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swap, &w, &h) || !swap) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    encode(cmd, swap, SDL_GetGPUSwapchainTextureFormat(device_, window_), w, h, items, frame);
    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
}

Image Renderer::render_to_image(const std::vector<DrawItem>& items, const FrameParams& frame, int width, int height) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kOffscreenFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = uint32_t(width);
    ti.height = uint32_t(height);
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device_, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = uint32_t(width) * uint32_t(height) * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbi);
    if (!target || !tb) fail("offscreen target");

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    encode(cmd, target, kOffscreenFormat, uint32_t(width), uint32_t(height), items, frame);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = target;
    src.w = uint32_t(width);
    src.h = uint32_t(height);
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{tb, 0, uint32_t(width), uint32_t(height)};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    Image img{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    std::memcpy(img.rgba.data(), SDL_MapGPUTransferBuffer(device_, tb, false), img.rgba.size());
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    SDL_ReleaseGPUTexture(device_, target);
    return img;
}

}  // namespace nova
