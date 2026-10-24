#include "game/decals.h"

#include <algorithm>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include "game/physics.h"

namespace nova {

namespace {

constexpr float kTan30 = 0.57735027f, kCos30Inv = 1.1547005f;
constexpr float kSizeScale = 0.4f;   // the table's size to the triangle's side

}  // namespace

bool DecalTable::find(int material, int& cell, float& size) const {
    if (entries.empty()) return false;
    const Entry* e = &entries[0];
    for (const Entry& x : entries)
        if (x.material == material) {
            e = &x;
            break;
        }
    cell = e->cell - 1, size = e->size;
    return cell >= 0 && size > 0;
}

bool Decals::add(const CollisionWorld& world, const glm::vec3& at, const glm::vec3& into, int cell, float size) {
    if (cell < 0 || cell > 15 || size <= 0) return false;
    const float edge = size * kSizeScale, reach = kTan30 * 0.5f * edge;
    // One overlapping another is not placed.
    for (const Decal& d : decals_)
        if (glm::dot(at - d.pos, at - d.pos) < reach * reach + d.reach * d.reach) return false;
    const glm::vec3 dir = glm::length(into) > 1e-6f ? glm::normalize(into) : glm::vec3(0, 0, -1);
    // The triangles in the box around it (not those of what moves), facing the hit.
    struct Face {
        glm::vec3 a, b, c, n;
        uint32_t owner;
    };
    std::vector<Face> faces;
    std::vector<uint32_t> cand;
    const glm::vec3 lo = at - glm::vec3(reach), hi = at + glm::vec3(reach);
    world.triangles_in(lo, hi, cand);
    for (uint32_t i : cand) {
        if (world.moving(i)) continue;
        const Triangle& t = world.triangle(i);
        if (glm::any(glm::lessThan(glm::max(glm::max(t.a, t.b), t.c), lo)) ||
            glm::any(glm::greaterThan(glm::min(glm::min(t.a, t.b), t.c), hi)))
            continue;
        glm::vec3 n = glm::cross(t.b - t.a, t.c - t.a);
        if (glm::dot(n, n) < 1e-12f) continue;
        n = glm::normalize(n);
        if (glm::dot(n, dir) > 0) n = -n;
        faces.push_back({t.a, t.b, t.c, n, t.owner});
    }
    if (faces.empty()) return false;
    // cdecal::Compute: laid along the mean of the faces' normals (those within 25 degrees of
    // one already counted left out).
    glm::vec3 mean(0);
    std::vector<glm::vec3> counted;
    for (const Face& f : faces) {
        if (std::any_of(counted.begin(), counted.end(), [&](const glm::vec3& n) { return glm::dot(n, f.n) >= 0.9f; })) continue;
        counted.push_back(f.n), mean += f.n;
    }
    const glm::vec3 normal = glm::length(mean) > 1e-6f ? glm::normalize(mean) : -dir;
    const glm::quat turn = glm::rotation(glm::vec3(0, 0, 1), normal);
    const glm::vec3 ax = turn * glm::vec3(1, 0, 0), ay = turn * glm::vec3(0, 1, 0);
    const glm::vec2 corner[3] = {{-0.5f * edge, -0.5f * edge * kTan30}, {0.5f * edge, -0.5f * edge * kTan30}, {0, 0.5f * edge * kCos30Inv}};
    // Its cell: a triangle with its apex up (cdecal::getDecalUVCenter).
    const glm::vec2 centre(float(cell & 3) * 0.25f + 0.125f, 0.17f + float(cell >> 2) * 0.25f);
    auto uv = [&](const glm::vec3& p) {
        glm::vec3 d = p - at;
        d -= normal * glm::dot(d, normal);
        const float x = std::clamp(glm::dot(d, ax) / edge, -0.5f, 0.5f), y = std::clamp(glm::dot(d, ay) / edge, -0.32f, 0.68f);
        return centre + glm::vec2(x * 0.25f, y * -0.25f);
    };
    Decal decal;
    decal.pos = at, decal.reach = reach, decal.owner = faces[0].owner;
    for (const Face& f : faces) {
        // The decal's triangle on the face's plane, and the face cut to it.
        glm::vec3 p[3];
        for (int k = 0; k < 3; ++k) {
            p[k] = at + ax * corner[k].x + ay * corner[k].y;
            p[k] -= f.n * glm::dot(p[k] - f.a, f.n);
        }
        std::vector<glm::vec3> poly{f.a, f.b, f.c}, next;
        for (int k = 0; k < 3 && poly.size() >= 3; ++k) {
            const glm::vec3 e0 = p[k], e1 = p[(k + 1) % 3], inward = glm::cross(f.n, e1 - e0);
            const float side = glm::dot(inward, p[(k + 2) % 3] - e0) >= 0 ? 1.0f : -1.0f;
            next.clear();
            for (size_t i = 0; i < poly.size(); ++i) {
                const glm::vec3 &u = poly[i], &v = poly[(i + 1) % poly.size()];
                const float du = side * glm::dot(inward, u - e0), dv = side * glm::dot(inward, v - e0);
                if (du >= 0) next.push_back(u);
                if ((du >= 0) != (dv >= 0)) next.push_back(u + (v - u) * (du / (du - dv)));
            }
            poly.swap(next);
        }
        for (size_t i = 1; i + 1 < poly.size(); ++i) {
            const glm::vec3 t[3] = {poly[0], poly[i], poly[i + 1]};
            if (glm::length(glm::cross(t[1] - t[0], t[2] - t[0])) < 1e-6f) continue;
            for (const glm::vec3& v : t) decal.vertices.push_back({v, f.n, uv(v)});
        }
    }
    if (decal.vertices.empty()) return false;
    // At the most, the one nearest its end gives way.
    if (decals_.size() >= size_t(kMax))
        decals_.erase(std::min_element(decals_.begin(), decals_.end(), [](const Decal& a, const Decal& b) { return a.life < b.life; }));
    decals_.push_back(std::move(decal));
    return true;
}

void Decals::update(float dt) {
    for (Decal& d : decals_) d.life -= dt;
    std::erase_if(decals_, [](const Decal& d) { return d.life <= 0; });
}

void Decals::build(const CollisionWorld& world, const glm::vec3& eye, const GpuTexture* texture, ParticleList& out) const {
    if (!texture || decals_.empty()) return;
    ParticleBatch batch{texture, false, uint32_t(out.vertices.size()), 0};
    for (const Decal& d : decals_) {
        if (!world.owner_enabled(d.owner)) continue;
        // Lifted off the surface by 0.2% of the distance to the eye, 1 to 10 cm.
        const float lift = std::clamp(glm::distance(eye, d.pos) * 0.002f, 0.01f, 0.1f);
        for (const Vertex& v : d.vertices) out.vertices.push_back({v.pos + v.normal * lift, v.uv, 0xffffffffu});
        batch.count += uint32_t(d.vertices.size());
    }
    if (batch.count) out.batches.push_back(batch);
}

}  // namespace nova
