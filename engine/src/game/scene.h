// A loaded level: its objects resolved to GPU models and flattened into draw items.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "assets/collision.h"
#include "assets/level.h"
#include "game/physics.h"
#include "core/vfs.h"
#include "render/renderer.h"

namespace nova {

struct SceneStats {
    size_t placements = 0, models = 0, missing_models = 0, textures = 0, missing_textures = 0, draws = 0;
    size_t collision_objects = 0, collision_triangles = 0, missing_collision = 0;
    double load_seconds = 0;
};

class Scene {
public:
    Scene(const Vfs& vfs, Renderer& renderer, const Gol& gol);

    void load_level(const std::string& lvc_name);

    const std::vector<DrawItem>& draws() const { return draws_; }
    const Level& level() const { return level_; }
    const CollisionWorld& collision() const { return collision_; }

    // Doors are opened by triggers and scripts in the game; until those exist, leave
    // their collision out so the level can be explored.
    bool doors_open = true;
    const SceneStats& stats() const { return stats_; }

    // Where the player starts: the level's start spawn point (floor level).
    glm::vec3 start_position{0, 0, 0};
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
    void build_collision();

    const Vfs& vfs_;
    Renderer& renderer_;
    const Gol& gol_;
    Level level_;
    std::vector<DrawItem> draws_;
    CollisionWorld collision_;
    std::unordered_map<std::string, LoadedModel> models_;            // by lowercase file name
    std::unordered_map<std::string, const GpuTexture*> textures_;    // by lowercase stem
    SceneStats stats_;
};

// Object placement matrix: translation * rotation (degrees, X then Y then Z) * scale.
glm::mat4 placement_matrix(const Transform& t);

}  // namespace nova
