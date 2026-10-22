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

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace nova {

struct Triangle {
    glm::vec3 a, b, c;
    uint8_t material;
};

class CollisionWorld {
public:
    void add(const std::vector<glm::vec3>& vertices, const std::vector<uint8_t>& materials, const glm::mat4& transform);
    void build(float cell_size = 4.0f);
    size_t triangle_count() const { return tris_.size(); }

    struct MoveResult {
        glm::vec3 position;
        bool falling = true;      // the gravity sweep hit nothing
        bool hit = false;         // the movement sweep touched something
    };
    // Move an ellipsoid (centre `position`, radii `radius`) by `velocity`, sliding along
    // what it touches, then by `gravity` (both are displacements for this step).
    MoveResult move(const glm::vec3& position, const glm::vec3& radius, const glm::vec3& velocity,
                    const glm::vec3& gravity, float sliding = 0.0005f) const;

    // Nearest hit of a downward ray from `from`, within `max_dist`; returns false if none.
    bool ray_down(const glm::vec3& from, float max_dist, float& hit_z) const;

private:
    struct Data;
    glm::vec3 collide(int depth, Data& d, const glm::vec3& pos, const glm::vec3& vel) const;
    void candidates(const glm::vec3& lo, const glm::vec3& hi, std::vector<uint32_t>& out) const;

    std::vector<Triangle> tris_;
    float cell_ = 4.0f;
    std::unordered_map<int64_t, std::vector<uint32_t>> grid_;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t epoch_ = 0;
};

}  // namespace nova
