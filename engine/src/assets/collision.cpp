#include "assets/collision.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/bdae.h"
#include "assets/dict_stream.h"

namespace nova {

CollisionMesh load_col(const Bytes& data) {
    DictStream st(data);
    CollisionMesh mesh;
    const int groups = st.i();
    for (int g = 0; g < groups; ++g) {
        st.s();   // group name
        const int n = st.i();
        if (n < 0 || st.pos + size_t(n) * 37 > st.end) throw FormatError("bad .col triangle count");
        for (int t = 0; t < n; ++t) {
            for (int v = 0; v < 3; ++v) mesh.vertices.push_back({st.f(), st.f(), st.f()});
            mesh.materials.push_back(uint8_t(st.c()));
        }
    }
    if (st.pos != st.end) throw FormatError("trailing data after the last .col group");
    return mesh;
}

namespace {

// All triangles of a model, with its node transforms applied (CCollisionComponent's fallback).
CollisionMesh model_triangles(const Model& model) {
    CollisionMesh out;
    std::vector<std::pair<int, glm::mat4>> stack;
    for (int r : model.roots) stack.emplace_back(r, glm::mat4(1));
    while (!stack.empty()) {
        auto [ni, parent] = stack.back();
        stack.pop_back();
        const ModelNode& node = model.nodes[ni];
        glm::mat4 local = glm::translate(glm::mat4(1), node.translation) * glm::mat4_cast(node.rotation);
        const glm::mat4 world = parent * glm::scale(local, node.scale);
        for (int m : node.meshes) {
            for (const Primitive& p : model.meshes[m].primitives) {
                for (uint32_t idx : p.indices) out.vertices.push_back(glm::vec3(world * glm::vec4(p.vertices[idx].position, 1)));
                out.materials.insert(out.materials.end(), p.indices.size() / 3, kMaterialNone);
            }
        }
        for (int c : node.children) stack.emplace_back(c, world);
    }
    return out;
}

}  // namespace

std::optional<CollisionMesh> load_collision(const Vfs& vfs, const std::string& name) {
    std::string stem = to_lower(name);
    if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".bdae") == 0) stem.erase(stem.size() - 5);
    if (const auto& hits = vfs.find(stem + ".col"); !hits.empty()) return load_col(vfs.read(hits.front()));
    if (const auto& hits = vfs.find(stem + ".bdae"); !hits.empty()) return model_triangles(load_bdae(vfs.read(hits.front())));
    return std::nullopt;
}

}  // namespace nova
