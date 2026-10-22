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

Scene::Scene(const Vfs& vfs, Renderer& renderer, const Gol& gol) : vfs_(vfs), renderer_(renderer), gol_(gol) {
    materials_ = std::make_unique<MaterialSystem>(
        vfs_, renderer_, [this](const std::string& image) { return texture(image); }, ShaderCompiler::default_cache_dir());
}

void Scene::set_game_shaders(bool on) {
    game_shaders_ = on;
    for (size_t i = 0; i < draws_.size(); ++i) draws_[i].material = on ? draw_materials_[i] : nullptr;
}

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

size_t Scene::add_instance(const LoadedModel& m, const glm::mat4& placement, const Placement* p, bool sky) {
    Instance& inst = instances_.emplace_back();
    inst.first_draw = draws_.size();
    // Animated if it has an animation clip or any skinned mesh (skins need a joint palette).
    const AnimationLibrary* lib = p && !p->anim_file.empty() ? animation(p->anim_file) : nullptr;
    std::unique_ptr<AnimatedInstance> anim;
    if (lib || !m.cpu.skins.empty()) {
        anim = std::make_unique<AnimatedInstance>();
        anim->model = &m;
        anim->placement = placement;
        anim->placement_inverse = glm::inverse(placement);
        anim->loop = !sky;
        anim->overrides.resize(m.cpu.materials.size());
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
            for (const ValueTrack& vt : lib->values) {
                if (vt.light) continue;   // bound when the light is placed below
                for (size_t mi = 0; mi < m.cpu.materials.size(); ++mi) {
                    if (m.cpu.materials[mi].id != vt.target) continue;
                    anim->overrides[mi].push_back({vt.param, vt.first, std::min(vt.stride, 4 - vt.first), glm::vec4(0)});
                    anim->values.push_back({&vt, int(mi), int(anim->overrides[mi].size()) - 1, -1, 1});
                }
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
        // Point and spot lights (CCustomColladaLightSceneNode); a black light is switched off.
        for (int li : node.lights) {
            const ModelLight& ml = m.cpu.lights[size_t(li)];
            const ValueTrack* colour = nullptr;
            if (anim && anim->library) {
                for (const ValueTrack& vt : anim->library->values)
                    if (vt.light && vt.target == ml.id) colour = &vt;
            }
            if (sky || (ml.type != 1 && ml.type != 2) || ml.falloff_end <= 0) continue;
            if (glm::vec3(ml.color) == glm::vec3(0) && !colour) continue;   // black and never lit
            if (colour) anim->values.push_back({colour, -1, -1, int(lights_.size()), ml.intensity});
            PointLight pl;
            pl.position = glm::vec3(world[3]);
            pl.diffuse = ml.color * ml.intensity;
            pl.attenuation = {ml.falloff_start, ml.falloff_end > ml.falloff_start ? 1.0f / (ml.falloff_end - ml.falloff_start) : 0.0f, 0};
            pl.radius = ml.falloff_end;
            lights_.push_back(pl);
            light_sources_.push_back({anim.get(), ni, pl.position});
        }
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
                const GpuMaterial* gm = range.material >= 0 ? materials_->get(m.cpu.materials[range.material], skin >= 0) : nullptr;
                d.material = game_shaders_ ? gm : nullptr;
                d.lighting = &inst.lighting;
                d.sky = sky;
                if (anim && range.material >= 0 && !anim->overrides[size_t(range.material)].empty())
                    d.overrides = &anim->overrides[size_t(range.material)];
                draw_materials_.push_back(gm);
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
    inst.draw_count = draws_.size() - inst.first_draw;
    return instances_.size() - 1;
}

void Scene::load_skyboxes() {
    // Every skybox is loaded; the one flagged for the start (or the only one) is shown.
    const auto refs = level_skyboxes(level_);
    int initial = refs.size() == 1 ? refs.front().id : -1;
    for (const SkyboxRef& r : refs) {
        if (r.initial && initial < 0) initial = r.id;
        const LoadedModel* lm = model(r.model);
        if (!lm) continue;
        // Skyboxes can carry their own animation (Boreas' sandstorm); play its timeline.
        Placement p;
        p.model = r.model;
        p.object = "skybox";
        const std::string key = to_lower(r.model);
        if (!animations_.count(key)) {
            std::unique_ptr<AnimationLibrary> lib;
            try {
                lib = std::make_unique<AnimationLibrary>(load_animation(vfs_.read(vfs_.find(key).front())));
            } catch (const std::exception&) {
            }
            if (lib && lib->tracks.empty() && lib->values.empty()) lib.reset();
            animations_[key] = std::move(lib);
        }
        if (animations_[key]) p.anim_file = r.model;
        skyboxes_.push_back({r.id, add_instance(*lm, glm::mat4(1), &p, true)});
    }
    stats_.skyboxes = skyboxes_.size();
    if (!set_skybox(initial)) set_skybox(skyboxes_.empty() ? -1 : skyboxes_.front().id);
}

bool Scene::set_skybox(int id) {
    bool found = false;
    for (const Skybox& s : skyboxes_) {
        const Instance& inst = instances_[s.instance];
        found = found || s.id == id;
        for (size_t i = 0; i < inst.draw_count; ++i) draws_[inst.first_draw + i].visible = s.id == id;
    }
    return found;
}

// CNovaSceneManager::UpdateLights, per instance: the light probe at the bounds centre, and
// the nearest light (measured from the edge of its range), blended with the next one
// when the object is inside both.
void Scene::compute_object_lighting() {
    for (Instance& inst : instances_) {
        if (!inst.draw_count) continue;
        glm::vec3 center(0);
        for (size_t i = 0; i < inst.draw_count; ++i) center += draws_[inst.first_draw + i].center;
        center /= float(inst.draw_count);
        ObjectLighting& L = inst.lighting;
        L.center = center;
        L.probe = irradiance_parameters(sample_irradiance(irradiance_, center));
        int best = -1, second = -1;
        float best_d = 0, second_d = 0;
        for (size_t i = 0; i < lights_.size(); ++i) {
            const float d = glm::distance(center, lights_[i].position) - lights_[i].radius;
            if (best < 0 || d < best_d) {
                second = best, second_d = best_d;
                best = int(i), best_d = d;
            } else if (second < 0 || d < second_d) {
                second = int(i), second_d = d;
            }
        }
        L.has_light = best >= 0;
        if (!L.has_light) continue;
        const PointLight& a = lights_[size_t(best)];
        L.l0_position = a.position;
        L.l0_color = a.diffuse;
        L.l0_attenuation = a.attenuation;
        L.l0_scale = glm::vec3(1);
        if (second >= 0 && second_d < 0 && best_d < 0) {
            const PointLight& b = lights_[size_t(second)];
            const float t = best_d / (second_d + best_d);
            L.l0_position = t * a.position + (1 - t) * b.position;
            L.l0_color = a.diffuse + (1 - t) * (b.diffuse - a.diffuse);
        }
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
        const float ms = a.time * 1000.0f;
        t = a.clip.start + (len <= 0 ? 0.0f : a.loop ? std::fmod(ms, len) : std::min(ms, len));
    }
    for (size_t n = 0; n < m.cpu.nodes.size(); ++n) {   // parents come before children
        const ModelNode& node = m.cpu.nodes[n];
        NodePose p{node.translation, node.rotation, node.scale};
        if (a.tracks[n]) sample_tracks(*a.tracks[n], t, p);
        const glm::mat4 parent = m.parent[n] < 0 ? a.placement : a.world[m.parent[n]];
        a.world[n] = parent * local_matrix(p.translation, p.rotation, p.scale);
    }
    float v[16];
    for (const auto& vb : a.values) {
        if (vb.track->stride > 16) continue;
        sample_value(*vb.track, t, v);
        if (vb.material >= 0) {
            ParamOverride& o = a.overrides[size_t(vb.material)][size_t(vb.slot)];
            for (int c = 0; c < o.count; ++c) o.value[c] = v[c];
        } else if (vb.light >= 0) {
            // CCustomColladaLightSceneNode: a black light is switched off.
            const glm::vec3 rgb(v[0], vb.track->stride > 1 ? v[1] : v[0], vb.track->stride > 2 ? v[2] : v[0]);
            lights_[size_t(vb.light)].diffuse = glm::vec4(rgb, 1) * vb.intensity;
        }
    }
    for (size_t s = 0; s < m.cpu.skins.size(); ++s) {
        const Skin& sk = m.cpu.skins[s];
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            const int node = m.joint_nodes[s][j];
            // Object space: the game's shaders get the placement as World.
            a.palettes[s][j] = node >= 0 ? a.placement_inverse * a.world[node] * sk.inverse_bind[j] : glm::mat4(1);
        }
    }
    for (const auto& d : a.draws) {
        DrawItem& item = draws_[d.item];
        if (d.skin >= 0) {
            item.world = a.placement;
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
    for (size_t i = 0; i < light_sources_.size(); ++i) {
        const LightSource& s = light_sources_[i];
        if (s.anim) lights_[i].position = glm::vec3(s.anim->world[size_t(s.node)][3]);
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
    for (const LevelObject& o : level_.objects) {
        if (o.kind != "level_properties" || o.fields.size() < 8) continue;
        // i, then fog colour as an SColor's bytes (B, G, R, A), fog near, fog far, density.
        auto byte = [&](size_t k) { return float(uint8_t(o.fields[k].i)) / 255.0f; };
        fog_color = {byte(3), byte(2), byte(1), byte(4)};
        fog_start = o.fields[5].as_float();
        fog_end = o.fields[6].as_float();
        fog_density = o.fields[7].as_float();
    }
    draws_.clear();
    draw_materials_.clear();
    irradiance_.clear();
    // Irradiance volumes belong to level objects: <level>_<object id>.irr, relative to the object.
    const std::string stem = lvc_name.substr(0, lvc_name.rfind('.'));
    for (const LevelObject& o : level_.objects) {
        if (!o.base) continue;
        const auto& hits = vfs_.find(stem + "_" + std::to_string(o.base->id) + ".irr");
        if (hits.empty()) continue;
        try {
            auto vols = load_irradiance(vfs_.read(hits.front()), o.base->position);
            for (auto& v : vols) irradiance_.push_back(std::move(v));
        } catch (const FormatError& ex) {
            std::fprintf(stderr, "irradiance %s_%d: %s\n", stem.c_str(), o.base->id, ex.what());
        }
    }
    stats_.irradiance_volumes = irradiance_.size();
    animated_.clear();
    lod_instances_.clear();
    instances_.clear();
    skyboxes_.clear();
    lights_.clear();
    light_sources_.clear();
    for (const LevelLight& l : level_lights(level_, gol_)) {
        // CLightComponent::Load: attenuation (1, 1 / radius, 0).
        PointLight pl;
        pl.position = l.position;
        pl.diffuse = l.color * l.intensity;
        pl.attenuation = {1, l.radius > 0 ? 1.0f / l.radius : 0.0f, 0};
        pl.radius = l.radius;
        pl.fake = l.fake;
        lights_.push_back(pl);
        light_sources_.push_back({nullptr, -1, pl.position});
    }
    for (const Placement& p : model_placements(level_, gol_)) {
        ++stats_.placements;
        if (const LoadedModel* lm = model(p.model)) add_instance(*lm, placement_matrix(p.transform), &p);
    }
    load_skyboxes();
    compute_object_lighting();
    stats_.lights = lights_.size();
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
