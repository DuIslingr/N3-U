// A loaded level: its objects resolved to GPU models and flattened into draw items.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "assets/level.h"
#include "core/vfs.h"
#include "render/renderer.h"

namespace nova {

struct SceneStats {
    size_t placements = 0, models = 0, missing_models = 0, textures = 0, missing_textures = 0, draws = 0;
    double load_seconds = 0;
};

class Scene {
public:
    Scene(const Vfs& vfs, Renderer& renderer, const Gol& gol);

    void load_level(const std::string& lvc_name);

    const std::vector<DrawItem>& draws() const { return draws_; }
    const Level& level() const { return level_; }
    const SceneStats& stats() const { return stats_; }

    // Where the player starts: the level's start spawn point (eye height added).
    glm::vec3 start_position{0, 0, 1.7f};
    float start_yaw = 0;   // degrees about Z

private:
    struct LoadedModel {
        Model cpu;
        GpuModel* gpu = nullptr;
        struct MaterialTextures {
            const GpuTexture* diffuse = nullptr;
            const GpuTexture* lightmap = nullptr;
            bool blend = false, alpha_test = false;
        };
        std::vector<MaterialTextures> materials;
    };

    const LoadedModel* model(const std::string& name);
    const GpuTexture* texture(const std::string& image_name);
    void add_instance(const LoadedModel& m, const glm::mat4& placement);

    const Vfs& vfs_;
    Renderer& renderer_;
    const Gol& gol_;
    Level level_;
    std::vector<DrawItem> draws_;
    std::unordered_map<std::string, LoadedModel> models_;            // by lowercase file name
    std::unordered_map<std::string, const GpuTexture*> textures_;    // by lowercase stem
    SceneStats stats_;
};

// Object placement matrix: translation * rotation (degrees, X then Y then Z) * scale.
glm::mat4 placement_matrix(const Transform& t);

}  // namespace nova
