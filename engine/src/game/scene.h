// A loaded level: its objects resolved to GPU models and flattened into draw items.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/animation.h"
#include "assets/collision.h"
#include "assets/level.h"
#include "game/physics.h"
#include "core/vfs.h"
#include "render/renderer.h"

namespace nova {

struct SceneStats {
    size_t placements = 0, models = 0, missing_models = 0, textures = 0, missing_textures = 0, draws = 0;
    size_t collision_objects = 0, collision_triangles = 0, missing_collision = 0;
    size_t animated = 0, missing_animations = 0;
    size_t lod_hidden = 0;    // draws hidden because another level of detail shows
    double load_seconds = 0;
};

class Scene {
public:
    Scene(const Vfs& vfs, Renderer& renderer, const Gol& gol);

    void load_level(const std::string& lvc_name);
    // Advance animations by `dt` seconds and update the draw items they drive, then
    // pick each model's level of detail for a camera at `camera`.
    void update(float dt, const glm::vec3& camera);

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
        std::vector<int> parent;                       // per node, -1 for roots (parents come first)
        std::vector<std::vector<int>> joint_nodes;     // per skin, per joint: node index or -1

        // Levels of detail (the game's CColladaRootLODSceneNode): nodes named <set>LOD<n>
        // or <set>LODD; one level of each set is shown, chosen by camera distance.
        static constexpr int kLodLevels = 10;
        struct LodSet {
            std::string name;
            std::array<int, kLodLevels> nodes;         // node per level, -1 = nothing shown
            std::array<float, kLodLevels> min_sq, max_sq;   // squared camera distance range per level
            bool fixed = true;                         // level 0 at every distance
        };
        std::vector<LodSet> lod_sets;
        std::vector<int> lod_set;                      // per node: its set, or -1
    };

    // A placed model whose nodes move: an animation clip, and/or skinned meshes.
    struct AnimatedInstance {
        const LoadedModel* model = nullptr;
        glm::mat4 placement{1};
        const AnimationLibrary* library = nullptr;
        AnimClip clip;
        float speed = 1, time = 0;                     // seconds into the clip
        std::vector<const std::vector<AnimTrack>*> tracks;   // per node, or null
        std::vector<glm::mat4> world;                  // per node
        std::vector<std::array<glm::mat4, kMaxJoints>> palettes;   // per skin
        struct Draw {
            size_t item;
            int node, skin;
        };
        std::vector<Draw> draws;
    };

    // A placed model with distance-dependent LOD sets.
    struct LodInstance {
        const LoadedModel* model = nullptr;
        const AnimatedInstance* anim = nullptr;        // node positions come from here if animated
        std::vector<glm::vec3> node_pos;               // otherwise: world position per node
        std::vector<int> level;                        // current level per set
        struct Draw {
            size_t item;
            std::vector<int> lod_nodes;                // LOD nodes on the path from the root
        };
        std::vector<Draw> draws;
    };

    const LoadedModel* model(const std::string& name);
    static void build_lod_sets(LoadedModel& lm);
    const GpuTexture* texture(const std::string& image_name);
    void add_instance(const LoadedModel& m, const glm::mat4& placement, const Placement* p = nullptr);
    const AnimationLibrary* animation(const std::string& name);
    void pose(AnimatedInstance& a);
    void select_lods(LodInstance& li, const glm::vec3& camera);
    void build_collision();

    const Vfs& vfs_;
    Renderer& renderer_;
    const Gol& gol_;
    Level level_;
    std::vector<DrawItem> draws_;
    CollisionWorld collision_;
    std::vector<std::unique_ptr<AnimatedInstance>> animated_;
    std::vector<LodInstance> lod_instances_;
    std::unordered_map<std::string, std::unique_ptr<AnimationLibrary>> animations_;   // by lowercase name
    std::unordered_map<std::string, LoadedModel> models_;            // by lowercase file name
    std::unordered_map<std::string, const GpuTexture*> textures_;    // by lowercase stem
    SceneStats stats_;
};

// Object placement matrix: translation * rotation (degrees, X then Y then Z) * scale.
glm::mat4 placement_matrix(const Transform& t);

}  // namespace nova
