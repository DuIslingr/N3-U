// Walkable surfaces from the level's nav meshes: world-space triangles. Triangles connect
// through shared edges (the meshes' neighbour lists, and edges with the same end points,
// which also join meshes) and through doors (each mesh links a polygon to a door; meshes
// linking the same door connect there). Actors locate themselves on a triangle, find
// paths with A*, and pull the path tight with a funnel (string pulling) in the XY plane.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"

namespace nova {

class Navigation {
public:
    void build(const Level& level);
    bool empty() const { return tris_.empty(); }
    size_t triangle_count() const { return tris_.size(); }

    // The triangle containing `p` in XY whose surface is closest in height (within
    // `max_dz`), or -1.
    int locate(const glm::vec3& p, float max_dz = 2.5f) const;
    // The closest point on the mesh within `max_dist` of `p`; returns its triangle or -1.
    int nearest(const glm::vec3& p, float max_dist, glm::vec3& out) const;
    // Height of a triangle's surface at `xy`.
    float height(int tri, const glm::vec2& xy) const;
    // A path of points from `from` (on triangle `from_tri`) to `to` (on `to_tri`),
    // ending at `to`. False if the triangles are not connected.
    // `door_open(id)` says whether a door can be walked through.
    // `off_mesh` (if given) flags points reached through a doorway, off the mesh.
    bool find_path(int from_tri, const glm::vec3& from, int to_tri, const glm::vec3& to, std::vector<glm::vec3>& out,
                   const std::function<bool(int)>& door_open = {}, std::vector<uint8_t>* off_mesh = nullptr) const;

private:
    struct Link {
        int tri;
        int edge;                    // >= 0: across edge (v[edge], v[edge + 1]); -1: through a door
        int door = -1;
        glm::vec3 from{0}, to{0};    // door links: the doorway points on each side
    };
    struct Tri {
        int v[3];
        std::vector<Link> links;
        glm::vec3 center;
        glm::vec2 lo, hi;
    };
    void connect(int a, int b, int edge);
    bool inside(const Tri& t, const glm::vec2& p) const;
    std::vector<int> candidates(const glm::vec2& p) const;

    std::vector<glm::vec3> verts_;
    std::vector<Tri> tris_;
    static constexpr float kCell = 4.0f;
    glm::vec2 origin_{0};
    int grid_w_ = 0, grid_h_ = 0;
    std::vector<std::vector<int>> grid_;   // triangles overlapping each cell
};

}  // namespace nova
