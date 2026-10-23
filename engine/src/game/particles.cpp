#include "game/particles.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace nova {

namespace {

template <typename V>
V curve(const std::vector<std::pair<float, V>>& keys, float t, const V& start) {
    // CPSAnim_*::Compute: the first two keys hold the start value; linear between keys,
    // the last key's value after it.
    if (keys.empty()) return start;
    auto value = [&](size_t k) { return k < 2 ? start : keys[k].second; };
    if (t >= keys.back().first) return value(keys.size() - 1);
    for (size_t k = keys.size() - 1; k-- > 0;) {
        if (t < keys[k].first) continue;
        const float span = keys[k + 1].first - keys[k].first;
        const float f = span > 0 ? (t - keys[k].first) / span : 1.0f;
        return value(k) + (value(k + 1) - value(k)) * f;
    }
    return value(0);
}

}  // namespace

int ParticleSystem::start(const std::string& effect, const glm::mat4& transform) {
    if (!library_) return -1;
    auto it = library_->find(effect);
    if (it == library_->end()) return -1;
    Instance inst;
    inst.handle = next_handle_++;
    inst.transform = transform;
    for (const EffectEmitter& e : it->second.emitters) {
        if (e.technique.empty()) continue;   // distortion and other screen effects: not drawn yet
        EmitterState s;
        s.def = &e;
        inst.emitters.push_back(s);
    }
    instances_.push_back(std::move(inst));
    return instances_.back().handle;
}

void ParticleSystem::move(int handle, const glm::mat4& transform) {
    for (Instance& i : instances_)
        if (i.handle == handle) i.transform = transform;
}

void ParticleSystem::stop(int handle) {
    for (Instance& i : instances_)
        if (i.handle == handle) i.emitting = false;
}

bool ParticleSystem::alive(int handle) const {
    return std::any_of(instances_.begin(), instances_.end(), [&](const Instance& i) { return i.handle == handle; });
}

void ParticleSystem::clear() { instances_.clear(); }

size_t ParticleSystem::particle_count() const {
    size_t n = 0;
    for (const Instance& i : instances_)
        for (const EmitterState& e : i.emitters) n += e.particles.size();
    return n;
}

void ParticleSystem::spawn(Instance& inst, EmitterState& s) {
    const EffectEmitter& d = *s.def;
    Particle p;
    glm::vec3 local = d.center;
    const glm::vec3 r(uniform(-1, 1), uniform(-1, 1), uniform(-1, 1));
    switch (d.shape) {
        case EffectEmitter::Shape::Box: local += r * d.extent * 0.5f; break;
        case EffectEmitter::Shape::Sphere: {
            const glm::vec3 dir = glm::length(r) > 1e-4f ? glm::normalize(r) : glm::vec3(0, 0, 1);
            local += dir * uniform(0, d.extent.x);
            break;
        }
        case EffectEmitter::Shape::Disc:
        case EffectEmitter::Shape::Cylinder: {
            const float a = uniform(0, 6.2831853f), rad = d.extent.x * std::sqrt(uniform(0, 1));
            local += glm::vec3(std::cos(a) * rad, std::sin(a) * rad, d.shape == EffectEmitter::Shape::Cylinder ? r.z * d.extent.y * 0.5f : 0);
            break;
        }
        default: break;
    }
    // Velocity: the direction (m/ms) within the spread cone, with speed variation.
    glm::vec3 vel = d.direction * 1000.0f;
    const float speed = glm::length(vel);
    const float spread = float(std::max({d.spread.x, d.spread.y, d.spread.z}));
    if (speed > 0 && spread > 0) {
        const glm::vec3 axis = vel / speed;
        glm::vec3 ortho = glm::cross(axis, std::abs(axis.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
        ortho = glm::normalize(ortho);
        const float tilt = glm::radians(uniform(0, spread)), around = uniform(0, 6.2831853f);
        const glm::mat4 rot = glm::rotate(glm::rotate(glm::mat4(1), around, axis), tilt, ortho);
        vel = glm::vec3(rot * glm::vec4(vel, 0));
    }
    vel *= 1.0f + uniform(-1, 1) * float(d.speed_variation) / 100.0f;
    p.pos = glm::vec3(inst.transform * glm::vec4(local, 1));
    p.vel = glm::vec3(inst.transform * glm::vec4(vel, 0));
    p.life = std::max(uniform(float(d.particle_life_min), float(d.particle_life_max)), 1.0f);
    const float scale = std::max(1.0f + uniform(-1, 1) * float(d.size_variation) / 100.0f, 0.05f);
    p.size = glm::vec2(d.width, d.height) * scale;
    p.color = glm::mix(glm::vec4(d.color_min), glm::vec4(d.color_max), uniform(0, 1));
    p.rot = uniform(float(d.rotation_min), float(d.rotation_max));
    p.spin = uniform(float(d.spin_min), float(d.spin_max));
    p.cell = std::min(int(uniform(0, float(d.uv_cols * d.uv_rows))), d.uv_cols * d.uv_rows - 1);
    s.particles.push_back(p);
}

void ParticleSystem::update(float dt) {
    const float ms = dt * 1000.0f;
    for (Instance& inst : instances_) {
        for (EmitterState& s : inst.emitters) {
            const EffectEmitter& d = *s.def;
            s.time += ms;
            // Emission windows: start after the delay; an emitter with a lifetime is active
            // that long (at least one particle each time), then restarts or ends.
            if (inst.emitting && !s.done && s.time >= float(d.start_delay)) {
                if (!s.active && (s.restart_at < 0 || s.time >= s.restart_at)) {
                    s.active = true;
                    s.accum = 0;
                    s.window_end = d.life_min < 0 ? -1.0f : s.time + uniform(float(d.life_min), float(std::max(d.life_max, d.life_min)));
                    const float rate = uniform(float(d.rate_min), float(d.rate_max));
                    if (s.window_end >= 0) {   // a burst: the window's worth of particles now
                        const int n = std::max(1, int(rate * (s.window_end - s.time) / 1000.0f + 0.5f));
                        for (int k = 0; k < n && s.particles.size() < 2000; ++k) spawn(inst, s);
                    }
                }
                if (s.active && s.window_end < 0) {
                    s.accum += uniform(float(d.rate_min), float(d.rate_max)) * dt;
                    while (s.accum >= 1 && s.particles.size() < 2000) s.accum -= 1, spawn(inst, s);
                }
                if (s.active && s.window_end >= 0 && s.time >= s.window_end) {
                    s.active = false;
                    if (d.restart_min >= 0) s.restart_at = s.time + uniform(float(d.restart_min), float(std::max(d.restart_max, d.restart_min)));
                    else s.done = true;
                }
            }
            for (Particle& p : s.particles) {
                p.age += ms;
                p.vel += d.gravity * 1000.0f * dt;
                p.pos += p.vel * dt;
                p.rot += p.spin * dt;
            }
            s.particles.erase(std::remove_if(s.particles.begin(), s.particles.end(), [](const Particle& p) { return p.age >= p.life; }),
                              s.particles.end());
        }
    }
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
                                    [](const Instance& i) {
                                        for (const EmitterState& s : i.emitters)
                                            if (!s.particles.empty() || (i.emitting && !s.done)) return false;
                                        return true;
                                    }),
                     instances_.end());
}

void ParticleSystem::build(const glm::vec3& eye, const std::function<const GpuTexture*(const std::string&)>& texture,
                           ParticleList& out) const {
    out.vertices.clear();
    out.batches.clear();
    for (const Instance& inst : instances_) {
        for (const EmitterState& s : inst.emitters) {
            if (s.particles.empty()) continue;
            const EffectEmitter& d = *s.def;
            const GpuTexture* tex = texture(d.texture);
            if (!tex) continue;
            const bool additive = d.technique.rfind("Additive", 0) == 0;
            ParticleBatch batch{tex, additive, uint32_t(out.vertices.size()), 0};
            if (!out.batches.empty() && out.batches.back().texture == tex && out.batches.back().additive == additive)
                batch = out.batches.back(), out.batches.pop_back();
            for (const Particle& p : s.particles) {
                const float t = p.age / p.life;
                const glm::vec4 c = glm::clamp(curve(d.color_keys, t, p.color), 0.0f, 255.0f) / 255.0f;
                const glm::vec2 size = curve(d.size_keys, t, p.size);
                if (c.a <= 0.002f && !additive) continue;
                glm::vec3 to_eye = eye - p.pos;
                if (glm::dot(to_eye, to_eye) < 1e-6f) continue;
                to_eye = glm::normalize(to_eye);
                glm::vec3 right, up;
                const float speed = glm::length(p.vel);
                if (d.billboard == 2 && speed > 1e-4f) {   // stretched along the velocity
                    up = p.vel / speed;
                    right = glm::cross(up, to_eye);
                    if (glm::length(right) < 1e-4f) continue;
                    right = glm::normalize(right);
                } else {
                    right = glm::cross(to_eye, glm::vec3(0, 0, 1));
                    right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3(1, 0, 0);
                    up = glm::cross(right, to_eye);
                    if (p.rot != 0) {
                        const float a = glm::radians(p.rot), ca = std::cos(a), sa = std::sin(a);
                        const glm::vec3 r2 = right * ca + up * sa;
                        up = up * ca - right * sa;
                        right = r2;
                    }
                }
                const glm::vec3 hx = right * (size.x * 0.5f), hy = up * (size.y * 0.5f);
                // Texture rectangle, or one cell of the animation grid inside it.
                glm::vec4 uv = d.uv;
                if (const int frames = d.uv_cols * d.uv_rows; frames > 1) {
                    const int f = d.uv_anim ? int(p.age / float(d.uv_period) * float(frames)) % frames : p.cell;
                    const glm::vec2 step(std::abs(uv.z - uv.x), std::abs(uv.w - uv.y));
                    const glm::vec2 off = glm::vec2(float(f % d.uv_cols), float(f / d.uv_cols)) * step;
                    uv += glm::vec4(off, off);
                }
                const uint32_t rgba = pack_rgba(c);
                const ParticleVertex v[4] = {{p.pos - hx + hy, {uv.x, uv.y}, rgba}, {p.pos + hx + hy, {uv.z, uv.y}, rgba},
                                             {p.pos + hx - hy, {uv.z, uv.w}, rgba}, {p.pos - hx - hy, {uv.x, uv.w}, rgba}};
                out.vertices.insert(out.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
                batch.count += 6;
            }
            if (batch.count) out.batches.push_back(batch);
        }
    }
}

}  // namespace nova
