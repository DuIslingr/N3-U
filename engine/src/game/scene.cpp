#include "game/scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    if (const auto& hits = vfs_.find("effects.bin"); !hits.empty()) {
        try {
            effect_library_ = load_effects(vfs_.read(hits.front()));
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "effects.bin: %s\n", ex.what());
        }
    }
}

void Scene::build_particles(const glm::vec3& eye, ParticleList& out) {
    particles_.build(eye, [this](const std::string& image) { return texture(image); }, out);
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
    for (const Mesh& mesh : lm.cpu.meshes) {
        auto& per_prim = lm.bounds.emplace_back();
        for (const Primitive& prim : mesh.primitives) {
            glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
            for (const Vertex& v : prim.vertices) lo = glm::min(lo, v.position), hi = glm::max(hi, v.position);
            per_prim.emplace_back(lo, hi);
        }
    }
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
    glm::vec3 bounds_min(std::numeric_limits<float>::max()), bounds_max(-std::numeric_limits<float>::max());
    glm::vec3 local_min = bounds_min, local_max = bounds_max;
    const glm::mat4 to_local = glm::inverse(placement);
    // Animated if it has an animation clip or any skinned mesh (skins need a joint palette).
    const AnimationLibrary* lib = p && !p->anim_file.empty() ? animation(p->anim_file) : nullptr;
    std::unique_ptr<AnimatedInstance> anim;
    if (lib || !m.cpu.skins.empty() || (p && p->animated)) {
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
                if (vt.camera_fov) {      // "Camera003-camera" -> the node Camera003 holding it
                    const std::string node = vt.target.size() > 7 && vt.target.compare(vt.target.size() - 7, 7, "-camera") == 0
                                                 ? vt.target.substr(0, vt.target.size() - 7) : vt.target;
                    for (size_t n = 0; n < m.cpu.nodes.size(); ++n)
                        if (m.cpu.nodes[n].camera_fov > 0 && (m.cpu.nodes[n].name == node || m.cpu.nodes[n].id == node + "-node"))
                            anim->values.push_back({&vt, -1, -1, -1, 1, -1, int(n)});
                    continue;
                }
                if (vt.visibility) {
                    for (size_t n = 0; n < m.cpu.nodes.size(); ++n)
                        if (m.cpu.nodes[n].id == vt.target) anim->values.push_back({&vt, -1, -1, -1, 1, int(n)});
                    continue;
                }
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

    // Distance fade (CGameObject::InitFadeParams): the first node with properties on each
    // branch from the roots sets fadeNear/fadeFar.
    const bool fades = p && (p->mesh_flags & 0xC) != 0;
    float fade_near = 1, fade_far = 10;
    if (fades) {
        std::vector<int> todo(m.cpu.roots.begin(), m.cpu.roots.end());
        while (!todo.empty()) {
            const ModelNode& node = m.cpu.nodes[size_t(todo.back())];
            todo.pop_back();
            if (node.properties.empty()) {
                todo.insert(todo.end(), node.children.begin(), node.children.end());
                continue;
            }
            if (auto at = node.properties.find("fadeNear="); at != std::string::npos)
                fade_near = std::strtof(node.properties.c_str() + at + 9, nullptr);
            if (auto at = node.properties.find("fadeFar="); at != std::string::npos)
                fade_far = std::strtof(node.properties.c_str() + at + 8, nullptr);
        }
    }

    LodInstance lods;
    lods.model = &m;
    if (!m.lod_sets.empty()) lods.node_pos.assign(m.cpu.nodes.size(), glm::vec3(0));

    struct Visit {
        int node;
        glm::mat4 parent;
        std::vector<int> lod_nodes;
        int controller = -1;   // index into corona_controllers_ of the enclosing controller
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
        // Stand-ins that vehicles replace with objects of their own (CVehicleCar::Load).
        if (node.name.rfind("HIDDEN_", 0) == 0) continue;
        const glm::mat4 world = v.parent * local_matrix(node.translation, node.rotation, node.scale);
        // CCustomColladaFactory::createNode: a node whose name contains "coronas" is a
        // CCoronasControllerNode; its <name>_PIVOT child's properties tune it (InitCoronas).
        if (!sky && !anim && v.controller < 0 && node.name.find("coronas") != std::string::npos) {
            CoronaController c;
            c.world = world;
            c.inverse = glm::inverse(world);
            for (int ch : node.children) {
                const std::string& props = m.cpu.nodes[size_t(ch)].properties;
                auto num = [&](const char* key, float& out) {
                    if (auto at = props.find(key); at != std::string::npos) out = std::strtof(props.c_str() + at + std::strlen(key), nullptr);
                };
                num("coronasMinScale=", c.min_scale);
                num("coronasMin=", c.min_dist);
                num("coronasMax=", c.max_dist);
            }
            v.controller = int(corona_controllers_.size());
            corona_controllers_.push_back(std::move(c));
        }
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
            light_sources_.push_back({anim.get(), ni, pl.position, p ? zones_.object_zone(p->object_index) : ZoneSystem::kNone});
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
                const auto [lo, hi] = m.bounds[size_t(mesh)][pi];
                DrawInfo info;
                info.name = (p ? p->model : std::string("?")) + " node " + node.name +
                            (range.material >= 0 ? " material " + m.cpu.materials[range.material].id : std::string());
                info.min = glm::vec3(1e30f), info.max = glm::vec3(-1e30f);
                if (lo.x <= hi.x) {
                    for (int k = 0; k < 8; ++k) {
                        const glm::vec3 corner((k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z);
                        const glm::vec3 w = glm::vec3(world * glm::vec4(corner, 1));
                        info.min = glm::min(info.min, w), info.max = glm::max(info.max, w);
                        bounds_min = glm::min(bounds_min, w);
                        bounds_max = glm::max(bounds_max, w);
                        const glm::vec3 l = glm::vec3(to_local * glm::vec4(w, 1));
                        local_min = glm::min(local_min, l);
                        local_max = glm::max(local_max, l);
                    }
                }
                const GpuMaterial* gm = range.material >= 0 ? materials_->get(m.cpu.materials[range.material], skin >= 0) : nullptr;
                d.material = game_shaders_ ? gm : nullptr;
                d.lighting = &inst.lighting;
                d.sky = sky;
                if (anim && range.material >= 0 && !anim->overrides[size_t(range.material)].empty())
                    d.overrides = &anim->overrides[size_t(range.material)];
                if (fades && range.material >= 0) {
                    const auto& params = m.cpu.materials[range.material].params;
                    if (std::any_of(params.begin(), params.end(), [](const MaterialParam& mp) { return mp.name == "FadeFactor"; })) {
                        DistanceFade f;
                        // Animated: share the material's overrides (filled before any draw points at them).
                        f.values = anim ? &anim->overrides[size_t(range.material)] : &fade_values_.emplace_back();
                        auto it = std::find_if(f.values->begin(), f.values->end(), [](const ParamOverride& o) { return o.param == "FadeFactor"; });
                        f.slot = size_t(it - f.values->begin());
                        if (it == f.values->end()) f.values->push_back({"FadeFactor", 0, 1, glm::vec4(1)});
                        d.overrides = f.values;
                        f.anim = anim.get();
                        f.node = ni;
                        f.position = glm::vec3(world[3]);
                        f.axis = glm::vec3(world * glm::vec4(0, 1, 0, 0));
                        f.near = fade_near, f.far = fade_far;
                        f.fade_in = (p->mesh_flags & 4) != 0;
                        fades_.push_back(f);
                    }
                }
                draw_materials_.push_back(gm);
                draw_info_.push_back(std::move(info));
                if (anim) anim->draws.push_back({draws_.size(), ni, skin});
                if (!v.lod_nodes.empty()) lods.draws.push_back({draws_.size(), v.lod_nodes});
                if (v.controller >= 0)
                    corona_controllers_[size_t(v.controller)].draws.push_back({draws_.size(), corona_controllers_[size_t(v.controller)].inverse * world});
                draws_.push_back(d);
            }
        }
        for (int c : node.children) stack.push_back({c, world, v.lod_nodes, v.controller});
    }
    if (anim) {
        // A fade may have added overrides to a material drawn earlier.
        for (const auto& ad : anim->draws) {
            DrawItem& d = draws_[ad.item];
            if (d.range.material >= 0 && !anim->overrides[size_t(d.range.material)].empty())
                d.overrides = &anim->overrides[size_t(d.range.material)];
        }
        pose(*anim);
        inst.anim = anim.get();
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
    if (bounds_min.x <= bounds_max.x) {
        inst.world_min = bounds_min, inst.world_max = bounds_max;
        inst.local_min = local_min, inst.local_max = local_max;
    }
    if (p && !sky) {
        inst.zone = zones_.object_zone(p->object_index);
        if (bounds_min.x <= bounds_max.x) zones_.grow(p->object_index, bounds_min, bounds_max);
    }
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
// when the object is inside both. Only lights registered this frame count — those of
// visible zones — however far; with none, the object gets no light (L0 black).
void Scene::compute_object_lighting() {
    for (Instance& inst : instances_) {
        if (!inst.draw_count) continue;
        glm::vec3 center(0);
        for (size_t i = 0; i < inst.draw_count; ++i) center += draws_[inst.first_draw + i].center;
        light_instance(inst, center / float(inst.draw_count));
    }
}

void Scene::light_instance(Instance& inst, const glm::vec3& center) {
    {
        ObjectLighting& L = inst.lighting;
        L.center = center;
        L.probe = irradiance_parameters(sample_irradiance(irradiance_, center));
        int best = -1, second = -1;
        float best_d = 0, second_d = 0;
        for (size_t i = 0; i < lights_.size(); ++i) {
            if (i < light_on_.size() && !light_on_[i]) continue;
            const float d = glm::distance(center, lights_[i].position) - lights_[i].radius;
            if (best < 0 || d < best_d) {
                second = best, second_d = best_d;
                best = int(i), best_d = d;
            } else if (second < 0 || d < second_d) {
                second = int(i), second_d = d;
            }
        }
        L.has_light = best >= 0;
        if (!L.has_light) return;
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
        const float into = len <= 0 ? 0.0f : a.loop ? std::fmod(ms, len) : std::min(ms, len);
        t = a.reverse ? a.clip.end - into : a.clip.start + into;
    }
    for (size_t n = 0; n < m.cpu.nodes.size(); ++n) {   // parents come before children
        const ModelNode& node = m.cpu.nodes[n];
        NodePose p{node.translation, node.rotation, node.scale};
        if (a.tracks[n]) sample_tracks(*a.tracks[n], t, p);
        if (auto moved = a.node_translation.find(int(n)); moved != a.node_translation.end()) p.translation = moved->second;
        if (int(n) == a.root_lock) p.translation.x = a.root_xy.x, p.translation.y = a.root_xy.y;
        const glm::mat4 parent = m.parent[n] < 0 ? a.placement : a.world[m.parent[n]];
        a.world[n] = parent * local_matrix(p.translation, p.rotation, p.scale);
    }
    float v[16];
    bool any_visibility = false;
    a.node_hidden.assign(m.cpu.nodes.size(), 0);
    for (const auto& vb : a.values) {
        if (vb.track->stride > 16) continue;
        sample_value(*vb.track, t, v);
        if (vb.camera >= 0) {
            a.camera_fov[vb.camera] = v[0];
        } else if (vb.node >= 0) {
            a.node_hidden[size_t(vb.node)] = v[0] < 0.5f;
            any_visibility = true;
        } else if (vb.material >= 0) {
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
    for (size_t n = 0; n < a.node_forced.size(); ++n) {
        if (a.node_forced[n] < 0) continue;
        a.node_hidden[n] = a.node_forced[n] == 0;
        any_visibility = true;
    }
    if (any_visibility) {
        for (size_t n = 0; n < m.cpu.nodes.size(); ++n)   // parents come before children
            if (m.parent[n] >= 0 && a.node_hidden[size_t(m.parent[n])]) a.node_hidden[n] = 1;
    }
    for (const auto& d : a.draws) {
        DrawItem& item = draws_[d.item];
        item.anim_hidden = any_visibility && a.node_hidden[size_t(d.node)];
        if (d.skin >= 0) {
            item.world = a.placement;
            item.joints = a.palettes[d.skin].data();
        } else {
            item.world = a.world[d.node];
        }
    }
}

void Scene::update(float dt, const glm::vec3& camera) {
    particles_.update(dt);
    for (auto& a : animated_) {
        if (!a->library) continue;
        a->time += dt * a->speed;
        pose(*a);
    }
    for (LodInstance& li : lod_instances_) select_lods(li, camera);
    // CCoronasControllerNode: a ray from the camera to each corona; seen, it fades in
    // over 200 ms, blocked it fades out four times faster.
    for (Corona& c : coronas_) {
        DrawItem& d = draws_[c.draw];
        bool seen = d.visible && !d.hidden && !d.anim_hidden && !d.zone_hidden;
        if (seen) {
            const glm::vec3 to = d.center + glm::normalize(camera - d.center) * 0.3f;
            float t = 1;
            seen = glm::distance(camera, d.center) < 0.3f || !collision_.raycast(camera, to, t);
        }
        c.fade = std::clamp(c.fade + (seen ? 1.0f : -4.0f) * dt * 1000.0f, 0.0f, 200.0f);
        d.occluded = c.fade <= 0;
    }
    // CCoronasControllerNode::onAnimate: the controller's scale follows the fade and the
    // camera distance: fade/200 * max(min scale, min(distance, max) / min).
    stats_.corona_controllers = corona_controllers_.size();
    stats_.coronas_shown = 0;
    for (CoronaController& c : corona_controllers_) {
        bool seen = false;
        for (const auto& [di, local] : c.draws) {
            const DrawItem& d = draws_[di];
            seen = seen || (d.visible && !d.hidden && !d.anim_hidden && !d.zone_hidden);
        }
        const glm::vec3 pos(c.world[3]);
        const float dist = glm::distance(camera, pos);
        if (seen) {
            float t = 1;
            seen = dist < 0.3f || !collision_.raycast(camera, pos + (camera - pos) / std::max(dist, 1e-3f) * 0.3f, t);
        }
        c.fade = std::clamp(c.fade + (seen ? 1.0f : -4.0f) * dt * 1000.0f, 0.0f, 200.0f);
        const float scale = c.fade * 0.005f * std::max(c.min_scale, std::min(dist, c.max_dist) / c.min_dist);
        stats_.coronas_shown += scale > 0;
        const glm::mat4 scaled = glm::scale(c.world, glm::vec3(scale));
        for (const auto& [di, local] : c.draws) {
            DrawItem& d = draws_[di];
            d.world = scaled * local;
            d.occluded = scale <= 0;
        }
    }
    for (DistanceFade& f : fades_) {
        if (f.anim) {
            const glm::mat4& w = f.anim->world[size_t(f.node)];
            f.position = glm::vec3(w[3]);
            f.axis = glm::vec3(w * glm::vec4(0, 1, 0, 0));
        }
        const float d = std::abs(glm::dot(f.axis, camera) - glm::dot(f.axis, f.position));
        float v = 1;
        if (f.fade_in) {
            if (!(d > f.near)) v = 0;
            else if (d < f.far) v = (d - f.near) / (f.far - f.near);
        } else {
            if (d > f.far) v = 0;
            else if (d > f.near) v = (f.far - d) / (f.far - f.near);
        }
        (*f.values)[f.slot].value.x = v;
    }
    for (size_t i = 0; i < light_sources_.size(); ++i) {
        const LightSource& s = light_sources_[i];
        if (s.anim) lights_[i].position = glm::vec3(s.anim->world[size_t(s.node)][3]);
    }
}

void Scene::build_collision() {
    collision_ = CollisionWorld();
    std::unordered_map<std::string, std::optional<CollisionMesh>> cache;
    for (const Placement& p : collision_placements(level_, gol_)) {
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
        collision_.add(it->second->vertices, it->second->materials, placement_matrix(p.transform), uint32_t(p.object_index));
        ++stats_.collision_objects;
    }
    collision_.build();
    stats_.collision_triangles = collision_.triangle_count();
}

void Scene::load_level(const std::string& lvc_name, uint32_t section_mask) {
    moving_.clear();
    material_copies_.clear();
    const auto t0 = std::chrono::steady_clock::now();
    level_ = nova::load_level(vfs_.read(lvc_name), gol_);
    particles_.clear();
    keep_section(level_, section_mask);
    level_name_ = lvc_name;
    section_ = section_mask;
    stats_ = SceneStats{};
    for (const LevelObject& o : level_.objects) {
        if (o.kind != "level_properties" || o.fields.size() < 8) continue;
        // i, then fog colour as an SColor's bytes (R, G, B, A: glitch's SColor keeps red in the low byte), fog near, fog far, density.
        auto byte = [&](size_t k) { return float(uint8_t(o.fields[k].i)) / 255.0f; };
        fog_color = {byte(1), byte(2), byte(3), byte(4)};
        fog_start = o.fields[5].as_float();
        fog_end = o.fields[6].as_float();
        fog_density = o.fields[7].as_float();
    }
    draws_.clear();
    draw_materials_.clear();
    draw_info_.clear();
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
    corona_controllers_.clear();
    fades_.clear();
    fade_values_.clear();
    instances_.clear();
    skyboxes_.clear();
    zones_.build(level_, gol_);
    lights_.clear();
    light_sources_.clear();
    light_on_.clear();
    for (const LevelLight& l : level_lights(level_, gol_)) {
        // CLightComponent::Load: attenuation (1, 1 / radius, 0).
        PointLight pl;
        pl.position = l.position;
        pl.diffuse = l.color * l.intensity;
        pl.attenuation = {1, l.radius > 0 ? 1.0f / l.radius : 0.0f, 0};
        pl.radius = l.radius;
        pl.fake = l.fake;
        lights_.push_back(pl);
        light_sources_.push_back({nullptr, -1, pl.position, zones_.object_zone(l.index)});
    }
    object_instances_.clear();
    for (const Placement& p : model_placements(level_, gol_)) {
        ++stats_.placements;
        if (const LoadedModel* lm = model(p.model)) object_instances_[p.object_index] = add_instance(*lm, placement_matrix(p.transform), &p);
    }
    // Doors with a zone link are portals between the two zones (CDoorAIComponent::
    // LoadZonesLink): a quad at the centre of the door's bounds, standing up (+90 deg about
    // X), as wide as the door and 0.3 m taller; it is open while the door is.
    for (size_t i = 0; i < level_.objects.size(); ++i) {
        const LevelObject& o = level_.objects[i];
        auto link = o.components.find(0x72);
        auto inst = object_instances_.find(i);
        if (link == o.components.end() || link->second.size() < 2 || inst == object_instances_.end() || !o.base) continue;
        const int a = link->second[0].i, b = link->second[1].i;
        if (a == -1 || b == -1) continue;
        const Instance& in = instances_[inst->second];
        if (in.world_min.x > in.world_max.x) continue;
        Transform t = *o.base;
        t.position = (in.world_min + in.world_max) * 0.5f - glm::vec3(0, 0, 0.15f);
        t.rotation.x += 90.0f;
        const float width = in.local_max.x - in.local_min.x, height = in.local_max.z + 0.3f - in.local_min.z;
        zones_.add_door_portal(i, a, b, placement_matrix(t), width, height);
    }
    load_skyboxes();
    compute_object_lighting();
    // Coronas: additive sprites drawn without depth testing (CCoronasControllerNode).
    coronas_.clear();
    std::vector<uint8_t> controlled(draws_.size(), 0);
    for (const CoronaController& c : corona_controllers_)
        for (const auto& d : c.draws) controlled[d.first] = 1;
    for (size_t i = 0; i < draws_.size(); ++i)
        if (draws_[i].material && !draws_[i].material->state.depth_test && !draws_[i].sky && !controlled[i]) coronas_.push_back({i, 0.0f});
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

void Scene::set_object_visible(size_t object, bool visible) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end()) return;
    const Instance& inst = instances_[it->second];
    for (size_t i = 0; i < inst.draw_count; ++i) draws_[inst.first_draw + i].hidden = !visible;
}

bool Scene::play_clip(size_t object, const std::string& clip, bool loop, bool reverse) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end()) return false;
    AnimatedInstance* a = instances_[it->second].anim;
    if (!a || !a->library) return false;
    const AnimClip* c = a->library->clip(clip);
    if (!c) return false;
    a->clip = *c;
    a->clip_name = clip;
    ++a->clip_serial;
    a->time = 0;
    a->loop = loop;
    a->reverse = reverse;
    if (a->root_lock >= 0 && a->tracks[size_t(a->root_lock)]) {
        const ModelNode& node = a->model->cpu.nodes[size_t(a->root_lock)];
        NodePose p{node.translation, node.rotation, node.scale};
        sample_tracks(*a->tracks[size_t(a->root_lock)], reverse ? c->end : c->start, p);
        a->root_xy = {p.translation.x, p.translation.y};
    }
    pose(*a);
    return true;
}

void Scene::set_clip_time(size_t object, float seconds) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end()) return;
    AnimatedInstance* a = instances_[it->second].anim;
    if (!a) return;
    a->time = seconds, a->speed = 0;
    pose(*a);
}

size_t Scene::spawn_object(int type, int id, const glm::vec3& position, float yaw) {
    auto g = gol_.find(type);
    LevelObject o;
    o.kind = "object";
    o.type = type;
    o.name = g != gol_.end() ? g->second.name : "";
    Transform t;
    t.id = id;
    t.position = position;
    t.rotation = {0, 0, yaw};
    o.base = t;
    const size_t index = level_.objects.size();
    level_.objects.push_back(o);
    Level one;
    one.objects.push_back(o);
    for (Placement p : model_placements(one, gol_)) {
        p.object_index = index;
        if (const LoadedModel* lm = model(p.model)) {
            const size_t inst = add_instance(*lm, placement_matrix(p.transform), &p);
            object_instances_[index] = inst;
            Instance& in = instances_[inst];
            light_instance(in, (in.world_min + in.world_max) * 0.5f);
            set_object_visible(index, false);
        }
    }
    stats_.draws = draws_.size();
    return index;
}

size_t Scene::spawn_model(const std::string& model_file, bool hud, const std::string& anim_file, const std::string& clip) {
    LevelObject o;
    o.kind = "runtime";
    o.name = model_file;
    o.base = Transform{};
    const size_t index = level_.objects.size();
    level_.objects.push_back(o);
    Placement p;
    p.object_index = index;
    p.model = model_file;
    p.animated = true;
    p.anim_file = anim_file;
    p.anim_clip = clip;
    if (const LoadedModel* lm = model(p.model)) {
        const size_t inst = add_instance(*lm, glm::mat4(1), &p);
        object_instances_[index] = inst;
        Instance& in = instances_[inst];
        in.zone = ZoneSystem::kNone;
        in.never_culled = hud;
        for (size_t i = 0; i < in.draw_count; ++i) draws_[in.first_draw + i].hud = hud;
    }
    stats_.draws = draws_.size();
    return index;
}

bool Scene::clip_state(size_t object, ClipState& out) const {
    const AnimatedInstance* a = object_anim(object);
    if (!a || !a->library || a->clip_name.empty()) return false;
    out.name = a->clip_name;
    out.length = a->clip.end - a->clip.start;
    out.ms = a->time * 1000.0f;
    if (a->loop && out.length > 0) out.ms = std::fmod(out.ms, out.length);
    else out.ms = std::min(out.ms, out.length);
    out.serial = a->clip_serial;
    out.reverse = a->reverse;
    return true;
}

float Scene::clip_progress(size_t object) const {
    const AnimatedInstance* a = object_anim(object);
    if (!a) return 1;
    const float len = a->clip.end - a->clip.start;
    return len <= 0 ? 1.0f : std::clamp(a->time * 1000.0f / len, 0.0f, 1.0f);
}

const Scene::AnimatedInstance* Scene::object_anim(size_t object) const {
    auto it = object_instances_.find(object);
    return it == object_instances_.end() ? nullptr : instances_[it->second].anim;
}

int Scene::find_node(const LoadedModel& m, const std::string& name) const {
    for (size_t n = 0; n < m.cpu.nodes.size(); ++n)
        if (m.cpu.nodes[n].name == name || m.cpu.nodes[n].id == name) return int(n);
    return -1;
}

bool Scene::set_object_transform(size_t object, const glm::mat4& placement) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end() || !instances_[it->second].anim) return false;
    Instance& inst = instances_[it->second];
    AnimatedInstance* a = inst.anim;
    a->placement = placement;
    a->placement_inverse = glm::inverse(placement);
    pose(*a);
    const glm::vec3 c(placement[3]);
    inst.world_min = c - glm::vec3(2), inst.world_max = c + glm::vec3(2);
    inst.zone = ZoneSystem::kNone;
    for (size_t i = 0; i < inst.draw_count; ++i) draws_[inst.first_draw + i].center = c;
    light_instance(inst, c);
    return true;
}

void Scene::set_object_viewmodel(size_t object) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end()) return;
    Instance& inst = instances_[it->second];
    inst.zone = ZoneSystem::kNone;
    inst.never_culled = true;
    for (size_t i = 0; i < inst.draw_count; ++i) draws_[inst.first_draw + i].viewmodel = true, draws_[inst.first_draw + i].zone_hidden = false;
}

bool Scene::node_quad(size_t object, const std::string& node, Quad& out) const {
    const AnimatedInstance* a = object_anim(object);
    if (!a) return false;
    const LoadedModel& m = *a->model;
    std::vector<int> todo{find_node(m, node)};
    while (!todo.empty()) {
        const int n = todo.back();
        todo.pop_back();
        if (n < 0) continue;
        const ModelNode& mn = m.cpu.nodes[size_t(n)];
        for (int mesh : mn.meshes) {
            const auto& prims = m.cpu.meshes[size_t(mesh)].primitives;
            if (prims.empty() || prims[0].vertices.size() < 4) continue;
            for (int k = 0; k < 4; ++k) {
                out.position[size_t(k)] = glm::vec3(a->world[size_t(n)] * glm::vec4(prims[0].vertices[size_t(k)].position, 1));
                out.uv[size_t(k)] = prims[0].vertices[size_t(k)].uv0;
            }
            out.texture = prims[0].material >= 0 ? m.materials[size_t(prims[0].material)].diffuse : nullptr;
            return true;
        }
        todo.insert(todo.end(), mn.children.begin(), mn.children.end());
    }
    return false;
}

std::vector<std::string> Scene::node_names(size_t object) const {
    std::vector<std::string> out;
    if (const AnimatedInstance* a = object_anim(object))
        for (const ModelNode& n : a->model->cpu.nodes) out.push_back(n.name);
    return out;
}

bool Scene::move_object(size_t object, const glm::vec3& position) {
    if (object >= level_.objects.size() || !level_.objects[object].base) return false;
    const Transform& base = *level_.objects[object].base;
    moving_.insert(object);
    collision_.set_owner_offset(uint32_t(object), position - base.position);
    return set_object_pose(object, position, base.rotation.z);
}

bool Scene::set_object_pose(size_t object, const glm::vec3& position, float yaw) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end()) return false;
    Instance& inst = instances_[it->second];
    AnimatedInstance* a = inst.anim;
    if (!a) return false;
    const glm::vec3 scale(glm::length(glm::vec3(a->placement[0])), glm::length(glm::vec3(a->placement[1])),
                          glm::length(glm::vec3(a->placement[2])));
    a->placement = glm::scale(glm::rotate(glm::translate(glm::mat4(1), position), glm::radians(yaw), glm::vec3(0, 0, 1)), scale);
    a->placement_inverse = glm::inverse(a->placement);
    pose(*a);
    // Bounds from the placement-space box; actors leave their zone, so test them against the view.
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (int k = 0; k < 8; ++k) {
        const glm::vec3 c((k & 1) ? inst.local_max.x : inst.local_min.x, (k & 2) ? inst.local_max.y : inst.local_min.y,
                          (k & 4) ? inst.local_max.z : inst.local_min.z);
        const glm::vec3 w = glm::vec3(a->placement * glm::vec4(c, 1));
        lo = glm::min(lo, w), hi = glm::max(hi, w);
    }
    inst.world_min = lo, inst.world_max = hi;
    inst.zone = ZoneSystem::kNone;
    for (size_t i = 0; i < inst.draw_count; ++i) draws_[inst.first_draw + i].center = (lo + hi) * 0.5f;
    light_instance(inst, (lo + hi) * 0.5f);
    return true;
}

bool Scene::set_root_lock(size_t object, const std::string& node) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end() || !instances_[it->second].anim) return false;
    AnimatedInstance& a = *instances_[it->second].anim;
    a.root_lock = find_node(*a.model, node);
    return a.root_lock >= 0;
}

std::vector<std::string> Scene::clip_names(size_t object) const {
    std::vector<std::string> out;
    const AnimatedInstance* a = object_anim(object);
    if (a && a->library)
        for (const AnimClip& c : a->library->clips) out.push_back(c.name);
    return out;
}

float Scene::clip_length(size_t object, const std::string& clip) const {
    const AnimatedInstance* a = object_anim(object);
    const AnimClip* c = a && a->library ? a->library->clip(clip) : nullptr;
    return c ? (c->end - c->start) / 1000.0f : 0.0f;
}

float Scene::clip_root_speed(size_t object, const std::string& clip) const {
    const AnimatedInstance* a = object_anim(object);
    const AnimClip* c = a && a->library ? a->library->clip(clip) : nullptr;
    if (!c || a->root_lock < 0 || !a->tracks[size_t(a->root_lock)] || c->end <= c->start) return 0;
    const ModelNode& node = a->model->cpu.nodes[size_t(a->root_lock)];
    NodePose p0{node.translation, node.rotation, node.scale}, p1 = p0;
    sample_tracks(*a->tracks[size_t(a->root_lock)], c->start, p0);
    sample_tracks(*a->tracks[size_t(a->root_lock)], c->end, p1);
    const float scale = glm::length(glm::vec3(a->placement[0]));
    return glm::length(glm::vec2(p1.translation - p0.translation)) * scale / ((c->end - c->start) / 1000.0f);
}

void Scene::set_clip_speed(size_t object, float speed) {
    auto it = object_instances_.find(object);
    if (it != object_instances_.end() && instances_[it->second].anim) instances_[it->second].anim->speed = speed;
}

void Scene::set_node_visible(size_t object, const std::string& node, bool visible) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end() || !instances_[it->second].anim) return;
    AnimatedInstance& a = *instances_[it->second].anim;
    int n = find_node(*a.model, node);
    if (n < 0) n = find_node(*a.model, node + "-node");
    if (n < 0) return;
    if (a.node_forced.size() != a.model->cpu.nodes.size()) a.node_forced.assign(a.model->cpu.nodes.size(), -1);
    const int8_t want = visible ? 1 : 0;
    if (a.node_forced[size_t(n)] == want) return;
    a.node_forced[size_t(n)] = want;
    pose(a);
}

void Scene::set_node_translation(size_t object, const std::string& node, const glm::vec3& local) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end() || !instances_[it->second].anim) return;
    AnimatedInstance& a = *instances_[it->second].anim;
    int n = find_node(*a.model, node);
    if (n < 0) return;
    a.node_translation[n] = local;
    pose(a);
}

bool Scene::camera_view(size_t object, CameraView& out) const {
    const AnimatedInstance* a = object_anim(object);
    if (!a) return false;
    const auto& nodes = a->model->cpu.nodes;
    for (size_t n = 0; n < nodes.size(); ++n) {
        if (nodes[n].camera_fov <= 0) continue;
        const glm::mat4& w = a->world[n];
        out.position = glm::vec3(w[3]);
        out.forward = glm::normalize(-glm::vec3(w[2]));
        out.up = glm::normalize(glm::vec3(w[1]));
        auto animated = a->camera_fov.find(int(n));
        out.fov = animated != a->camera_fov.end() && animated->second > 0 ? animated->second : nodes[n].camera_fov;
        return true;
    }
    return false;
}

glm::mat4 Scene::object_matrix(size_t object, const std::string& node) const {
    if (const AnimatedInstance* a = object_anim(object)) {
        if (node.empty()) return a->placement;   // where it is now (it may have moved)
        if (const int n = find_node(*a->model, node); n >= 0) return a->world[size_t(n)];
    }
    if (object < level_.objects.size() && level_.objects[object].base) return placement_matrix(*level_.objects[object].base);
    return glm::mat4(1);
}

bool Scene::node_position(size_t object, const std::string& node, glm::vec3& out) const {
    const AnimatedInstance* a = object_anim(object);
    if (!a) return false;
    const int n = find_node(*a->model, node);
    if (n < 0) return false;
    out = glm::vec3(a->world[size_t(n)][3]);
    return true;
}

bool Scene::clip_finished(size_t object) const {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end()) return true;
    const AnimatedInstance* a = instances_[it->second].anim;
    if (!a || a->loop) return !a;
    return a->time * 1000.0f >= a->clip.end - a->clip.start;
}

size_t Scene::hide_models(const std::string& substring) {
    size_t n = 0;
    const std::string want = to_lower(substring);
    for (const Placement& p : model_placements(level_, gol_)) {
        if (to_lower(p.model).find(want) == std::string::npos) continue;
        set_object_visible(p.object_index, false);
        ++n;
    }
    return n;
}

bool Scene::set_object_lightmap(size_t object, const std::string& name) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end()) return false;
    std::string file = name;
    if (file.find('.') == std::string::npos) file += ".tga";
    const GpuTexture* lm = texture(file);
    if (!lm) return false;
    const Instance& inst = instances_[it->second];
    bool any = false;
    for (size_t i = inst.first_draw; i < inst.first_draw + inst.draw_count; ++i) {
        DrawItem& d = draws_[i];
        if (!d.lightmap || d.lightmap == lm) continue;
        if (d.material) {
            GpuMaterial copy = *d.material;
            for (const GpuTexture*& t : copy.fragment_textures)
                if (t == d.lightmap) t = lm;
            for (const GpuTexture*& t : copy.vertex_textures)
                if (t == d.lightmap) t = lm;
            material_copies_.push_back(std::move(copy));
            d.material = &material_copies_.back();
        }
        d.lightmap = lm;
        any = true;
    }
    return any;
}

void Scene::set_object_param(size_t object, const std::string& param, const glm::vec4& value, int count) {
    auto it = object_instances_.find(object);
    if (it == object_instances_.end()) return;
    const Instance& inst = instances_[it->second];
    for (size_t i = inst.first_draw; i < inst.first_draw + inst.draw_count; ++i) {
        DrawItem& d = draws_[i];
        if (!d.material) continue;
        if (!d.overrides) d.overrides = &fade_values_.emplace_back();
        auto& list = *const_cast<std::vector<ParamOverride>*>(d.overrides);
        auto o = std::find_if(list.begin(), list.end(), [&](const ParamOverride& p) { return p.param == param; });
        if (o == list.end()) list.push_back({param, 0, count, value});
        else o->value = value, o->count = count;
    }
}

void Scene::cull(const glm::vec3& camera, const glm::mat4& view_proj, int view_zone) {
    int floor_zone = view_zone;
    float floor_z = 0;
    uint32_t owner = 0;
    if (view_zone == ZoneSystem::kNone)
    // (Not from a moving object: it keeps the zone it was placed in.)
    if (collision_.ray_down(camera, 8.0f, floor_z, &owner) && !moving_.count(owner)) floor_zone = zones_.object_zone(owner);
    zones_.update(camera, view_proj, floor_zone);
    for (const Instance& inst : instances_) {
        if (inst.draw_count && draws_[inst.first_draw].sky) continue;
        if (inst.never_culled) continue;
        const bool hidden = !zones_.box_visible(inst.zone, inst.world_min, inst.world_max);
        for (size_t i = 0; i < inst.draw_count; ++i) draws_[inst.first_draw + i].zone_hidden = hidden;
    }
    // Lights register with the scene through their zone: a hidden zone's lights light nothing.
    std::vector<uint8_t> on(lights_.size());
    for (size_t i = 0; i < on.size() && i < light_sources_.size(); ++i) on[i] = zones_.visible(light_sources_[i].zone);
    if (on != light_on_) {
        light_on_ = std::move(on);
        for (Instance& inst : instances_)
            if (inst.draw_count) light_instance(inst, inst.lighting.center);
    }
}

}  // namespace nova
