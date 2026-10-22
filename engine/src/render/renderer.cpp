#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

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

std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open shader " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

}  // namespace

Renderer::Renderer(SDL_Window* window, const std::string& shader_dir) : window_(window) {
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!device_) fail("SDL_CreateGPUDevice");
    if (window_ && !SDL_ClaimWindowForGPUDevice(device_, window_)) fail("SDL_ClaimWindowForGPUDevice");
    if (window_) SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    vs_ = load_shader(shader_dir + "/mesh.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    fs_ = load_shader(shader_dir + "/mesh.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);

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

    Image white{1, 1, {255, 255, 255, 255}};
    white_ = *upload_texture(white);
}

Renderer::~Renderer() {
    if (!device_) return;
    SDL_WaitForGPUIdle(device_);
    for (auto& m : models_) {
        SDL_ReleaseGPUBuffer(device_, m->vertices);
        SDL_ReleaseGPUBuffer(device_, m->indices);
    }
    for (auto& t : textures_) SDL_ReleaseGPUTexture(device_, t->texture);
    for (auto& [fmt, p] : pipelines_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, p.opaque);
        SDL_ReleaseGPUGraphicsPipeline(device_, p.blend);
    }
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    SDL_ReleaseGPUSampler(device_, sampler_);
    SDL_ReleaseGPUShader(device_, vs_);
    SDL_ReleaseGPUShader(device_, fs_);
    if (window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
}

SDL_GPUShader* Renderer::load_shader(const std::string& path, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniforms) {
    const auto code = read_binary(path);
    SDL_GPUShaderCreateInfo ci{};
    ci.code_size = code.size();
    ci.code = code.data();
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = stage;
    ci.num_samplers = samplers;
    ci.num_uniform_buffers = uniforms;
    SDL_GPUShader* s = SDL_CreateGPUShader(device_, &ci);
    if (!s) fail("SDL_CreateGPUShader " + path);
    return s;
}

const Renderer::Pipelines& Renderer::pipelines_for(SDL_GPUTextureFormat format) {
    auto it = pipelines_.find(int(format));
    if (it != pipelines_.end()) return it->second;

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = sizeof(Vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[5] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, position))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, normal))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv0))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv1))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(Vertex, color))},
    };

    auto make = [&](bool blend) {
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
        pi.vertex_shader = vs_;
        pi.fragment_shader = fs_;
        pi.vertex_input_state.vertex_buffer_descriptions = &vb;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = 5;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;   // TODO: match the game's winding and cull
        pi.depth_stencil_state.enable_depth_test = true;
        pi.depth_stencil_state.enable_depth_write = !blend;
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pi.target_info.depth_stencil_format = kDepthFormat;
        pi.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &pi);
        if (!p) fail("SDL_CreateGPUGraphicsPipeline");
        return p;
    };
    return pipelines_[int(format)] = Pipelines{make(false), make(true)};
}

void Renderer::ensure_depth(uint32_t w, uint32_t h) {
    if (depth_ && depth_w_ == w && depth_h_ == h) return;
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kDepthFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
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

const GpuTexture* Renderer::upload_texture(const Image& image) {
    auto tex = std::make_unique<GpuTexture>();
    tex->has_alpha = image.has_alpha();
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

void Renderer::encode(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, uint32_t w,
                      uint32_t h, const std::vector<DrawItem>& items, const FrameParams& frame) {
    ensure_depth(w, h);
    const Pipelines& pipes = pipelines_for(format);

    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = {frame.clear.r, frame.clear.g, frame.clear.b, frame.clear.a};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);

    const glm::mat4 view_proj = frame.proj * frame.view;
    MaterialUniforms mu{};
    mu.sun_dir = glm::vec4(frame.sun_dir, 0);
    mu.sun_color = glm::vec4(frame.sun_color, 1);
    mu.ambient = glm::vec4(frame.ambient, 1);
    mu.fog = frame.fog;
    mu.camera = glm::vec4(frame.camera, 1);

    // Opaque first, then translucent back to front.
    std::vector<const DrawItem*> opaque, blended;
    for (const auto& it : items) (it.blend ? blended : opaque).push_back(&it);
    std::sort(blended.begin(), blended.end(), [&](const DrawItem* a, const DrawItem* b) {
        return glm::distance(a->center, frame.camera) > glm::distance(b->center, frame.camera);
    });

    const GpuModel* bound = nullptr;
    SDL_GPUGraphicsPipeline* bound_pipe = nullptr;
    auto draw = [&](const DrawItem& it, SDL_GPUGraphicsPipeline* pipe) {
        if (!it.model->vertices) return;
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
        mu.flags = glm::vec4(it.lightmap ? 1.0f : 0.0f, it.alpha_test ? 0.5f : 0.0f, 0, 0);
        SDL_PushGPUFragmentUniformData(cmd, 0, &mu, sizeof mu);
        SDL_GPUTextureSamplerBinding tex[2] = {
            {(it.diffuse ? it.diffuse : &white_)->texture, sampler_},
            {(it.lightmap ? it.lightmap : &white_)->texture, sampler_},
        };
        SDL_BindGPUFragmentSamplers(pass, 0, tex, 2);
        SDL_DrawGPUIndexedPrimitives(pass, it.range.index_count, 1, it.range.first_index, it.range.vertex_offset, 0);
    };
    for (const DrawItem* it : opaque) draw(*it, pipes.opaque);
    for (const DrawItem* it : blended) draw(*it, pipes.blend);
    SDL_EndGPURenderPass(pass);
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
