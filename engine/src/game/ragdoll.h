// Ragdolls (the original's CPhysicsRagdollComponent / CRagdoll, on Bullet): the bodies of
// the dead going limp. Their shapes come from the gol's RagdollsSettings (component 0x8c):
// per kind, body parts (capsules from one bone to another, a radius, a parent part) and the
// joints between them (hinges and cones, with their limits); and a table of what kills by
// what death (CRagdoll::GetDeathType: by the killing weapon or projectile's type).
//
// Here as a position-based body rather than rigid bodies: a particle at each part's ends,
// each part and each parent with the starts of its children held rigid, joints kept from
// folding back on themselves, and the particles kept out of the world's triangles. The bones of the parts
// are set from the particles; the rest follow their parents.
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"

namespace nova {

class CollisionWorld;
class Scene;

struct RagdollShape {
    struct Part {
        int id = 0, parent = 0;   // parent: a part id (its own id for the root)
        std::string from, to;     // bones
        float radius = 0.1f;
    };
    struct Joint {
        int type = 2;             // 1 hinge, 2 cone
        int child = 0, parent = 0;
        float a = 0, b = 0, c = 0;   // hinge: low, high; cone: swing, swing, twist (radians)
    };
    std::vector<Part> parts;
    std::vector<Joint> joints;
};

struct RagdollSettings {
    std::map<int, RagdollShape> shapes;        // by kind (CPhysicsRagdollComponent [1])
    std::unordered_map<int, int> death_types;  // by the killing weapon / projectile's gol type
    static RagdollSettings load(const Gol& gol);
    // CRagdoll::GetDeathType: 0 an animated death, 1 a fall, 7 a random bullet hit, 8 an
    // explosion, 9 a heavy hit, 13 a repel, ...; 1 for what the table lacks.
    int death_type(int source) const;
};

class Ragdoll {
public:
    // From the object's pose now; false without the bones it needs.
    bool start(Scene& scene, size_t object, const RagdollShape& shape);
    // Velocity changes: the part nearest `at`, or all of it.
    void hit(const glm::vec3& at, const glm::vec3& dv);
    void push(const glm::vec3& dv);
    // A step of the simulation; the bones placed. False once it has come to rest (or run
    // its course).
    bool step(float dt, const CollisionWorld& world, Scene& scene);
    // Where its middle is (the pelvis), for what follows the body.
    glm::vec3 centre() const;

private:
    struct Particle {
        glm::vec3 p{0}, prev{0};
        float radius = 0.1f;
        bool touching = false;
    };
    struct Rod {
        int a = 0, b = 0;
        float length = 0;
        bool at_least = false;   // only kept from coming nearer
    };
    struct Part {
        int from = 0, to = 0;   // particles
        int node = -1;          // the bone at its start
        glm::mat4 rest{1};      // that bone's world transform when it fell
        glm::vec3 rest_dir{0, 1, 0};
        int parent = -1;        // index of the parent part, -1 for the root
        int ref = -1;           // a particle held off its axis (a child's start): its roll
        glm::mat3 rest_frame{1};
    };
    std::vector<Particle> particles_;
    std::vector<Rod> rods_;
    std::vector<Part> parts_;
    size_t object_ = 0;
    float time_ = 0, still_ = 0;
    void place(Scene& scene) const;
    glm::mat3 turn(const Part& p) const;   // the part's turn from how it fell (only its swing without a ref)
    glm::mat3 frame(const Part& p) const;
};

}  // namespace nova
