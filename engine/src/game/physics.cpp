#include "game/physics.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace nova {

namespace {

inline int64_t cell_key(int x, int y) { return (int64_t(x) << 32) ^ uint32_t(y); }

// Lowest root of a*t^2 + b*t + c in (0, max_r) (Irrlicht getLowestRoot).
bool lowest_root(float a, float b, float c, float max_r, float* root) {
    const float det = b * b - 4.0f * a * c;
    if (det < 0.0f || a == 0.0f) return false;
    const float s = std::sqrt(det);
    float r1 = (-b - s) / (2 * a), r2 = (-b + s) / (2 * a);
    if (r1 > r2) std::swap(r1, r2);
    if (r1 > 0 && r1 < max_r) return (*root = r1), true;
    if (r2 > 0 && r2 < max_r) return (*root = r2), true;
    return false;
}

// Point-in-triangle by same-side tests (Irrlicht triangle3d::isPointInside).
bool point_in_triangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    auto same_side = [](const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& x, const glm::vec3& y) {
        const glm::vec3 bminusa = y - x;
        return glm::dot(glm::cross(bminusa, p1 - x), glm::cross(bminusa, p2 - x)) >= 0.0f;
    };
    return same_side(p, a, b, c) && same_side(p, b, a, c) && same_side(p, c, a, b);
}

}  // namespace

struct CollisionWorld::Data {
    glm::vec3 eradius;
    glm::vec3 r3_position, r3_velocity;   // world space, for the candidate query box
    glm::vec3 velocity, normalized_velocity, base_point;
    bool found = false;
    float nearest = FLT_MAX;
    glm::vec3 intersection;
    float sliding = 0.0005f;
    int hits = 0;
    std::vector<uint32_t> cand;
};

void CollisionWorld::add(const std::vector<glm::vec3>& v, const std::vector<uint8_t>& m, const glm::mat4& tf,
                         uint32_t owner) {
    for (size_t t = 0; t < m.size(); ++t) {
        tris_.push_back({glm::vec3(tf * glm::vec4(v[3 * t], 1)), glm::vec3(tf * glm::vec4(v[3 * t + 1], 1)),
                         glm::vec3(tf * glm::vec4(v[3 * t + 2], 1)), m[t], owner});
    }
}

void CollisionWorld::set_owner_enabled(uint32_t owner, bool enabled) {
    if (owner >= owner_disabled_.size()) {
        if (enabled) return;
        owner_disabled_.resize(owner + 1, 0);
    }
    owner_disabled_[owner] = !enabled;
}

void CollisionWorld::build(float cell_size) {
    cell_ = cell_size;
    grid_.clear();
    for (uint32_t i = 0; i < tris_.size(); ++i) {
        const Triangle& t = tris_[i];
        const glm::vec3 lo = glm::min(glm::min(t.a, t.b), t.c), hi = glm::max(glm::max(t.a, t.b), t.c);
        const int x0 = int(std::floor(lo.x / cell_)), x1 = int(std::floor(hi.x / cell_));
        const int y0 = int(std::floor(lo.y / cell_)), y1 = int(std::floor(hi.y / cell_));
        if ((int64_t(x1) - x0 + 1) * (int64_t(y1) - y0 + 1) > 4096) continue;   // degenerate giant triangle
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) grid_[cell_key(x, y)].push_back(i);
    }
    stamp_.assign(tris_.size(), 0);
}

void CollisionWorld::candidates(const glm::vec3& lo, const glm::vec3& hi, std::vector<uint32_t>& out) const {
    out.clear();
    if (++epoch_ == 0) std::fill(stamp_.begin(), stamp_.end(), 0), epoch_ = 1;
    const int x0 = int(std::floor(lo.x / cell_)), x1 = int(std::floor(hi.x / cell_));
    const int y0 = int(std::floor(lo.y / cell_)), y1 = int(std::floor(hi.y / cell_));
    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            auto it = grid_.find(cell_key(x, y));
            if (it == grid_.end()) continue;
            for (uint32_t i : it->second) {
                if (stamp_[i] == epoch_) continue;
                stamp_[i] = epoch_;
                const Triangle& t = tris_[i];
                if (t.owner < owner_disabled_.size() && owner_disabled_[t.owner]) continue;
                const glm::vec3 tlo = glm::min(glm::min(t.a, t.b), t.c), thi = glm::max(glm::max(t.a, t.b), t.c);
                if (thi.z < lo.z || tlo.z > hi.z) continue;
                out.push_back(i);
            }
        }
    }
}

glm::vec3 CollisionWorld::collide(int depth, Data& d, const glm::vec3& pos, const glm::vec3& vel) const {
    const float very_close = d.sliding;
    if (depth > 5) return pos;
    d.velocity = vel;
    d.normalized_velocity = glm::length(vel) > 0 ? glm::normalize(vel) : vel;
    d.base_point = pos;
    d.found = false;
    d.nearest = FLT_MAX;

    // Candidate triangles around the swept volume (world space), tested in ellipsoid space.
    const glm::vec3 world_pos = pos * d.eradius, world_end = (pos + vel) * d.eradius;
    candidates(glm::min(world_pos, world_end) - d.eradius, glm::max(world_pos, world_end) + d.eradius, d.cand);
    for (uint32_t i : d.cand) {
        const Triangle& wt = tris_[i];
        glm::vec3 A = wt.a / d.eradius, B = wt.b / d.eradius, C = wt.c / d.eradius;
        glm::vec3 n = glm::cross(B - A, C - A);
        const float nlen = glm::length(n);
        if (nlen < 1e-12f) continue;
        n /= nlen;
        // Two-sided: test the side facing the movement (Irrlicht only tests front faces).
        if (glm::dot(n, d.normalized_velocity) > 0) n = -n;
        const float plane_d = -glm::dot(n, A);

        float t0, t1;
        bool embedded = false;
        const float signed_dist = glm::dot(n, d.base_point) + plane_d;
        float n_dot_v = glm::dot(n, d.velocity);
        if (std::fabs(n_dot_v) < 1e-6f) {
            if (std::fabs(signed_dist) >= 1.0f) continue;
            embedded = true;
            t0 = 0;
            t1 = 1;
        } else {
            n_dot_v = 1.0f / n_dot_v;
            t0 = (-1.f - signed_dist) * n_dot_v;
            t1 = (1.f - signed_dist) * n_dot_v;
            if (t0 > t1) std::swap(t0, t1);
            if (t0 > 1.0f || t1 < 0.0f) continue;
            t0 = std::clamp(t0, 0.f, 1.f);
            t1 = std::clamp(t1, 0.f, 1.f);
        }
        (void)t1;

        glm::vec3 point(0);
        bool found = false;
        float t = 1.0f;
        if (!embedded) {
            const glm::vec3 plane_point = (d.base_point - n) + d.velocity * t0;
            if (point_in_triangle(plane_point, A, B, C)) {
                found = true;
                t = t0;
                point = plane_point;
            }
        }
        if (!found) {
            const glm::vec3 v = d.velocity, base = d.base_point;
            const float v2 = glm::dot(v, v);
            float nt;
            for (const glm::vec3* p : {&A, &B, &C}) {   // vertices
                const float b = 2.0f * glm::dot(v, base - *p);
                const float c = glm::dot(*p - base, *p - base) - 1.f;
                if (lowest_root(v2, b, c, t, &nt)) {
                    t = nt;
                    found = true;
                    point = *p;
                }
            }
            const glm::vec3* edges[3][2] = {{&A, &B}, {&B, &C}, {&C, &A}};
            for (auto& e : edges) {   // edges
                const glm::vec3 edge = *e[1] - *e[0], btv = *e[0] - base;
                const float e2 = glm::dot(edge, edge), edv = glm::dot(edge, v), edb = glm::dot(edge, btv);
                const float a = e2 * -v2 + edv * edv;
                const float b = e2 * (2.f * glm::dot(v, btv)) - 2.0f * edv * edb;
                const float c = e2 * (1.f - glm::dot(btv, btv)) + edb * edb;
                if (lowest_root(a, b, c, t, &nt)) {
                    const float f = (edv * nt - edb) / e2;
                    if (f >= 0.0f && f <= 1.0f) {
                        t = nt;
                        found = true;
                        point = *e[0] + edge * f;
                    }
                }
            }
        }
        if (found) {
            ++d.hits;
            const float dist = t * glm::length(d.velocity);
            if (!d.found || dist < d.nearest) {
                d.nearest = dist;
                d.intersection = point;
                d.found = true;
            }
        }
    }

    if (!d.found) return pos + vel;

    const glm::vec3 destination = pos + vel;
    glm::vec3 new_base = pos;
    if (d.nearest >= very_close) {
        const glm::vec3 dir = glm::normalize(vel);
        new_base = d.base_point + dir * (d.nearest - very_close);
        d.intersection -= dir * very_close;
    } else {
        // Already touching (or a hair inside): Irrlicht keeps the position, so a surface
        // touched exactly blocks every later move. Restore the gap along the contact normal.
        const glm::vec3 away = d.base_point - d.intersection;
        if (glm::length(away) > 1e-6f) new_base = d.intersection + glm::normalize(away) * (1.0f + very_close);
    }
    const glm::vec3 slide_origin = d.intersection;
    const glm::vec3 diff = new_base - d.intersection;
    if (glm::length(diff) < 1e-9f) return new_base;
    const glm::vec3 slide_normal = glm::normalize(diff);
    const float dist_to_plane = glm::dot(slide_normal, destination) - glm::dot(slide_normal, slide_origin);
    const glm::vec3 new_destination = destination - slide_normal * dist_to_plane;
    const glm::vec3 new_velocity = new_destination - d.intersection;
    if (glm::length(new_velocity) < very_close) return new_base;
    return collide(depth + 1, d, new_base, new_velocity);
}

CollisionWorld::MoveResult CollisionWorld::move(const glm::vec3& position, const glm::vec3& radius, const glm::vec3& velocity,
                                                const glm::vec3& gravity, float sliding) const {
    Data d;
    d.eradius = radius;
    d.sliding = sliding;
    MoveResult r;
    glm::vec3 p = collide(0, d, position / radius, velocity / radius);
    r.hit = d.hits > 0;
    r.falling = false;
    if (gravity != glm::vec3(0)) {
        d.hits = 0;
        p = collide(0, d, p, gravity / radius);
        r.falling = d.hits == 0;
    }
    r.position = p * radius;
    return r;
}

bool CollisionWorld::ray_down(const glm::vec3& from, float max_dist, float& hit_z) const {
    std::vector<uint32_t> cand;
    candidates(from - glm::vec3(0.01f, 0.01f, max_dist), from + glm::vec3(0.01f, 0.01f, 0), cand);
    bool any = false;
    float best = from.z - max_dist;
    for (uint32_t i : cand) {
        const Triangle& t = tris_[i];
        // Barycentric test in XY, then the triangle's height at that point.
        const glm::vec2 p(from), a(t.a), b(t.b), c(t.c);
        const float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(den) < 1e-9f) continue;
        const float w1 = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / den;
        const float w2 = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / den;
        const float w3 = 1 - w1 - w2;
        if (w1 < 0 || w2 < 0 || w3 < 0) continue;
        const float z = w1 * t.a.z + w2 * t.b.z + w3 * t.c.z;
        if (z <= from.z && z >= best) {
            best = z;
            any = true;
        }
    }
    hit_z = best;
    return any;
}

bool CollisionWorld::raycast(const glm::vec3& from, const glm::vec3& to, float& t) const {
    std::vector<uint32_t> cand;
    candidates(glm::min(from, to), glm::max(from, to), cand);
    const glm::vec3 dir = to - from;
    bool any = false;
    t = 1;
    for (uint32_t i : cand) {
        // Moller-Trumbore, two-sided.
        const Triangle& tri = tris_[i];
        const glm::vec3 e1 = tri.b - tri.a, e2 = tri.c - tri.a;
        const glm::vec3 p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-12f) continue;
        const float inv = 1 / det;
        const glm::vec3 s = from - tri.a;
        const float u = glm::dot(s, p) * inv;
        if (u < 0 || u > 1) continue;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(dir, q) * inv;
        if (v < 0 || u + v > 1) continue;
        const float h = glm::dot(e2, q) * inv;
        if (h >= 0 && h < t) {
            t = h;
            any = true;
        }
    }
    return any;
}

std::vector<uint32_t> CollisionWorld::owners_near(const glm::vec3& p, float radius) const {
    std::vector<uint32_t> cand, out;
    candidates(p - glm::vec3(radius), p + glm::vec3(radius), cand);
    for (uint32_t i : cand) {
        const Triangle& t = tris_[i];
        const glm::vec3 lo = glm::min(glm::min(t.a, t.b), t.c), hi = glm::max(glm::max(t.a, t.b), t.c);
        if (glm::any(glm::greaterThan(lo, p + glm::vec3(radius))) || glm::any(glm::lessThan(hi, p - glm::vec3(radius)))) continue;
        if (std::find(out.begin(), out.end(), t.owner) == out.end()) out.push_back(t.owner);
    }
    return out;
}

}  // namespace nova
