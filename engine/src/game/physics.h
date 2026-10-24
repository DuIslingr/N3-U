// World collision: a level's collision triangles in a grid, and ellipsoid
// collide-and-slide movement.
//
// The movement code is a port of Irrlicht's CSceneCollisionManager
// (collideEllipsoidWithWorld / collideWithWorld / testTriangleIntersection, zlib
// licence), itself based on Kasper Fauerby's "Improved Collision detection and
// Response". Glitch derives from Irrlicht, so the original game most likely moved
// its player the same way. Changes: Z is up, triangles are two-sided, and
// candidate triangles come from a uniform XY grid.
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace nova {

struct Triangle {
    glm::vec3 a, b, c;
    uint8_t material;
    uint32_t owner;           // object it belongs to, for switching its collision off
};

class CollisionWorld {
public:
    // Triangles [first, first + count) (all by default).
    void add(const std::vector<glm::vec3>& vertices, const std::vector<uint8_t>& materials, const glm::mat4& transform,
             uint32_t owner = 0, size_t first = 0, size_t count = size_t(-1));
    // Disabled owners' triangles are ignored by move() and the ray queries.
    void set_owner_enabled(uint32_t owner, bool enabled);
    // Moving owners (elevators, platforms): translate an owner's triangles by `offset` from
    // where they were added. The first call takes them out of the static grid.
    void set_owner_offset(uint32_t owner, const glm::vec3& offset);
    // A part that follows a node of its object (a .col group named after it): triangles
    // [first, first + count) of `v` in the node's space, placed at `transform`. Returns its id.
    uint32_t add_part(const std::vector<glm::vec3>& v, const std::vector<uint8_t>& m, size_t first, size_t count,
                      const glm::mat4& transform, uint32_t owner);
    // Place a part again; `motion` (applied to world points) carries what stands on it.
    void set_part_transform(uint32_t part, const glm::mat4& transform, const glm::mat4& motion = glm::mat4(1));
    // Moved by the part under `feet` (found within `reach` below them): `p` (and `yaw`,
    // degrees) by that part's last move. False if what is under them is not a part, or it
    // did not move.
    bool carry(const glm::vec3& feet, float reach, glm::vec3& p, float& yaw) const;
    bool in_part(uint32_t tri) const { return tri < part_of_.size() && part_of_[tri] != 0; }
    uint8_t material(uint32_t tri) const { return tri < tris_.size() ? tris_[tri].material : 0; }
    // On a part that moved more than a sway (2 cm) at its last placement.
    bool part_travelling(uint32_t tri) const {
        return in_part(tri) && glm::length(glm::vec3(parts_[part_of_[tri] - 1].motion[3])) > 0.02f;
    }
    void build(float cell_size = 4.0f);
    size_t triangle_count() const { return tris_.size(); }

    struct MoveResult {
        glm::vec3 position;
        bool falling = true;      // the gravity sweep hit nothing
        bool hit = false;         // the movement sweep touched something
    };
    // Move an ellipsoid (centre `position`, radii `radius`) by `velocity`, sliding along
    // what it touches, then by `gravity` (both are displacements for this step).
    // `one_sided`: the movement passes through triangles it meets from behind.
    MoveResult move(const glm::vec3& position, const glm::vec3& radius, const glm::vec3& velocity,
                    const glm::vec3& gravity, float sliding = 0.0005f, bool one_sided = false) const;

    // Nearest hit of a downward ray from `from`, within `max_dist`; returns false if none.
    // `owner` (if given) receives the hit triangle's owner.
    bool ray_down(const glm::vec3& from, float max_dist, float& hit_z, uint32_t* owner = nullptr, uint32_t* tri = nullptr) const;
    // A sphere at `p` pushed out of the triangles it overlaps; false if it overlapped none.
    // `normal` (if given) receives the direction of the last push.
    // `from` (if given): where it was before; a triangle it crossed pushes it back to that side.
    bool push_sphere(glm::vec3& p, float radius, glm::vec3* normal = nullptr, const glm::vec3* from = nullptr) const;
    // Owners of triangles with a vertex within `radius` of `p` (diagnostics).
    std::vector<uint32_t> owners_near(const glm::vec3& p, float radius) const;
    // First hit along the segment from -> to, as a fraction of its length; false if none.
    // `material` / `normal` (if given) receive the hit triangle's material byte and its
    // unit normal facing the ray's start; `owner` its owner. `shot`: through the invisible
    // walls that only hold the player (materials 18 and 19).
    bool raycast(const glm::vec3& from, const glm::vec3& to, float& t, uint8_t* material = nullptr,
                 glm::vec3* normal = nullptr, uint32_t* owner = nullptr, bool shot = false) const;

private:
    struct Data;
    glm::vec3 collide(int depth, Data& d, const glm::vec3& pos, const glm::vec3& vel) const;
    void candidates(const glm::vec3& lo, const glm::vec3& hi, std::vector<uint32_t>& out) const;

    std::vector<Triangle> tris_;
    std::vector<uint8_t> owner_disabled_;   // by owner
    struct Mover {
        std::vector<uint32_t> tris;
        std::vector<std::array<glm::vec3, 3>> base;   // as added
        glm::vec3 lo{0}, hi{0};                      // current bounds
    };
    std::unordered_map<uint32_t, Mover> movers_;
    struct Part : Mover {
        glm::mat4 motion{1};   // its last move, for what stands on it
    };
    std::vector<Part> parts_;
    std::vector<uint32_t> part_of_;          // by triangle: its part + 1, 0 if static
    float cell_ = 4.0f;
    std::unordered_map<int64_t, std::vector<uint32_t>> grid_;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t epoch_ = 0;
};

}  // namespace nova
