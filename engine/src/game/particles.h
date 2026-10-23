// Particle effects from the effect library (levels/effects.bin): instances of
// effects placed in the world, their emitters and particles, and the quads that draw
// them. Modelled on the game's spark particle system: each emitter emits at a rate
// within an active window (or forever), particles fly with their velocity and the
// effect's gravity, and colour/size follow curves over their life.
#pragma once

#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/effects.h"
#include "render/renderer.h"

namespace nova {

class ParticleSystem {
public:
    explicit ParticleSystem(const std::unordered_map<std::string, ParticleEffect>* library) : library_(library) {}

    // Start an effect at `transform`; returns its handle, or -1 if there is no such effect.
    int start(const std::string& effect, const glm::mat4& transform);
    void move(int handle, const glm::mat4& transform);
    void stop(int handle);          // stop emitting; live particles fade out on their own
    bool alive(int handle) const;
    void clear();

    void update(float dt);
    // Quads for the camera at `eye`; `texture` resolves texture file names.
    void build(const glm::vec3& eye, const std::function<const GpuTexture*(const std::string&)>& texture,
               ParticleList& out) const;
    size_t particle_count() const;
    bool has(const std::string& effect) const { return library_ && library_->count(effect); }
    // Called when an emitter with sounds starts emitting: its sounds (pick one) and where.
    std::function<void(const std::vector<std::string>&, const glm::vec3&)> on_sound;

private:
    struct Particle {
        glm::vec3 pos, vel;
        float age = 0, life = 1;   // ms
        glm::vec2 size;
        glm::vec4 color;           // 0..255
        float rot = 0, spin = 0;   // degrees, degrees per second
        int cell = 0;              // sprite-sheet cell (sheets that do not animate)
    };
    struct EmitterState {
        const EffectEmitter* def = nullptr;
        float time = 0;            // ms since the instance started
        float window_end = 0;      // end of the current active window (ms); < 0: forever
        float restart_at = -1;     // next window start (ms); < 0: none
        float accum = 0;           // fractional particles owed
        bool active = false, done = false;
        std::vector<Particle> particles;
    };
    struct Instance {
        int handle = 0;
        glm::mat4 transform{1};
        bool emitting = true;
        std::vector<EmitterState> emitters;
    };
    void spawn(Instance& inst, EmitterState& e);
    float uniform(float a, float b) { return std::uniform_real_distribution<float>(std::min(a, b), std::max(a, b))(rng_); }

    const std::unordered_map<std::string, ParticleEffect>* library_;
    std::vector<Instance> instances_;
    int next_handle_ = 1;
    std::mt19937 rng_{99};
};

}  // namespace nova
