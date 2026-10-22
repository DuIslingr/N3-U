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
#include "assets/texture.h"

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

    // Draw to the window's swapchain. Returns false if the frame was skipped.
    bool render_to_window(const std::vector<DrawItem>& items, const FrameParams& frame);
    // Draw offscreen and return the pixels (RGBA8, first row first).
    Image render_to_image(const std::vector<DrawItem>& items, const FrameParams& frame, int width, int height);

    SDL_GPUDevice* device() const { return device_; }

private:
    struct Pipelines {
        SDL_GPUGraphicsPipeline* opaque = nullptr;
        SDL_GPUGraphicsPipeline* blend = nullptr;
    };
    const Pipelines& pipelines_for(SDL_GPUTextureFormat format);
    SDL_GPUShader* load_shader(const std::string& path, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniforms);
    void ensure_depth(uint32_t w, uint32_t h);
    void encode(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, uint32_t w, uint32_t h,
                const std::vector<DrawItem>& items, const FrameParams& frame);
    SDL_GPUBuffer* upload_buffer(SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vs_ = nullptr;
    SDL_GPUShader* fs_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    uint32_t depth_w_ = 0, depth_h_ = 0;
    std::unordered_map<int, Pipelines> pipelines_;
    GpuTexture white_;
    std::vector<std::unique_ptr<GpuModel>> models_;
    std::vector<std::unique_ptr<GpuTexture>> textures_;
};

}  // namespace nova
