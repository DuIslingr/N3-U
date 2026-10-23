#include "game/lens_flare.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "game/level_logic.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompLensFlare = 138;   // (kCompMesh: level.h)
// The rays' offsets from the sun's direction: rotations about x and y, degrees.
constexpr float kGlowScale = 11.311f;   // fx_lensflare_*'s sunGlow node scale
constexpr float kRayX[5] = {1, -2, 0, 2, -1}, kRayY[5] = {-2, -1, 0, 1, 2};

}  // namespace

void LensFlares::start() {
    flares_.clear();
    const Level& level = scene_.level();
    for (size_t i = 0; i < level.objects.size(); ++i) {
        const LevelObject& o = level.objects[i];
        if (o.kind != "object" || !o.base) continue;
        auto g = scene_.gol().find(o.type);
        if (g == scene_.gol().end()) continue;
        const Value* p = o.components.count(kCompLensFlare) ? &o.components.at(kCompLensFlare)
                                                            : resolved_template(g->second, scene_.gol(), kCompLensFlare);
        if (!p || p->size() < 5) continue;
        const Value* mesh = o.components.count(kCompMesh) ? &o.components.at(kCompMesh) : resolved_template(g->second, scene_.gol(), kCompMesh);
        if (!mesh || mesh->size() < 1 || (*mesh)[0].type != Value::String) continue;
        Flare f;
        f.id = o.base->id;
        for (int k = 0; k < 5; ++k) f.params[k] = (*p)[size_t(k)].as_float();
        Transform t = *o.base;
        t.position = glm::vec3(0);
        f.rotation = placement_matrix(t);
        f.model = scene_.spawn_model((*mesh)[0].s, false);
        if (f.model == size_t(-1)) continue;
        scene_.set_object_visible(f.model, false);   // drawn here, not in the world
        scene_.set_object_visible(i, false);
        for (const std::string& n : scene_.node_names(f.model))
            if (n.rfind("lens_flare_", 0) == 0 && n.find("_PIVOT") == std::string::npos && n != "lens_flare_01") f.chain.push_back(n);
        flares_.push_back(std::move(f));
    }
}

void LensFlares::frame(float dt, const glm::vec3& camera, const glm::mat4& view_proj, int w, int h, ParticleList& out) {
    const bool cast = frames_++ % 8 == 0;
    const float W = float(w), H = float(h);
    auto project = [&](const glm::vec3& p, glm::vec2& s) {
        const glm::vec4 c = view_proj * glm::vec4(p, 1);
        if (c.w <= 1e-4f) return false;
        s = glm::vec2((c.x / c.w * 0.5f + 0.5f) * W, (0.5f - c.y / c.w * 0.5f) * H);
        return true;
    };
    for (Flare& f : flares_) {
        const LevelLogic::Object* o = logic_.object(f.id);
        if (!o || !o->enabled) {
            f.fade = 0;
            continue;
        }
        scene_.set_object_transform(f.model, glm::translate(glm::mat4(1), camera) * f.rotation);
        glm::vec3 sun;
        if (!scene_.node_position(f.model, "sunGlow", sun)) continue;
        if (cast) {   // how much of the sun the world hides
            const glm::vec3 to = sun - camera;
            int blocked = 0;
            for (int k = 0; k < 5; ++k) {
                const glm::mat4 r = glm::rotate(glm::rotate(glm::mat4(1), glm::radians(kRayX[k]), {1, 0, 0}), glm::radians(kRayY[k]), {0, 1, 0});
                float t = 0;
                if (scene_.collision().raycast(camera, camera + glm::vec3(r * glm::vec4(to, 0)), t)) ++blocked;
            }
            f.target = 1 - 0.2f * float(blocked);
            f.glow = blocked < 3;
        }
        // Toward the target at params[4] per second.
        const float step = f.params[4] * dt;
        f.fade = f.fade < f.target ? std::min(f.fade + step, f.target) : std::max(f.fade - step, f.target);
        glm::vec2 s;
        if (f.fade <= 0 || !project(sun, s)) continue;
        // Fading out toward the screen's edges (distances in a 640-high screen's pixels).
        const float edge = std::min(W * 0.5f - std::abs(s.x - W * 0.5f), H * 0.5f - std::abs(s.y - H * 0.5f)) * 640.0f / H;
        const float near_edge = std::clamp(edge * 0.01f, 0.0f, 1.0f), wide = std::clamp(edge * 0.003125f, 0.0f, 1.0f);
        const float bright = near_edge * f.fade;
        if (bright <= 0) continue;
        auto quad = [&](const std::string& node, glm::vec2 centre, float scale, float alpha) {
            Scene::Quad q;
            if (!scene_.node_quad(f.model, node, q) || !q.texture) return;
            glm::vec2 c[4];
            glm::vec2 mid(0);
            for (int k = 0; k < 4; ++k) {
                if (!project(q.position[size_t(k)], c[k])) return;
                mid += c[k] * 0.25f;
            }
            const uint32_t rgba = pack_rgba(glm::vec4(alpha, alpha, alpha, 1));
            ParticleVertex v[4];
            for (int k = 0; k < 4; ++k) v[k] = {glm::vec3(centre + (c[k] - mid) * scale, 0), q.uv[size_t(k)], rgba};
            const uint32_t first = uint32_t(out.vertices.size());
            out.vertices.insert(out.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
            out.batches.push_back({q.texture, true, first, 6});
        };
        // The glow node's scale (11.3 in the model) becomes brightness x params[0].
        if (f.glow)
            for (const char* n : {"sunGlow_PIVOT", "lens_flare_01"}) quad(n, s, bright * f.params[0] / kGlowScale, 1.0f);
        // The chain: each quad at its share of the way from the screen's centre to the sun.
        const glm::vec2 centre(W * 0.5f, H * 0.5f);
        glm::vec3 pivot;
        if (!scene_.node_position(f.model, "lensFlare_PIVOT", pivot)) continue;
        const glm::vec3 axis = pivot - camera;
        const float size = f.params[3] + (f.params[2] - f.params[3]) * f.fade * wide;
        for (const std::string& n : f.chain) {
            glm::vec3 p;
            if (!scene_.node_position(f.model, n, p)) continue;
            const float t = glm::dot(p - camera, axis) / std::max(glm::dot(axis, axis), 1e-3f);
            quad(n, centre + (s - centre) * t, size, bright);
        }
    }
}

}  // namespace nova
