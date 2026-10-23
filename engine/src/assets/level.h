// Levels (.lvc) and the game object library (gol.bin). See tools/lvc.py for the format.
//
// A level is a stream of objects: u32 type, then exactly the fields the game's
// loader reads for that type (no lengths). Level-structure types (zones, portals,
// nav data, spawn points...) have dedicated readers; any other type is a game
// object whose per-instance component data follows the type's component list in
// gol.bin. Component readers come from the generated specs in level_specs.inc.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/dict_stream.h"

namespace nova {

constexpr int kCompBase = 0x00;       // CComponentBase: id, position, rotation (degrees), scale, 2 flags
constexpr int kCompMesh = 0x01;       // CComponentMesh: model name, int, 11 flags
constexpr int kCompAnimation = 0x0C;  // CAnimationComponent: s(animation file) s(default clip) f(speed) ...
constexpr int kCompCollision = 0x19;  // CCollisionComponent: c c c c c s(collision model) [s i] i(triangles)
constexpr int kCompLight = 0x88;      // CLightComponent: c c c c (RGBA) f(intensity) f(radius) i i f c(fake)

struct Transform {
    int id = 0;
    glm::vec3 position{0};
    glm::vec3 rotation{0};   // degrees, applied X then Y then Z (Irrlicht setRotationDegrees)
    glm::vec3 scale{1};
    bool visible = true;     // CComponentBase flag: shown (and enabled) when the level starts
};

struct GolType {
    int id = 0;
    std::string name;
    std::vector<int> parents;
    std::vector<std::pair<int, int>> components;   // (component id, flag); flag 0 = stored per instance
    std::map<int, Value> templates;                // component id -> template values
    bool templates_ok = false;
};

using Gol = std::unordered_map<int, GolType>;

struct NavMesh {
    std::vector<glm::vec3> vertices;                // world space
    std::vector<std::array<int16_t, 8>> polygons;   // 3 vertex indices, 3 neighbours, 2 flags
    // Links to other nav meshes through doors: polygon, a point in the doorway, the door's id.
    struct Link {
        int polygon = -1;
        glm::vec3 point{0};
        int door = -1;
    };
    std::vector<Link> links;
};

struct LevelObject {
    size_t offset = 0;
    size_t content_end = 0;    // zones / meta zones: end of the records the game skips with them
    int type = 0;
    std::string kind;          // "object", "zone", "nav_mesh", "spawn_point", ...
    std::string name;          // object type name from gol.bin
    std::optional<Transform> base;
    std::string model;         // per-instance mesh component model
    uint32_t mesh_flags = 0;   // its flags (4: fade in, 8: fade out with distance; CGameObject::InitComponentMesh)
    std::map<int, Value> components;
    Value fields;              // structure-type fields
    NavMesh nav;
};

struct Level {
    int header_count = 0;
    std::vector<LevelObject> objects;
    std::vector<std::string> strings;
};

Gol load_gol(const Bytes& data);
Level load_level(const Bytes& data, const Gol& gol);

// Levels are split into sections (bit masks on zone and meta zone records). The game
// loads one section at a time (LoadLevelSections reloads with another) and skips every
// zone whose mask misses it, with all the objects up to the next zone record. Keep only
// what the section `section_mask` loads.
void keep_section(Level& level, uint32_t section_mask);

// Components including inherited ones (parents first, the type's own entries win).
std::vector<std::pair<int, int>> resolved_components(const GolType& type, const Gol& gol);
// Template values for `comp` from the type or its nearest parent; nullptr if none.
const Value* resolved_template(const GolType& type, const Gol& gol, int comp);

struct Placement {
    size_t object_index = 0;   // into Level::objects
    std::string model;
    uint32_t mesh_flags = 0;   // LevelObject::mesh_flags, or the template's
    Transform transform;
    std::string object;
    std::string anim_file;     // from the animation component, if any
    std::string anim_clip;
    float anim_speed = 1.0f;
};

// Visible models placed in the level: per-instance meshes, or the type's template mesh.
std::vector<Placement> model_placements(const Level& level, const Gol& gol, bool library = true);
// Collision components: collision model name (.bdae; the game looks for .col) per object.
std::vector<Placement> collision_placements(const Level& level, const Gol& gol);

Transform transform_from_values(const Value& v, size_t first = 0);

// Skybox records (level type 0x3D): the model, the id scripts pass to SetSkybox, and
// whether it is shown when the level starts.
struct SkyboxRef {
    std::string model;
    int id = 0;
    bool initial = false;
};
std::vector<SkyboxRef> level_skyboxes(const Level& level);

// Objects with a light component (CLightComponent::Load).
struct LevelLight {
    std::string object;
    glm::vec3 position{0};
    glm::vec4 color{1};       // RGBA, bytes in that order
    float intensity = 1, radius = 1;
    bool fake = false;        // registered with its zone as a "fake light"
};
std::vector<LevelLight> level_lights(const Level& level, const Gol& gol);

}  // namespace nova
