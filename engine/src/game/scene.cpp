#include "game/scene.h"

#include <chrono>
#include <cstdio>
#include <regex>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace nova {

glm::mat4 placement_matrix(const Transform& t) {
    // Irrlicht's setRotationDegrees (verified against matrix4.h): rotate about X, then Y, then Z.
    const glm::vec3 r = glm::radians(t.rotation);
    glm::mat4 m = glm::translate(glm::mat4(1), t.position);
    m = glm::rotate(m, r.z, {0, 0, 1});
    m = glm::rotate(m, r.y, {0, 1, 0});
    m = glm::rotate(m, r.x, {1, 0, 0});
    return glm::scale(m, t.scale);
}

Scene::Scene(const Vfs& vfs, Renderer& renderer, const Gol& gol) : vfs_(vfs), renderer_(renderer), gol_(gol) {}

const GpuTexture* Scene::texture(const std::string& image_name) {
    if (image_name.empty()) return nullptr;
    std::string stem = to_lower(image_name);
    if (auto slash = stem.find_last_of("/\\"); slash != std::string::npos) stem.erase(0, slash + 1);
    if (auto dot = stem.find_last_of('.'); dot != std::string::npos) stem.erase(dot);
    if (auto it = textures_.find(stem); it != textures_.end()) return it->second;
    const GpuTexture* result = nullptr;
    for (const char* ext : {".etc", ".pvr", ".pvr2"}) {
        const auto& hits = vfs_.find(stem + ext);
        if (hits.empty()) continue;
        try {
            result = renderer_.upload_texture(load_texture(vfs_.read(hits.front())));
            ++stats_.textures;
        } catch (const FormatError& ex) {
            std::fprintf(stderr, "texture %s: %s\n", image_name.c_str(), ex.what());
        }
        break;
    }
    if (!result) ++stats_.missing_textures;
    textures_[stem] = result;
    return result;
}

const Scene::LoadedModel* Scene::model(const std::string& name) {
    const std::string key = to_lower(name);
    if (auto it = models_.find(key); it != models_.end()) return it->second.gpu ? &it->second : nullptr;
    LoadedModel& lm = models_[key];
    const auto& hits = vfs_.find(key);
    if (hits.empty()) {
        ++stats_.missing_models;
        return nullptr;
    }
    try {
        lm.cpu = load_bdae(vfs_.read(hits.front()));
    } catch (const FormatError& ex) {
        std::fprintf(stderr, "model %s: %s\n", name.c_str(), ex.what());
        ++stats_.missing_models;
        return nullptr;
    }
    lm.gpu = renderer_.upload_model(lm.cpu);
    for (const Material& m : lm.cpu.materials) {
        LoadedModel::MaterialTextures mt;
        mt.diffuse = texture(m.diffuse);
        mt.lightmap = texture(m.lightmap);
        mt.blend = m.blend;
        mt.alpha_test = !m.blend && mt.diffuse && mt.diffuse->has_alpha;
        lm.materials.push_back(mt);
    }
    ++stats_.models;
    return &lm;
}

void Scene::add_instance(const LoadedModel& m, const glm::mat4& placement) {
    std::vector<std::pair<int, glm::mat4>> stack;
    for (int r : m.cpu.roots) stack.emplace_back(r, placement);
    while (!stack.empty()) {
        auto [ni, parent] = stack.back();
        stack.pop_back();
        const ModelNode& node = m.cpu.nodes[ni];
        glm::mat4 local = glm::translate(glm::mat4(1), node.translation) * glm::mat4_cast(node.rotation);
        local = glm::scale(local, node.scale);
        const glm::mat4 world = parent * local;
        for (int mesh : node.meshes) {
            for (size_t p = 0; p < m.gpu->meshes[mesh].size(); ++p) {
                const GpuModel::Range& range = m.gpu->meshes[mesh][p];
                DrawItem d;
                d.model = m.gpu;
                d.range = range;
                d.world = world;
                if (range.material >= 0) {
                    const auto& mt = m.materials[range.material];
                    d.diffuse = mt.diffuse;
                    d.lightmap = mt.lightmap;
                    d.blend = mt.blend;
                    d.alpha_test = mt.alpha_test;
                }
                const auto& verts = m.cpu.meshes[mesh].primitives[p].vertices;
                glm::vec3 c(0);
                for (const auto& v : verts) c += v.position;
                d.center = glm::vec3(world * glm::vec4(c / float(std::max<size_t>(verts.size(), 1)), 1));
                draws_.push_back(d);
            }
        }
        for (int c : node.children) stack.emplace_back(c, world);
    }
}

void Scene::build_collision() {
    collision_ = CollisionWorld();
    std::unordered_map<std::string, std::optional<CollisionMesh>> cache;
    for (const Placement& p : collision_placements(level_, gol_)) {
        if (doors_open && to_lower(p.object).find("door") != std::string::npos) continue;
        const std::string key = to_lower(p.model);
        auto it = cache.find(key);
        if (it == cache.end()) {
            std::optional<CollisionMesh> mesh;
            try {
                mesh = load_collision(vfs_, p.model);
            } catch (const FormatError& ex) {
                std::fprintf(stderr, "collision %s: %s\n", p.model.c_str(), ex.what());
            }
            it = cache.emplace(key, std::move(mesh)).first;
        }
        if (!it->second) {
            ++stats_.missing_collision;
            continue;
        }
        collision_.add(it->second->vertices, it->second->materials, placement_matrix(p.transform));
        ++stats_.collision_objects;
    }
    collision_.build();
    stats_.collision_triangles = collision_.triangle_count();
}

void Scene::load_level(const std::string& lvc_name) {
    const auto t0 = std::chrono::steady_clock::now();
    level_ = nova::load_level(vfs_.read(lvc_name), gol_);
    draws_.clear();
    for (const Placement& p : model_placements(level_, gol_)) {
        const std::string m = to_lower(p.model);
        if (m.find("sky") != std::string::npos || m.find("_sun") != std::string::npos) continue;   // skyboxes later
        ++stats_.placements;
        if (const LoadedModel* lm = model(p.model)) add_instance(*lm, placement_matrix(p.transform));
    }
    stats_.draws = draws_.size();
    build_collision();

    // Start at the level's start spawn point (spawn flags: 3 chars, the first marks the start).
    const LevelObject* spawn = nullptr;
    for (const auto& o : level_.objects) {
        if (o.kind != "spawn_point" || !o.base) continue;
        if (!spawn) spawn = &o;
        if (o.fields.size() > 0 && o.fields[0].i == 1) {
            spawn = &o;
            break;
        }
    }
    // Some start points only play an intro and then move the player on with the Lua
    // call TeleportPlayerToZone(id), where id is another spawn point. Until scripts
    // run, follow that call directly.
    if (spawn && spawn->fields.size() > 3) {
        static const std::regex teleport(R"(TeleportPlayerToZone\s*\(\s*(\d+)\s*\))");
        std::smatch m;
        const std::string& script = spawn->fields[3].s;
        int target = -1;
        for (auto it = std::sregex_iterator(script.begin(), script.end(), teleport); it != std::sregex_iterator(); ++it) {
            target = std::stoi((*it)[1].str());   // the last call wins
        }
        for (const auto& o : level_.objects) {
            if (target >= 0 && o.kind == "spawn_point" && o.base && o.base->id == target) {
                spawn = &o;
                break;
            }
        }
    }
    if (spawn) {
        start_position = spawn->base->position;
        start_yaw = spawn->base->rotation.z;
    }
    stats_.load_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace nova
