// A loaded level: its objects resolved to GPU models and flattened into draw items.
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/animation.h"
#include "assets/collision.h"
#include "assets/irradiance.h"
#include "assets/level.h"
#include "game/physics.h"
#include "game/zones.h"
#include "core/vfs.h"
#include "render/material_system.h"
#include "render/renderer.h"

namespace nova {

struct SceneStats {
    size_t placements = 0, models = 0, missing_models = 0, textures = 0, missing_textures = 0, draws = 0;
    size_t collision_objects = 0, collision_triangles = 0, missing_collision = 0;
    size_t animated = 0, missing_animations = 0;
    size_t lod_hidden = 0;    // draws hidden because another level of detail shows
    size_t irradiance_volumes = 0, lights = 0, skyboxes = 0;
    size_t corona_controllers = 0, coronas_shown = 0;   // shown: last update
    double load_seconds = 0;
};

class Scene {
public:
    Scene(const Vfs& vfs, Renderer& renderer, const Gol& gol);

    // Load a level with the given section (bit mask; the game starts with section 1).
    void load_level(const std::string& lvc_name, uint32_t section_mask = 1);
    const std::string& level_name() const { return level_name_; }
    uint32_t section() const { return section_; }
    // Advance animations by `dt` seconds and update the draw items they drive, then
    // pick each model's level of detail for a camera at `camera`.
    void update(float dt, const glm::vec3& camera);

    const std::vector<DrawItem>& draws() const { return draws_; }
    const Level& level() const { return level_; }
    const CollisionWorld& collision() const { return collision_; }

    // Zone/portal visibility for a camera: hides objects in zones it cannot see.
    void cull(const glm::vec3& camera, const glm::mat4& view_proj);
    ZoneSystem& zones() { return zones_; }
    const ZoneSystem& zones() const { return zones_; }

    // Level objects (by index into level().objects), for game logic.
    void set_object_visible(size_t object, bool visible);
    void set_object_collision(size_t object, bool enabled) { collision_.set_owner_enabled(uint32_t(object), enabled); }
    // Debugging: hide every placement whose model name contains `substring`; returns how many.
    size_t hide_models(const std::string& substring);
    // Play a named clip of the object's animation; false if it has none.
    bool play_clip(size_t object, const std::string& clip, bool loop, bool reverse = false);
    // The object's current clip has played through (never true for looping clips).
    bool clip_finished(size_t object) const;
    // How far the object's current clip has played, 0..1 (1 if none).
    float clip_progress(size_t object) const;

    // Create an object of a gol type at runtime (spawn managers); it starts hidden.
    // Returns its index into level().objects.
    size_t spawn_object(int type, int id, const glm::vec3& position, float yaw);
    // Actors: objects that game logic moves. Place the object's model at `position`
    // turned `yaw` degrees about Z (keeping its scale), and update its bounds and lighting.
    bool set_object_pose(size_t object, const glm::vec3& position, float yaw);
    // Keep the named node's horizontal translation at the clip's first frame: the
    // animations carry root motion, and the game moves actors itself.
    bool set_root_lock(size_t object, const std::string& node);
    // Clips of the object's animation; a clip's length (seconds) and root-node speed (m/s).
    std::vector<std::string> clip_names(size_t object) const;
    float clip_length(size_t object, const std::string& clip) const;
    float clip_root_speed(size_t object, const std::string& clip) const;
    void set_clip_speed(size_t object, float speed);
    // Show or hide one of the object's nodes (by name or id) regardless of its animation.
    void set_node_visible(size_t object, const std::string& node, bool visible);
    // World position of one of the object's nodes (by name or id), e.g. Bip01_Head.
    bool node_position(size_t object, const std::string& node, glm::vec3& out) const;
    const SceneStats& stats() const { return stats_; }

    // Draw with the game's own shaders (default) or the built-in fallback shader.
    void set_game_shaders(bool on);
    bool game_shaders() const { return game_shaders_; }
    const MaterialStats& material_stats() const { return materials_->stats(); }
    const ShaderCompiler& shader_compiler() const { return materials_->compiler(); }

    // Show the skybox with this id (the level's SetSkybox script call). False if none has it.
    bool set_skybox(int id);
    // Dynamic lights (level light objects and lights inside placed models), world space.
    const std::vector<PointLight>& lights() const { return lights_; }

    // Fog from the level properties (CComponentLevelProperties).
    glm::vec4 fog_color{0, 0, 0, 1};
    float fog_start = 0, fog_end = 0, fog_density = 0;

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
        std::vector<std::vector<std::pair<glm::vec3, glm::vec3>>> bounds;   // per mesh, per primitive (local)

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
        glm::mat4 placement{1}, placement_inverse{1};
        const AnimationLibrary* library = nullptr;
        AnimClip clip;
        float speed = 1, time = 0;                     // seconds into the clip
        bool loop = true;                              // false: play once and hold the last frame (skyboxes)
        bool reverse = false;                          // play the clip backwards (door closing)
        std::vector<const std::vector<AnimTrack>*> tracks;   // per node, or null
        std::vector<glm::mat4> world;                  // per node
        std::vector<std::array<glm::mat4, kMaxJoints>> palettes;   // per skin
        struct Draw {
            size_t item;
            int node, skin;
        };
        std::vector<Draw> draws;
        // Animated material parameters, per model material, and the tracks feeding them.
        std::vector<std::vector<ParamOverride>> overrides;
        struct ValueBinding {
            const ValueTrack* track;
            int material = -1, slot = -1;   // material parameter: overrides[material][slot]
            int light = -1;                  // light colour: index into Scene::lights_
            float intensity = 1;
            int node = -1;                   // visibility: node index
        };
        std::vector<ValueBinding> values;
        std::vector<uint8_t> node_hidden;    // per node, from visibility tracks (inherited by children)
        std::vector<int8_t> node_forced;     // per node: -1 animated, 0 hidden, 1 shown (game logic)
        int root_lock = -1;                  // node whose horizontal translation is held (actors)
        glm::vec2 root_xy{0};
    };
    const AnimatedInstance* object_anim(size_t object) const;
    int find_node(const LoadedModel& m, const std::string& name) const;

    // A material whose FadeFactor follows the camera's distance from a node's XZ plane
    // (CGameObject::Update with mesh flags 4/8; fadeNear=/fadeFar= in the node properties).
    struct DistanceFade {
        std::vector<ParamOverride>* values = nullptr;   // the draws' overrides
        size_t slot = 0;                                // FadeFactor in them
        const AnimatedInstance* anim = nullptr;         // node moves with this, if animated
        int node = -1;
        glm::vec3 position{0}, axis{0, 1, 0};
        float near = 1, far = 10;
        bool fade_in = false;                           // flag 4: 0 up close; flag 8: 0 far away
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
    // Returns the instance's index into instances_.
    size_t add_instance(const LoadedModel& m, const glm::mat4& placement, const Placement* p = nullptr, bool sky = false);
    void load_skyboxes();
    void compute_object_lighting();
    const AnimationLibrary* animation(const std::string& name);
    void pose(AnimatedInstance& a);
    void select_lods(LodInstance& li, const glm::vec3& camera);
    void build_collision();

    const Vfs& vfs_;
    Renderer& renderer_;
    const Gol& gol_;
    Level level_;
    std::vector<DrawItem> draws_;
    std::vector<const GpuMaterial*> draw_materials_;   // per draw: its game-shader material (or null)
    std::vector<IrradianceVolume> irradiance_;

    // Placed instances: their draws and lighting.
    struct Instance {
        size_t first_draw = 0, draw_count = 0;
        ObjectLighting lighting;
        struct AnimatedInstance* anim = nullptr;
        int zone = ZoneSystem::kNone;
        glm::vec3 world_min{0}, world_max{0}, local_min{0}, local_max{0};   // bounds (local: placement space)
    };
    std::unordered_map<size_t, size_t> object_instances_;   // level object index -> instance
    std::deque<Instance> instances_;                    // stable addresses: draws point at .lighting
    void light_instance(Instance& inst, const glm::vec3& center);
    struct Skybox {
        int id = 0;
        size_t instance = 0;
    };
    std::vector<Skybox> skyboxes_;
    // Lights; model lights follow their node (animated instances move them).
    struct LightSource {
        const AnimatedInstance* anim = nullptr;
        int node = -1;
        glm::vec3 position{0};
    };
    std::vector<PointLight> lights_;
    std::vector<LightSource> light_sources_;
    std::unique_ptr<MaterialSystem> materials_;
    bool game_shaders_ = true;
    CollisionWorld collision_;
    ZoneSystem zones_;
    std::string level_name_;
    uint32_t section_ = 1;
    std::vector<std::unique_ptr<AnimatedInstance>> animated_;
    std::vector<LodInstance> lod_instances_;
    std::vector<DistanceFade> fades_;
    struct Corona {
        size_t draw;
        float fade;                                 // ms, 0..200
    };
    std::vector<Corona> coronas_;
    struct CoronaController {
        glm::mat4 world{1}, inverse{1};
        float min_dist = 25, max_dist = 50, min_scale = 0.5f;   // coronasMin/Max/MinScale
        float fade = 200;                                       // ms, starts visible
        std::vector<std::pair<size_t, glm::mat4>> draws;        // draw, transform relative to the controller
    };
    std::vector<CoronaController> corona_controllers_;
    std::deque<std::vector<ParamOverride>> fade_values_;   // overrides of fading draws on static instances
    std::unordered_map<std::string, std::unique_ptr<AnimationLibrary>> animations_;   // by lowercase name
    std::unordered_map<std::string, LoadedModel> models_;            // by lowercase file name
    std::unordered_map<std::string, const GpuTexture*> textures_;    // by lowercase stem
    SceneStats stats_;
};

// Object placement matrix: translation * rotation (degrees, X then Y then Z) * scale.
glm::mat4 placement_matrix(const Transform& t);

}  // namespace nova
