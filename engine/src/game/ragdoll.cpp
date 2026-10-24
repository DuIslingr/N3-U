#include "game/ragdoll.h"

#include <algorithm>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include "game/physics.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompRagdollsSettings = 0x8c;
constexpr float kGravity = 9.8f;
constexpr float kStep = 1.0f / 120.0f;
constexpr int kIterations = 8;

float num(const Value& v, size_t i, float fallback) { return i < v.size() ? v[i].as_float() : fallback; }
std::string str(const Value& v, size_t i) { return i < v.size() && v[i].type == Value::String ? v[i].s : std::string(); }

glm::mat3 swing(const glm::vec3& from, const glm::vec3& to) {
    return glm::mat3_cast(glm::rotation(glm::normalize(from), glm::normalize(to)));
}

}  // namespace

RagdollSettings RagdollSettings::load(const Gol& gol) {
    RagdollSettings s;
    for (const auto& [id, type] : gol) {
        const Value* v = resolved_template(type, gol, kCompRagdollsSettings);
        if (!v || v->size() < 2) continue;
        // [0]: {{kind, {parts}, {joints}}...}; a part {id, parent, index, from bone, to bone,
        // radius, ...}; a joint {id, type, child part, parent part, limits...}.
        for (const Value& kind : (*v)[0].list) {
            if (kind.size() < 3) continue;
            RagdollShape& shape = s.shapes[kind[0].i];
            for (const Value& p : kind[1].list) {
                if (p.size() < 6) continue;
                shape.parts.push_back({p[0].i, p[1].i, str(p, 3), str(p, 4), num(p, 5, 0.1f)});
            }
            for (const Value& j : kind[2].list) {
                if (j.size() < 7) continue;
                shape.joints.push_back({j[1].i, j[2].i, j[3].i, num(j, 4, 0), num(j, 5, 0), num(j, 6, 0)});
            }
        }
        // [1]: {{weapon or projectile type, death type}...}
        for (const Value& e : (*v)[1].list)
            if (e.size() >= 2) s.death_types[e[0].i] = e[1].i;
        break;
    }
    return s;
}

int RagdollSettings::death_type(int source) const {
    auto it = death_types.find(source);
    return it != death_types.end() && it->second >= 0 ? it->second : 1;
}

bool Ragdoll::start(Scene& scene, size_t object, const RagdollShape& shape) {
    object_ = object;
    particles_.clear(), rods_.clear(), parts_.clear();
    std::unordered_map<std::string, int> by_bone;
    auto particle = [&](const std::string& bone, float radius) -> int {
        if (auto it = by_bone.find(bone); it != by_bone.end()) {
            particles_[size_t(it->second)].radius = std::max(particles_[size_t(it->second)].radius, radius);
            return it->second;
        }
        if (scene.node_index(object, bone) < 0) return -1;
        Particle p;
        p.p = p.prev = glm::vec3(scene.object_matrix(object, bone)[3]);
        p.radius = radius;
        particles_.push_back(p);
        return by_bone[bone] = int(particles_.size()) - 1;
    };
    std::unordered_map<int, int> part_index;   // part id -> index
    for (const RagdollShape::Part& sp : shape.parts) {
        const float r = std::clamp(sp.radius, 0.06f, 0.25f);
        Part p;
        p.from = particle(sp.from, r), p.to = particle(sp.to, r);
        if (p.from < 0 || p.to < 0 || p.from == p.to) continue;
        p.node = scene.node_index(object, sp.from);
        p.rest = scene.object_matrix(object, sp.from);
        const glm::vec3 d = particles_[size_t(p.to)].p - particles_[size_t(p.from)].p;
        if (glm::length(d) < 1e-3f) continue;
        p.rest_dir = glm::normalize(d);
        part_index[sp.id] = int(parts_.size());
        parts_.push_back(p);
    }
    if (parts_.empty()) return false;
    // Parents.
    for (const RagdollShape::Part& sp : shape.parts) {
        auto me = part_index.find(sp.id);
        if (me == part_index.end()) continue;
        if (auto par = part_index.find(sp.parent); par != part_index.end() && par->second != me->second)
            parts_[size_t(me->second)].parent = par->second;
    }
    // A part with a child's start off its axis has its roll too (the pelvis, the chest).
    for (size_t i = 0; i < parts_.size(); ++i) {
        Part& p = parts_[i];
        const glm::vec3 a = particles_[size_t(p.from)].p;
        for (const Part& c : parts_) {
            if (c.parent != int(i) || c.from == p.from || c.from == p.to) continue;
            const glm::vec3 off = particles_[size_t(c.from)].p - a;
            if (glm::length(off - p.rest_dir * glm::dot(off, p.rest_dir)) > 0.03f) {
                p.ref = c.from;
                break;
            }
        }
        if (p.ref >= 0) p.rest_frame = frame(p);
    }
    // Rods: each part; each parent with its children's starts, all held at their distances.
    auto rod = [&](int a, int b) {
        if (a == b) return;
        for (const Rod& r : rods_)
            if (!r.at_least && ((r.a == a && r.b == b) || (r.a == b && r.b == a))) return;
        const float len = glm::distance(particles_[size_t(a)].p, particles_[size_t(b)].p);
        if (len > 1e-3f) rods_.push_back({a, b, len, false});
    };
    // The joints: a child's end kept from folding back onto its parent's start (in place of
    // the original's hinge and cone limits, which fight a body of particles).
    for (const Part& c : parts_) {
        if (c.parent < 0) continue;
        const int a = parts_[size_t(c.parent)].from, b = c.to;
        const float len = glm::distance(particles_[size_t(a)].p, particles_[size_t(b)].p);
        if (a != b && len > 1e-3f) rods_.push_back({a, b, len * 0.65f, true});
    }
    for (size_t i = 0; i < parts_.size(); ++i) {
        std::vector<int> cluster{parts_[i].from, parts_[i].to};
        for (const Part& c : parts_)
            if (c.parent == int(i) && std::find(cluster.begin(), cluster.end(), c.from) == cluster.end()) cluster.push_back(c.from);
        for (size_t a = 0; a < cluster.size(); ++a)
            for (size_t b = a + 1; b < cluster.size(); ++b) rod(cluster[a], cluster[b]);
    }
    time_ = still_ = 0;
    return true;
}

void Ragdoll::hit(const glm::vec3& at, const glm::vec3& dv) {
    int best = -1;
    float best_d = 1e9f;
    for (size_t i = 0; i < parts_.size(); ++i) {
        const glm::vec3 a = particles_[size_t(parts_[i].from)].p, b = particles_[size_t(parts_[i].to)].p;
        const glm::vec3 ab = b - a;
        const float t = std::clamp(glm::dot(at - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
        const float d = glm::distance(at, a + ab * t);
        if (d < best_d) best_d = d, best = int(i);
    }
    if (best < 0) return;
    // Velocity is position against the last: moving the last back sets it going.
    for (int k : {parts_[size_t(best)].from, parts_[size_t(best)].to}) particles_[size_t(k)].prev -= dv * kStep;
}

void Ragdoll::push(const glm::vec3& dv) {
    for (Particle& p : particles_) p.prev -= dv * kStep;
}

glm::mat3 Ragdoll::frame(const Part& p) const {
    const glm::vec3 a = particles_[size_t(p.from)].p;
    const glm::vec3 x = glm::normalize(particles_[size_t(p.to)].p - a);
    glm::vec3 y = particles_[size_t(p.ref)].p - a;
    y = glm::normalize(y - x * glm::dot(y, x));
    return glm::mat3(x, y, glm::cross(x, y));
}

glm::mat3 Ragdoll::turn(const Part& p) const {
    if (p.ref >= 0) return frame(p) * glm::transpose(p.rest_frame);
    return swing(p.rest_dir, particles_[size_t(p.to)].p - particles_[size_t(p.from)].p);
}

bool Ragdoll::step(float dt, const CollisionWorld& world, Scene& scene) {
    time_ += dt;
    int steps = std::clamp(int(std::round(dt / kStep)), 1, 8);
    float fastest = 0;
    while (steps-- > 0) {
        std::vector<glm::vec3> before(particles_.size());
        for (size_t i = 0; i < particles_.size(); ++i) {
            Particle& p = particles_[i];
            before[i] = p.p;
            const glm::vec3 v = (p.p - p.prev) * 0.995f;
            p.prev = p.p;
            p.p += v + glm::vec3(0, 0, -kGravity) * kStep * kStep;
        }
        for (int it = 0; it < kIterations; ++it) {
            for (const Rod& r : rods_) {
                Particle &a = particles_[size_t(r.a)], &b = particles_[size_t(r.b)];
                const glm::vec3 d = b.p - a.p;
                const float len = glm::length(d);
                if (len < 1e-6f) continue;
                if (r.at_least && len >= r.length) continue;   // a bend: only kept from folding tighter
                const glm::vec3 fix = d * (0.5f * (len - r.length) / len);
                a.p += fix, b.p -= fix;
            }
            for (size_t i = 0; i < particles_.size(); ++i)
                particles_[i].touching |= world.push_sphere(particles_[i].p, particles_[i].radius, nullptr, &before[i]);
        }
        for (size_t i = 0; i < particles_.size(); ++i) {
            Particle& p = particles_[i];
            if (p.touching) p.prev = p.p - (p.p - p.prev) * 0.6f;   // friction on what it lies on
            if (const glm::vec3 v = p.p - p.prev; glm::length(v) > 20.0f * kStep) p.prev = p.p - v * (20.0f * kStep / glm::length(v));
            p.touching = false;
            fastest = std::max(fastest, glm::length(p.p - p.prev) / kStep);
        }
    }
    place(scene);
    still_ = fastest < 0.15f ? still_ + dt : 0;
    return time_ < 8.0f && !(time_ > 1.0f && still_ > 0.5f);
}

glm::vec3 Ragdoll::centre() const { return parts_.empty() ? glm::vec3(0) : particles_[size_t(parts_[0].from)].p; }

void Ragdoll::place(Scene& scene) const {
    std::unordered_map<int, glm::mat4> worlds;
    for (const Part& p : parts_) {
        if (p.node < 0) continue;
        glm::mat4 w = glm::mat4(turn(p) * glm::mat3(p.rest));
        w[3] = glm::vec4(particles_[size_t(p.from)].p, 1);
        worlds[p.node] = w;
    }
    scene.set_node_worlds(object_, std::move(worlds));
}

}  // namespace nova
