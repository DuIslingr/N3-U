// Collision geometry (.col files). See tools/col.py.
//
// A .col is a DICT stream: u32 group count; per group a name, u32 triangle count,
// and per triangle 9 big-endian floats (three vertices) plus a material byte.
// CCollisionComponent names "<model>.bdae" and the game loads "<model>.col"; when
// there is no .col, the named model's own triangles are used instead.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "core/bytes.h"
#include "core/vfs.h"

namespace nova {

constexpr uint8_t kMaterialNone = 255;           // triangles taken from a model
inline bool is_invisible_material(uint8_t m) {   // CPhysicsManager::isColMatInvisible
    return m == 18 || m == 19;
}

struct CollisionMesh {
    std::vector<glm::vec3> vertices;   // 3 per triangle, in the model's space (a part's: its node's)
    std::vector<uint8_t> materials;    // 1 per triangle
    // A .col's groups: its name and its triangles. Those named after a node of the object's
    // model (collisionPart_*) are in that node's space and move with it.
    struct Group {
        std::string name;
        size_t first = 0, count = 0;
    };
    std::vector<Group> groups;
    size_t triangle_count() const { return materials.size(); }
};

CollisionMesh load_col(const Bytes& data);

// Resolve a collision component's model name the way the game does.
std::optional<CollisionMesh> load_collision(const Vfs& vfs, const std::string& name);

}  // namespace nova
