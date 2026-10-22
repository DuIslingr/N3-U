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
    uint32_t color = 0xFFFFFFFF;     // RGBA8, R in the low byte
};

struct Primitive {
    int material = -1;               // index into Model::materials
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;   // triangle list
};

struct Mesh {
    std::string id;
    std::vector<Primitive> primitives;
};

struct Material {
    std::string id;
    std::string effect;              // e.g. NV_LightMap.bdae
    std::string diffuse;             // image file names (as referenced, usually .tga)
    std::string lightmap;
    std::string normal;
    bool blend = false;              // effect draws translucent/additive
    bool vertex_color = false;       // effect multiplies by vertex colour
};

struct ModelNode {
    std::string id, name, sid;
    glm::vec3 translation{0};
    glm::quat rotation{1, 0, 0, 0};  // glm order: w, x, y, z
    glm::vec3 scale{1};
    std::vector<int> children;
    std::vector<int> meshes;         // indices into Model::meshes drawn at this node
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<ModelNode> nodes;
    std::vector<int> roots;
};

// Parse a .bdae (or a ZIP-wrapped effect .bdae). Throws FormatError.
Model load_bdae(const Bytes& data);

}  // namespace nova
