// Decals (the original's decals_manager / cdecal): bullet holes, glass cracks and scorch
// marks left where projectiles hit the world. Each is an equilateral triangle of the hit's
// size, laid on the world's triangles around the hit and cut to them, textured with one
// triangular cell of FX_decals_bullets.tga (4 x 4; NV_DiffuseVC "AlphaCull"). They last
// 40 s; at most 50 (the high-end GPU profiles' "Max Decals"), the one nearest its end
// giving way; one overlapping another is not placed.
#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "render/renderer.h"

namespace nova {

class CollisionWorld;
struct GpuTexture;

// A projectile's or grenade's decal per collision material (CContainerHitCollisionDecal:
// {material, cell (1-based), size}); the first for a material it lacks.
struct DecalTable {
    struct Entry {
        int material = 0, cell = -1;
        float size = 0;
    };
    std::vector<Entry> entries;
    // GetDecalProps: false if it has none.
    bool find(int material, int& cell, float& size) const;
};

class Decals {
public:
    static constexpr int kMax = 50;
    static constexpr float kLife = 40.0f;   // seconds

    void clear() { decals_.clear(); }
    // decals_manager::addDecal: at `at`, laid along `into` (towards the surface), cell
    // 0..15, `size` (the table's). False if nothing took it.
    bool add(const CollisionWorld& world, const glm::vec3& at, const glm::vec3& into, int cell, float size);
    void update(float dt);
    // Their triangles, lifted off their surfaces by the distance to `eye` (cdecal::Update);
    // not those on what has had its collision switched off (broken glass).
    void build(const CollisionWorld& world, const glm::vec3& eye, const GpuTexture* texture, ParticleList& out) const;
    size_t count() const { return decals_.size(); }

private:
    struct Vertex {
        glm::vec3 pos, normal;
        glm::vec2 uv;
    };
    struct Decal {
        glm::vec3 pos{0};
        float reach = 0;   // its incircle's radius
        float life = kLife;
        uint32_t owner = 0;             // what it lies on (its first surface's)
        std::vector<Vertex> vertices;   // triangles
    };
    std::vector<Decal> decals_;
};

}  // namespace nova
