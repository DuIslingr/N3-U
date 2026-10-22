#include "game/scene.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

// LOD sets as the game builds them (CColladaRootLODSceneNode::SearchLODS, addLod and
// initLods). A node whose name contains "LOD" (and not "_PIVOT") belongs to the set
// named by what precedes it; each "LOD<digit>" in the name claims that level and
// "LODD" fills every level nothing else claims. A "lod_dist=D" user property ends the
// level at D metres and starts the next one there. Without any, level 0 always shows.
void Scene::build_lod_sets(LoadedModel& lm) {
    constexpr int kLevels = LoadedModel::kLodLevels;
    constexpr float kFar = std::numeric_limits<float>::max();
    lm.lod_set.assign(lm.cpu.nodes.size(), -1);
    for (size_t n = 0; n < lm.cpu.nodes.size(); ++n) {
        const ModelNode& node = lm.cpu.nodes[n];
        const std::string& name = node.name.empty() ? node.id : node.name;
        size_t at = name.find("LOD");
        if (at == std::string::npos || name.find("_PIVOT") != std::string::npos) continue;
        const std::string key = name.substr(0, at);
        size_t si = 0;
        while (si < lm.lod_sets.size() && lm.lod_sets[si].name != key) ++si;
        if (si == lm.lod_sets.size()) {
            LoadedModel::LodSet set;
            set.name = key;
            set.nodes.fill(-1);
            set.min_sq.fill(kFar);
            set.max_sq.fill(kFar);
            set.min_sq[0] = 0;
            lm.lod_sets.push_back(set);
        }
        LoadedModel::LodSet& set = lm.lod_sets[si];
        for (; at != std::string::npos; at = name.find("LOD", at + 3)) {
            const char c = at + 3 < name.size() ? name[at + 3] : '\0';
            if (c == 'D') {
                for (int& slot : set.nodes)
                    if (slot < 0) slot = int(n);
                lm.lod_set[n] = int(si);
            } else if (c >= '0' && c <= '9') {
                const int level = c - '0';
                set.nodes[level] = int(n);
                lm.lod_set[n] = int(si);
                if (node.properties.rfind("lod_dist=", 0) != 0) continue;
                const float d = std::strtof(node.properties.c_str() + 9, nullptr);
                if (d <= 0) continue;
                if (level + 1 < kLevels) set.min_sq[level + 1] = d * d;
                set.max_sq[level] = d * d;
                for (int i = level + 1; i + 1 < kLevels; ++i) set.min_sq[i + 1] = std::max(set.min_sq[i + 1], set.min_sq[i]);
                for (int i = level; i + 1 < kLevels; ++i) set.max_sq[i + 1] = std::max(set.max_sq[i + 1], set.max_sq[i]);
                set.fixed = false;
            }
        }
    }
    // Levels before the first one present show the first; gaps show the level before.
    for (LoadedModel::LodSet& set : lm.lod_sets) {
        int first = 0, last = kLevels - 1;
        while (first < kLevels && set.nodes[first] < 0) ++first;
        if (first == kLevels) continue;
        while (set.nodes[last] < 0) --last;
        for (int i = 0; i < first; ++i) set.nodes[i] = set.nodes[first];
        for (int i = first + 1; i <= last; ++i)
            if (set.nodes[i] < 0) set.nodes[i] = set.nodes[i - 1];
    }
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
    lm.parent.assign(lm.cpu.nodes.size(), -1);
    for (size_t n = 0; n < lm.cpu.nodes.size(); ++n)
        for (int c : lm.cpu.nodes[n].children) lm.parent[c] = int(n);
    for (const Skin& sk : lm.cpu.skins) {
        auto& jn = lm.joint_nodes.emplace_back();
        for (const std::string& j : sk.joints) {
            int found = -1;
            for (size_t n = 0; n < lm.cpu.nodes.size() && found < 0; ++n)
                if (lm.cpu.nodes[n].sid == j) found = int(n);
            for (size_t n = 0; n < lm.cpu.nodes.size() && found < 0; ++n)
                if (lm.cpu.nodes[n].id == j) found = int(n);
            jn.push_back(found);
        }
    }
    build_lod_sets(lm);
    ++stats_.models;
    return &lm;
}

const AnimationLibrary* Scene::animation(const std::string& name) {
    const std::string key = to_lower(name);
    if (auto it = animations_.find(key); it != animations_.end()) return it->second.get();
    std::unique_ptr<AnimationLibrary> lib;
    if (const auto& hits = vfs_.find(key); !hits.empty()) {
        try {
            lib = std::make_unique<AnimationLibrary>(load_animation(vfs_.read(hits.front())));
        } catch (const FormatError& ex) {
            std::fprintf(stderr, "animation %s: %s\n", name.c_str(), ex.what());
        }
    }
    if (!lib) ++stats_.missing_animations;
    return (animations_[key] = std::move(lib)).get();
}

namespace {
glm::mat4 local_matrix(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    return glm::scale(glm::translate(glm::mat4(1), t) * glm::mat4_cast(r), s);
}
}  // namespace

void Scene::add_instance(const LoadedModel& m, const glm::mat4& placement, const Placement* p) {
    // Animated if it has an animation clip or any skinned mesh (skins need a joint palette).
    const AnimationLibrary* lib = p && !p->anim_file.empty() ? animation(p->anim_file) : nullptr;
    std::unique_ptr<AnimatedInstance> anim;
    if (lib || !m.cpu.skins.empty()) {
        anim = std::make_unique<AnimatedInstance>();
        anim->model = &m;
        anim->placement = placement;
        anim->library = lib;
        anim->world.assign(m.cpu.nodes.size(), glm::mat4(1));
        anim->palettes.resize(m.cpu.skins.size());
        anim->tracks.assign(m.cpu.nodes.size(), nullptr);
        if (lib) {
            const AnimClip* c = lib->clip(p->anim_clip);
            anim->clip = c ? *c : AnimClip{"timeline", lib->timeline_start, lib->timeline_end};
            anim->speed = p->anim_speed > 0 ? p->anim_speed : 1.0f;
            for (size_t n = 0; n < m.cpu.nodes.size(); ++n) {
                if (auto it = lib->tracks.find(m.cpu.nodes[n].id); it != lib->tracks.end()) anim->tracks[n] = &it->second;
            }
            // Offset each object into its loop so identical enemies don't move in lockstep.
            anim->time = float((p->transform.id * 2654435761u) % 10007u) / 1000.0f;
        }
    }

    LodInstance lods;
    lods.model = &m;
    if (!m.lod_sets.empty()) lods.node_pos.assign(m.cpu.nodes.size(), glm::vec3(0));

    struct Visit {
        int node;
        glm::mat4 parent;
        std::vector<int> lod_nodes;
    };
    std::vector<Visit> stack;
    for (int r : m.cpu.roots) stack.push_back({r, placement, {}});
    while (!stack.empty()) {
        Visit v = std::move(stack.back());
        stack.pop_back();
        const int ni = v.node;
        const ModelNode& node = m.cpu.nodes[ni];
        // Collision proxies for the physics engine ("r=0.75" etc. in their properties).
        if (node.name.rfind("PhysicsShape", 0) == 0) continue;
        const glm::mat4 world = v.parent * local_matrix(node.translation, node.rotation, node.scale);
        if (!m.lod_sets.empty()) {
            lods.node_pos[ni] = glm::vec3(world[3]);
            if (m.lod_set[ni] >= 0) v.lod_nodes.push_back(ni);
        }
        for (int mesh : node.meshes) {
            const int skin = m.cpu.meshes[mesh].skin;
            for (size_t pi = 0; pi < m.gpu->meshes[mesh].size(); ++pi) {
                const GpuModel::Range& range = m.gpu->meshes[mesh][pi];
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
                const auto& verts = m.cpu.meshes[mesh].primitives[pi].vertices;
                glm::vec3 c(0);
                for (const auto& v : verts) c += v.position;
                d.center = glm::vec3(world * glm::vec4(c / float(std::max<size_t>(verts.size(), 1)), 1));
                if (anim) anim->draws.push_back({draws_.size(), ni, skin});
                if (!v.lod_nodes.empty()) lods.draws.push_back({draws_.size(), v.lod_nodes});
                draws_.push_back(d);
            }
        }
        for (int c : node.children) stack.push_back({c, world, v.lod_nodes});
    }
    if (anim) {
        pose(*anim);
        lods.anim = anim.get();
        animated_.push_back(std::move(anim));
        ++stats_.animated;
    }
    if (!lods.draws.empty()) {
        lods.level.assign(m.lod_sets.size(), 0);
        select_lods(lods, glm::vec3(placement[3]));
        bool fixed = true;
        for (const auto& set : m.lod_sets) fixed = fixed && set.fixed;
        if (!fixed) lod_instances_.push_back(std::move(lods));
    }
}

void Scene::select_lods(LodInstance& li, const glm::vec3& camera) {
    const LoadedModel& m = *li.model;
    for (size_t s = 0; s < m.lod_sets.size(); ++s) {
        const LoadedModel::LodSet& set = m.lod_sets[s];
        int level = 0;
        if (!set.fixed) {
            // Distance to the shown level's node, as the game measures it.
            int node = set.nodes[li.level[s]];
            for (int k = 0; node < 0 && k < LoadedModel::kLodLevels; ++k) node = set.nodes[k];
            if (node < 0) continue;
            const glm::vec3 pos = li.anim ? glm::vec3(li.anim->world[node][3]) : li.node_pos[node];
            const glm::vec3 d = pos - camera;
            const float d2 = glm::dot(d, d);
            for (int k = LoadedModel::kLodLevels - 1; k >= 0; --k) {
                if (set.min_sq[k] <= d2 && d2 < set.max_sq[k]) {
                    level = k;
                    break;
                }
            }
        }
        li.level[s] = level;
    }
    for (const auto& d : li.draws) {
        bool visible = true;
        for (int n : d.lod_nodes) {
            const int s = m.lod_set[n];
            visible = visible && m.lod_sets[s].nodes[li.level[s]] == n;
        }
        draws_[d.item].visible = visible;
    }
}

void Scene::pose(AnimatedInstance& a) {
    const LoadedModel& m = *a.model;
    float t = 0;
    if (a.library) {
        const float len = a.clip.end - a.clip.start;
        t = a.clip.start + (len > 0 ? std::fmod(a.time * 1000.0f, len) : 0.0f);
    }
    for (size_t n = 0; n < m.cpu.nodes.size(); ++n) {   // parents come before children
        const ModelNode& node = m.cpu.nodes[n];
        NodePose p{node.translation, node.rotation, node.scale};
        if (a.tracks[n]) sample_tracks(*a.tracks[n], t, p);
        const glm::mat4 parent = m.parent[n] < 0 ? a.placement : a.world[m.parent[n]];
        a.world[n] = parent * local_matrix(p.translation, p.rotation, p.scale);
    }
    for (size_t s = 0; s < m.cpu.skins.size(); ++s) {
        const Skin& sk = m.cpu.skins[s];
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            const int node = m.joint_nodes[s][j];
            a.palettes[s][j] = node >= 0 ? a.world[node] * sk.inverse_bind[j] : a.placement;
        }
    }
    for (const auto& d : a.draws) {
        DrawItem& item = draws_[d.item];
        if (d.skin >= 0) {
            item.world = glm::mat4(1);
            item.joints = a.palettes[d.skin].data();
        } else {
            item.world = a.world[d.node];
        }
    }
}

void Scene::update(float dt, const glm::vec3& camera) {
    for (auto& a : animated_) {
        if (!a->library) continue;
        a->time += dt * a->speed;
        pose(*a);
    }
    for (LodInstance& li : lod_instances_) select_lods(li, camera);
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
    animated_.clear();
    lod_instances_.clear();
    for (const Placement& p : model_placements(level_, gol_)) {
        const std::string m = to_lower(p.model);
        if (m.find("sky") != std::string::npos || m.find("_sun") != std::string::npos) continue;   // skyboxes later
        ++stats_.placements;
        if (const LoadedModel* lm = model(p.model)) add_instance(*lm, placement_matrix(p.transform), &p);
    }
    stats_.draws = draws_.size();
    stats_.lod_hidden = 0;
    for (const DrawItem& d : draws_) stats_.lod_hidden += !d.visible;
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
