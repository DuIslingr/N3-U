// SDL_GPU renderer: uploads models and textures, draws a list of mesh instances.
// One code path for Vulkan, Direct3D 12 and Metal (SPIR-V shaders for now).
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "assets/bdae.h"
#include "assets/irradiance.h"
#include "assets/texture.h"
#include "render/material.h"
#include "shader/shader_compiler.h"

namespace nova {

struct GpuTexture {
    SDL_GPUTexture* texture = nullptr;
    bool has_alpha = false;
};

// A model's vertex/index data on the GPU, with one range per primitive.
struct GpuModel {
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr;
    struct Range {
        uint32_t first_index = 0, index_count = 0;
        int32_t vertex_offset = 0;
        int material = -1;
    };
    std::vector<std::vector<Range>> meshes;   // per Model::meshes entry
};

struct DrawItem {
    const GpuModel* model = nullptr;
    GpuModel::Range range;
    glm::mat4 world{1};
    const GpuTexture* diffuse = nullptr;
    const GpuTexture* lightmap = nullptr;
    bool blend = false;
    bool alpha_test = false;
    glm::vec3 center{0};                        // world-space bounds centre, for sorting
    const glm::mat4* joints = nullptr;          // skinned: kMaxJoints palette (object space: placement^-1 * world * inverse bind)
    bool visible = true;                        // false for LOD levels not currently shown
    bool hidden = false;                        // switched off by game logic (disabled or hidden object)
    bool anim_hidden = false;                   // under a node an animation has made invisible
    bool zone_hidden = false;                   // its zone cannot be seen from the camera's zone
    const GpuMaterial* material = nullptr;      // game shader; null: built-in shader with diffuse/lightmap
    const struct ObjectLighting* lighting = nullptr;   // probe and nearest light (defaults if null)
    bool sky = false;                           // skybox: drawn first, around the camera, at the far depth
    const std::vector<struct ParamOverride>* overrides = nullptr;   // animated material parameters
};

// An animated material parameter value for one object: components [first, first + count).
struct ParamOverride {
    std::string param;
    int first = 0, count = 0;
    glm::vec4 value{0};
};

// Per-object lighting the game computes in CNovaSceneManager::UpdateLights.
struct ObjectLighting {
    IrradianceParams probe;                     // ird_* uniforms; SpecDirection/SpecColor from its peak
    glm::vec3 center{0};                        // world bounds centre, for RimFactor
    bool has_light = false;                     // L0_*: nearest light (blended with the next when inside both)
    glm::vec3 l0_position{0};                   // world
    glm::vec4 l0_color{0};                      // colour * intensity
    glm::vec3 l0_attenuation{0};                // (falloff start, 1 / falloff length, 0)
    glm::vec3 l0_scale{1};                      // 1 / light node scale
};

// A dynamic point light, as the game's shaders see it (Light<n>... uniforms).
struct PointLight {
    glm::vec3 position{0};
    glm::vec3 attenuation{1, 0, 0};             // the shaders use [1] as 1/range
    glm::vec4 diffuse{0}, ambient{0}, specular{0};
    float radius = 0;                           // affects objects closer than this
    bool fake = false;                          // zone "fake light": specular (L0_*) only
};

struct FrameParams {
    glm::mat4 view{1};
    glm::mat4 proj{1};
    glm::vec3 camera{0};
    glm::vec3 sun_dir = glm::normalize(glm::vec3(0.4f, 0.3f, -1.0f));
    glm::vec3 sun_color{0.75f};
    glm::vec3 ambient{0.45f};
    glm::vec4 fog{0.35f, 0.37f, 0.42f, 0.0f};
    glm::vec4 clear{0.35f, 0.37f, 0.42f, 1.0f};
    // Game shader globals (level properties).
    glm::vec4 fog_color{0, 0, 0, 1};
    float fog_start = 50, fog_end = 250, fog_density = 0;
    float time = 0;                             // seconds
    std::vector<PointLight> lights;
};

class Renderer {
public:
    // `window` may be null for headless (offscreen) rendering.
    Renderer(SDL_Window* window, const std::string& shader_dir);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    GpuModel* upload_model(const Model& model);
    const GpuTexture* upload_texture(const Image& image);
    const GpuTexture* white() const { return &white_; }
    const GpuTexture* white_cube() const { return &white_cube_; }
    const GpuProgram* create_program(const CompiledProgram& compiled);

    // Draw to the window's swapchain. Returns false if the frame was skipped.
    bool render_to_window(const std::vector<DrawItem>& items, const FrameParams& frame);
    // Draw offscreen and return the pixels (RGBA8, first row first).
    Image render_to_image(const std::vector<DrawItem>& items, const FrameParams& frame, int width, int height);

    SDL_GPUDevice* device() const { return device_; }

private:
    struct Pipelines {
        SDL_GPUGraphicsPipeline* opaque = nullptr;
        SDL_GPUGraphicsPipeline* blend = nullptr;
        SDL_GPUGraphicsPipeline* opaque_skinned = nullptr;
        SDL_GPUGraphicsPipeline* blend_skinned = nullptr;
    };
    const Pipelines& pipelines_for(SDL_GPUTextureFormat format);
    SDL_GPUShader* load_shader(const std::string& path, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniforms);
    void ensure_depth(uint32_t w, uint32_t h);
    void encode(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, uint32_t w, uint32_t h,
                const std::vector<DrawItem>& items, const FrameParams& frame);
    SDL_GPUBuffer* upload_buffer(SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size);
    SDL_GPUGraphicsPipeline* material_pipeline(const GpuMaterial& m, SDL_GPUTextureFormat format);
    SDL_GPUShader* create_shader(const std::vector<uint32_t>& spirv, SDL_GPUShaderStage stage, uint32_t samplers,
                                 uint32_t uniforms);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vs_ = nullptr;
    SDL_GPUShader* vs_skinned_ = nullptr;
    SDL_GPUShader* fs_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    uint32_t depth_w_ = 0, depth_h_ = 0;
    std::unordered_map<int, Pipelines> pipelines_;
    GpuTexture white_, white_cube_;
    std::vector<std::unique_ptr<GpuProgram>> programs_;
    std::unordered_map<std::string, SDL_GPUGraphicsPipeline*> material_pipelines_;   // program/state/format
    std::vector<std::unique_ptr<GpuModel>> models_;
    std::vector<std::unique_ptr<GpuTexture>> textures_;
};

}  // namespace nova
