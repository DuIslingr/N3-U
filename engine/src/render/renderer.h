// SDL_GPU renderer: uploads models and textures, draws a list of mesh instances.
// One code path for Vulkan, Direct3D 12 and Metal (SPIR-V shaders for now).
#pragma once

#include <array>
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
    int width = 0, height = 0;
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
    bool owns_indices = true;                 // false: an object's copy sharing its model's indices
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
    bool occluded = false;                      // a corona whose view ray is blocked
    const GpuMaterial* material = nullptr;      // game shader; null: built-in shader with diffuse/lightmap
    const struct ObjectLighting* lighting = nullptr;   // probe and nearest light (defaults if null)
    bool sky = false;                           // skybox: drawn first, around the camera, at the far depth
    bool viewmodel = false;                     // first-person hands and weapon: drawn last, in front of the world
    bool hud = false;                           // the 3D HUD (NV_hud.bdae): drawn in the HUD pass with its own camera
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

// A coloured vertex for effects and the HUD. rgba: red in the low byte.
struct ColorVertex {
    glm::vec3 position;
    uint32_t rgba;
};
inline uint32_t pack_rgba(const glm::vec4& c) {
    const glm::vec4 v = glm::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f;
    return uint32_t(v.r) | uint32_t(v.g) << 8 | uint32_t(v.b) << 16 | uint32_t(v.a) << 24;
}
// Coloured triangles drawn after the scene: additive world-space effects (tracers,
// bolts) and the screen-space HUD (pixels, origin top-left, alpha-blended on top).
struct Overlay {
    std::vector<ColorVertex> world, screen;
    void clear() { world.clear(), screen.clear(); }
};

// Particles (game/particles.cpp): textured quads in world space, by texture and blend.
struct ParticleVertex {
    glm::vec3 position;
    glm::vec2 uv;
    uint32_t rgba;
};
struct ParticleBatch {
    const struct GpuTexture* texture = nullptr;
    bool additive = false;
    uint32_t first = 0, count = 0;
};
struct ParticleList {
    std::vector<ParticleVertex> vertices;
    std::vector<ParticleBatch> batches;
};

// Post effects (the game's PostEffects, enabled and tuned by level scripts).
struct PostParams {
    bool dof = false;                  // DOF: blur by distance from the focus depth
    float dof_focus = 0, dof_range = 0, dof_factor = 1, blur_step = 0;   // blur step: texels of the blur target
    bool height_fog = false;           // HEIGHTFOG: fog below a height
    float fog_top = 0, fog_bottom = 0, fog_opacity = 0;
    glm::vec3 fog_color{1};
    // Colour grading (ColorCorrection, CCFS.glsl): per-channel curves, 256 RGBA entries
    // (r from .r, g from .g, b from .b); bump the version when they change.
    bool grading = false;
    std::array<uint8_t, 256 * 4> grading_curve{};
    uint32_t grading_version = 0;
    // HeatHaze (HeatHazeFS.glsl): the image shifted by two scrolling layers of a bump texture,
    // tinted orange as blend drops below 1, brightened by overbright.
    bool heat_haze = false;
    float heat_strength = 0, heat_overbright = 1, heat_blend = 1, heat_time = 0;   // time: seconds
    const struct GpuTexture* heat_texture = nullptr;
    // Sepia (SepiaFS.glsl, the flashback): mix towards sepia by `sepia` (< 0: off).
    // ColorLevels (ColorLevelsFS.glsl): the colour divided by `levels_white` (1: off).
    // Flash (FlashFS.glsl): the image's grey brightness less `flash_threshold`, added
    // (<= -2: off).
    float sepia = -1, levels_white = 1, flash_threshold = -2;
    // Gray (GrayFS.glsl, StartEndLevelEffect): the image mixed towards its grey tinted by
    // gray_tint, `gray` of the way back to colour (< 0: off; 0 all grey).
    float gray = -1;
    glm::vec3 gray_tint{1};
    // RadialBlur (RadialBlur2FP.glsl), the motion blur of a fast turn: each sample stepped
    // out from the centre by blurwidth x its squared distance (0: off).
    float radial_blur = 0;
    bool active() const {
        return dof || height_fog || grading || heat_haze || sepia >= 0 || levels_white != 1 || flash_threshold > -2 || gray >= 0 || radial_blur > 0;
    }
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
    const Overlay* overlay = nullptr;
    const PostParams* post = nullptr;
    const ParticleList* particles = nullptr;
    const ParticleList* sprites = nullptr;   // HUD: textured quads in pixels (origin top left), alpha
    // The 3D HUD: items flagged `hud`, seen through their own camera, over the finished frame.
    bool hud3d = false;
    glm::mat4 hud_view{1}, hud_proj{1};
};

class Renderer {
public:
    // `window` may be null for headless (offscreen) rendering.
    Renderer(SDL_Window* window, const std::string& shader_dir);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    GpuModel* upload_model(const Model& model);
    // An object's own copy of a model's vertices, for vertices that change (morph targets);
    // it shares the model's indices and ranges.
    GpuModel* instance_model(const Model& model, const GpuModel& shared);
    // Replace vertices [first, first + count) of such a copy, before the next frame is drawn.
    void update_vertices(GpuModel* model, int32_t first, const Vertex* vertices, size_t count);
    const GpuTexture* upload_texture(const Image& image);
    // V-Sync on: presents wait for the display; off: the lowest-latency mode there is
    // (mailbox, else immediate), the frame rate as high as it goes.
    void set_vsync(bool on);
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
        SDL_GPUGraphicsPipeline* particles_additive = nullptr;
        SDL_GPUGraphicsPipeline* particles_alpha = nullptr;
        SDL_GPUGraphicsPipeline* effects = nullptr;    // overlay: world, additive
        SDL_GPUGraphicsPipeline* hud = nullptr;        // overlay: screen, alpha
        SDL_GPUGraphicsPipeline* sprites = nullptr;    // screen, textured, alpha
        SDL_GPUGraphicsPipeline* sprites_additive = nullptr;   // screen, textured, additive (lens flares)
    };
    void upload_overlay(SDL_GPUCommandBuffer* cmd, const Overlay& o);
    struct VertexUpdate {
        SDL_GPUBuffer* buffer;
        uint32_t offset;                 // bytes
        std::vector<Vertex> vertices;
    };
    std::vector<VertexUpdate> vertex_updates_;
    void flush_vertex_updates(SDL_GPUCommandBuffer* cmd);
    // The scene into `target`; `hud`: also the screen overlay (otherwise drawn after post effects).
    bool hud_pass_ = false;   // encode_scene draws the HUD items over the target (kept), not the world
    void encode_hud(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, uint32_t w, uint32_t h,
                    const std::vector<DrawItem>& items, const FrameParams& frame);
    void encode_scene(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, uint32_t w, uint32_t h,
                      const std::vector<DrawItem>& items, const FrameParams& frame, bool hud);
    void draw_screen_overlay(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* cmd, const Pipelines& pipes, uint32_t w,
                             uint32_t h, const Overlay* o, const ParticleList* sprites);
    void ensure_post_targets(uint32_t w, uint32_t h);
    struct PostPipelines {
        SDL_GPUGraphicsPipeline* blur = nullptr;
        SDL_GPUGraphicsPipeline* final = nullptr;
    };
    const PostPipelines& post_pipelines_for(SDL_GPUTextureFormat format);
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
    SDL_GPUShader* vs_particle_ = nullptr;
    SDL_GPUShader* fs_particle_ = nullptr;
    // Vertices into a growing GPU buffer (with its upload buffer), before a render pass.
    struct DynamicBuffer {
        SDL_GPUBuffer* buffer = nullptr;
        SDL_GPUTransferBuffer* transfer = nullptr;
        uint32_t capacity = 0;   // vertices
    };
    void upload_vertices(SDL_GPUCommandBuffer* cmd, const ParticleList& p, DynamicBuffer& b);
    DynamicBuffer particle_buffer_, sprite_buffer_;
    SDL_GPUShader* vs_post_ = nullptr;
    SDL_GPUShader* fs_post_blur_ = nullptr;
    SDL_GPUShader* fs_post_final_ = nullptr;
    SDL_GPUSampler* clamp_sampler_ = nullptr;
    SDL_GPUTexture* scene_color_ = nullptr;             // post effects: the scene, then two blur targets
    SDL_GPUTexture* blur_a_ = nullptr;
    SDL_GPUTexture* blur_b_ = nullptr;
    SDL_GPUTexture* grading_ = nullptr;                 // 256x1 colour grading curves
    SDL_GPUTransferBuffer* grading_transfer_ = nullptr;
    uint32_t grading_version_ = ~0u;
    uint32_t post_w_ = 0, post_h_ = 0;
    std::unordered_map<int, PostPipelines> post_pipelines_;
    SDL_GPUShader* vs_color_ = nullptr;
    SDL_GPUShader* fs_color_ = nullptr;
    SDL_GPUBuffer* overlay_buffer_ = nullptr;
    SDL_GPUTransferBuffer* overlay_transfer_ = nullptr;
    uint32_t overlay_capacity_ = 0;                    // vertices
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
