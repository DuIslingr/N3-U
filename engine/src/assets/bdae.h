// BDAE models (Glitch "BRES" resource images). See tools/bdae.py for the format.
//
// A BDAE is a memory image of glitch::collada structures with pointers stored as
// file offsets (listed in a relocation table). This loader extracts what the
// renderer needs: meshes (static vertex data), materials (texture names, effect)
// and the node hierarchy of the first visual scene.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/bytes.h"

namespace nova {

struct Vertex {
    glm::vec3 position{0};
    glm::vec3 normal{0, 0, 1};
    glm::vec2 uv0{0};
    glm::vec2 uv1{0};
    glm::vec2 uv2{0};
    glm::vec3 tangent{0};
    glm::vec3 binormal{0};
    uint32_t color = 0xFFFFFFFF;     // RGBA8, R in the low byte; white when the mesh has none (sky spheres need it)
    uint32_t joints = 0;             // 4 x u8 joint indices (skinned meshes)
    uint32_t weights = 0x000000FF;   // 4 x unorm8 weights, summing to 255
};

constexpr int kMaxJoints = 128;      // largest skin in the game has 89

struct Skin {
    std::vector<std::string> joints;            // node sids (or ids)
    std::vector<glm::mat4> inverse_bind;        // per joint; already includes the bind-shape matrix
};

struct Primitive {
    int material = -1;               // index into Model::materials
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;   // triangle list
};

struct Mesh {
    std::string id;
    int skin = -1;                   // index into Model::skins, or -1 for static
    std::vector<Primitive> primitives;
};

// A material's value for one effect parameter.
struct MaterialParam {
    std::string name;
    uint32_t type = 0;               // ParamType (assets/effect.h)
    std::vector<float> value;        // numeric parameters
    std::string texture;             // sampler parameters: image file name
};

struct Material {
    std::string id;
    std::string effect;              // e.g. NV_LightMap.bdae
    std::string technique;           // from the "<effect>-profile_GLES2/CurrentTechnique" parameter
    std::vector<MaterialParam> params;
    std::string diffuse;             // image file names (as referenced, usually .tga)
    std::string lightmap;
    std::string normal;
    bool blend = false;              // effect draws translucent/additive
    bool vertex_color = false;       // effect multiplies by vertex colour
};

// A light from the model's light library (glitch::collada::SLight).
struct ModelLight {
    std::string id;
    int type = 0;                    // 0 ambient, 1 point, 2 spot
    glm::vec4 color{1};              // RGBA
    float intensity = 1;
    float falloff_start = 0, falloff_end = 0;
};

struct ModelNode {
    std::string id, name, sid;
    glm::vec3 translation{0};
    glm::quat rotation{1, 0, 0, 0};  // glm order: w, x, y, z
    glm::vec3 scale{1};
    std::string properties;          // max user properties, e.g. "lod_dist=30"
    std::vector<int> children;
    std::vector<int> meshes;         // indices into Model::meshes drawn at this node
    std::vector<int> lights;         // indices into Model::lights placed at this node
    float camera_fov = 0;            // a camera at this node: its field of view (radians); 0: none
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Skin> skins;
    std::vector<Material> materials;
    std::vector<ModelLight> lights;
    std::vector<ModelNode> nodes;
    std::vector<int> roots;
};

// Parse a .bdae (or a ZIP-wrapped effect .bdae). Throws FormatError.
Model load_bdae(const Bytes& data);

}  // namespace nova
