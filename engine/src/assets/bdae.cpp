#include <cstdio>
#include "assets/bdae.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "assets/bres.h"
#include "assets/effect.h"
#include "core/zipfile.h"

namespace nova {

namespace {

// Root (glitch::collada::SCollada) collections: (count offset, pointer offset, stride).
constexpr Coll kLights{0x60, 0x64, 0x30};
constexpr Coll kCameras{0x50, 0x54, 0x30};   // +0 id, +8 field of view (radians), +0xC aspect, near, far
constexpr Coll kImages{0x68, 0x6C, 0x10};
constexpr Coll kMaterials{0x78, 0x7C, 0x24};
constexpr Coll kGeometries{0x84, 0x88, 0x10};
constexpr Coll kControllers{0x90, 0x94, 0x0C};
constexpr Coll kVisualScenes{0xB8, 0xBC, 0x10};
constexpr Coll kSceneNodes{0x08, 0x0C, 0x50};
constexpr Coll kNodeChildren{0x38, 0x3C, 0x50};
constexpr Coll kNodeInstances{0x40, 0x44, 0x08};
constexpr Coll kMaterialParams{0x10, 0x14, 0x18};
constexpr Coll kSubmeshes{0x38, 0x3C, 0x38};

constexpr uint32_t kGeometryMesh = 0;
constexpr uint32_t kInstanceCamera = 1;
constexpr uint32_t kInstanceController = 2;
constexpr uint32_t kInstanceGeometry = 3;
constexpr uint32_t kInstanceLight = 4;

// Submesh attribute slots (SMeshBuffer +0x0c..): position, normal, colour, texcoords 0-3,
// tangents 0-3, binormals 0-3.
constexpr int kSlotPosition = 0, kSlotNormal = 1, kSlotColor = 2, kSlotTexcoord0 = 4, kSlotTangent0 = 8,
              kSlotBinormal0 = 12;
constexpr uint8_t kNoSlot = 0xFF;

bool contains_any(const std::string& s, std::initializer_list<const char*> needles) {
    const std::string l = to_lower(s);
    return std::any_of(needles.begin(), needles.end(), [&](const char* n) { return l.find(n) != std::string::npos; });
}

class Loader {
public:
    explicit Loader(const Bytes& data) : b(data) {}

    Model run() {
        load_images();
        load_lights();
        load_materials();
        index_geometries();
        index_controllers();
        const auto scenes = b.array(b.root, kVisualScenes);
        if (!scenes.empty()) {
            for (uint32_t n : b.array(scenes.front(), kSceneNodes)) model.roots.push_back(load_node(n));
        } else {
            // No scene: one node drawing every mesh.
            ModelNode node;
            node.id = "root";
            for (const auto& [id, off] : geometry_offsets) {
                const int m = mesh_for(id);
                if (m >= 0) node.meshes.push_back(m);
            }
            model.nodes.push_back(std::move(node));
            model.roots.push_back(0);
        }
        return std::move(model);
    }

private:
    void load_images() {
        for (uint32_t o : b.array(b.root, kImages)) images.push_back(b.cstr(o + 8));
    }

    // SLight: +0 id, +8 type, +0xC SColor (ARGB), +0x10 intensity, +0x24/+0x28 falloff start/end.
    void load_lights() {
        for (uint32_t o : b.array(b.root, kLights)) {
            ModelLight l;
            l.id = b.cstr(o);
            l.type = int(b.u32(o + 8));
            const uint32_t c = b.u32(o + 0xC);
            l.color = glm::vec4((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24) / 255.0f;
            l.intensity = b.f32(o + 0x10);
            l.falloff_start = b.f32(o + 0x24);
            l.falloff_end = b.f32(o + 0x28);
            model.lights.push_back(l);
        }
    }

    void load_materials() {
        for (uint32_t o : b.array(b.root, kMaterials)) {
            Material m;
            m.id = b.cstr(o);
            m.effect = b.cstr(o + 8);
            for (uint32_t p : b.array(o, kMaterialParams)) {
                // +0 name, +8 type, +0xC count, +0x14 value.
                const uint32_t type = b.u32(p + 8), count = std::max<uint32_t>(b.u32(p + 0xC), 1);
                const uint32_t value = b.ptr(p + 0x14);
                if (!value) continue;
                const std::string name = b.cstr(p);
                if (type == kParamTechnique) {
                    if (name.size() > 17 && name.ends_with("/CurrentTechnique")) m.technique = b.cstr(value + 4);
                    continue;
                }
                if (!is_sampler(type)) {
                    const size_t n = type == kParamFloat ? 1 : type == kParamVec2 ? 2 : type == kParamVec3 ? 3
                                   : type == kParamVec4 ? 4 : type == kParamMat4 ? 16 : 0;
                    if (!n || value + 4 * n * count > b.data.size()) continue;
                    MaterialParam mp{name, type, {}, {}};
                    for (size_t k = 0; k < n * count; ++k) mp.value.push_back(b.f32(value + 4 * uint32_t(k)));
                    m.params.push_back(std::move(mp));
                    continue;
                }
                const uint32_t image_ref = b.ptr(value);
                const int32_t idx = image_ref ? b.i32(image_ref) : -1;
                if (idx < 0 || size_t(idx) >= images.size()) continue;
                m.params.push_back({name, type, {}, images[idx]});
                if (name == "DiffuseTexture" || (name == "DiffuseMap" && m.diffuse.empty())) m.diffuse = images[idx];
                else if (name == "LightmapTexture") m.lightmap = images[idx];
                else if (name == "NormalTexture") m.normal = images[idx];
            }
            m.blend = contains_any(m.effect, {"glass", "alpha", "additive", "coronas", "glow", "modulate", "hologram", "sun"});
            m.vertex_color = contains_any(m.effect, {"vc"});
            material_index[m.id] = int(model.materials.size());
            model.materials.push_back(std::move(m));
        }
    }

    void index_geometries() {
        for (uint32_t o : b.array(b.root, kGeometries)) {
            if (b.u32(o + 8) == kGeometryMesh) geometry_offsets.emplace_back(b.cstr(o), b.ptr(o + 0x0C));
        }
    }

    void index_controllers() {
        // Type 0 controllers are skins, type 1 morphs.
        for (uint32_t o : b.array(b.root, kControllers)) {
            if (b.u32(o) == 0) controllers[b.cstr(o + 4)] = b.ptr(o + 8);
            else if (b.u32(o) == 1) morphs[b.cstr(o + 4)] = b.ptr(o + 8);
        }
    }

    // A morph controller's mesh: its source geometry plus the targets (SMorph: +0 source
    // url, +4 method (0 normalized, 1 relative), +8/+0xC target geometry ids).
    int morph_for(const std::string& id, uint32_t morph) {
        const std::string key = id + "#morph";
        if (auto cached = mesh_index.find(key); cached != mesh_index.end()) return cached->second;
        int result = -1;
        std::string source = b.cstr(morph);
        if (!source.empty() && source[0] == '#') source.erase(0, 1);
        auto geometry = [&](const std::string& gid) -> uint32_t {
            for (const auto& [g, off] : geometry_offsets)
                if (g == gid) return off;
            return 0;
        };
        if (const uint32_t off = geometry(source)) {
            Mesh mesh = load_mesh(source, off, 0);
            mesh.morph_id = id;
            mesh.morph_relative = b.u32(morph + 4) == 1;
            const uint32_t stream = b.u32(off + 4);
            for (uint32_t t : b.array(morph, {0x08, 0x0C, 4})) {
                MorphTarget mt;
                const uint32_t toff = geometry(b.cstr(t));
                if (toff && b.u32(toff + 4) == stream) {   // the same vertices, moved
                    const Mesh target = load_mesh(b.cstr(t), toff, 0, true);
                    if (!target.primitives.empty())
                        for (const Vertex& v : target.primitives[0].vertices) mt.positions.push_back(v.position), mt.normals.push_back(v.normal);
                }
                mesh.morph_targets.push_back(std::move(mt));   // one that did not load keeps its weight's slot
            }
            if (!mesh.primitives.empty()) {
                result = int(model.meshes.size());
                model.meshes.push_back(std::move(mesh));
            }
        }
        return mesh_index[key] = result;
    }

    // Mesh for a geometry, optionally skinned by the skin struct at `skin` (0 = static).
    int mesh_for(const std::string& geometry_id, uint32_t skin = 0) {
        const std::string key = geometry_id + "#" + std::to_string(skin);
        auto cached = mesh_index.find(key);
        if (cached != mesh_index.end()) return cached->second;
        int result = -1;
        for (const auto& [id, off] : geometry_offsets) {
            if (id == geometry_id && off) {
                Mesh mesh = load_mesh(id, off, skin);
                if (!mesh.primitives.empty()) {
                    result = int(model.meshes.size());
                    model.meshes.push_back(std::move(mesh));
                }
                break;
            }
        }
        mesh_index[key] = result;
        return result;
    }

    // Skin struct (CColladaDatabase skin controller): +0/+4 inverse bind floats,
    // +0x4c/+0x50 joint names, +0x54/+0x58 per-vertex influences (4 x u8 joint, then up to
    // 4 x f32 weight: the buffer's length in words over the vertex count says how many).
    int skin_for(uint32_t skin) {
        auto cached = skin_index.find(skin);
        if (cached != skin_index.end()) return cached->second;
        Skin sk;
        for (uint32_t o : b.array(skin, {0x4C, 0x50, 4})) sk.joints.push_back(b.cstr(o));
        const uint32_t nfloats = b.u32(skin), mats = b.ptr(skin + 4);
        if (sk.joints.size() > size_t(kMaxJoints) || nfloats != sk.joints.size() * 16 || !mats) return skin_index[skin] = -1;
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            glm::mat4 m;
            for (int k = 0; k < 16; ++k) m[k / 4][k % 4] = b.f32(mats + 4 * (16 * j + k));   // column-major
            sk.inverse_bind.push_back(m);
        }
        const int index = int(model.skins.size());
        model.skins.push_back(std::move(sk));
        return skin_index[skin] = index;
    }

    // `whole_stream`: one primitive holding every vertex of the geometry's stream, in order
    // (a morph target's positions and normals).
    Mesh load_mesh(const std::string& id, uint32_t o, uint32_t skin_off, bool whole_stream = false) {
        Mesh mesh;
        mesh.id = id;
        uint32_t influences = 0;
        if (skin_off) {
            mesh.skin = skin_for(skin_off);
            if (mesh.skin >= 0) influences = b.ptr(skin_off + 0x58);
            if (influences) influences += 4;   // skip the buffer's u32 prefix
        }
        const size_t joint_count = mesh.skin >= 0 ? model.skins[size_t(mesh.skin)].joints.size() : 0;
        const bool interleaved = b.u32(o) != 0;
        const uint32_t vcount = b.u32(o + 4);
        // Influence records: 20 bytes (4 weights) mostly, 16 (3) in some meshes (Boreas_Thug_A's LOD0).
        const int influence_weights = influences && vcount ? std::clamp(int(b.u32(skin_off + 0x54) / vcount) - 1, 1, 4) : 4;
        const size_t influence_size = 4 + 4 * size_t(influence_weights);
        std::vector<uint32_t> strides, offsets, types, comps;
        for (uint32_t x : b.array(o, {0x08, 0x0C, 4})) strides.push_back(b.u32(x));
        for (uint32_t x : b.array(o, {0x10, 0x14, 4})) offsets.push_back(b.u32(x));
        for (uint32_t x : b.array(o, {0x18, 0x1C, 4})) types.push_back(b.u32(x));
        for (uint32_t x : b.array(o, {0x20, 0x24, 4})) comps.push_back(b.u32(x));
        const uint32_t vbuf = b.ptr(o + 0x30);
        if (!vbuf || offsets.empty() || strides.empty()) return mesh;
        const uint32_t vdata = vbuf + 4;   // buffers start with a u32 prefix

        // Component reader for attribute `a`, vertex `v`, component `k`, as float.
        auto comp = [&](size_t a, uint32_t v, uint32_t k) -> float {
            const uint32_t stride = interleaved ? strides[0] : strides[a];
            const uint32_t t = types[a];
            const size_t size = t == 6 ? 4 : 1;
            const size_t off = vdata + offsets[a] + size_t(v) * stride + k * size;
            check_range(b.data.size(), off, size);
            if (t == 6) return load_le<float>(&b.data[off]);
            if (t == 0) return float(int8_t(b.data[off])) / 127.0f;
            return float(b.data[off]) / 255.0f;
        };

        for (uint32_t s : b.array(o, kSubmeshes)) {
            if (b.u32(s) != 0) continue;   // triangle lists only
            const uint8_t* slots = &b.data[s + 0x0C];
            uint32_t min_index = b.u32(s + 0x20), max_index = b.u32(s + 0x24);
            const uint32_t count = b.u32(s + 0x28), ibuf = b.ptr(s + 0x2C);
            if (!ibuf || !count || max_index < min_index || max_index >= vcount || slots[kSlotPosition] == kNoSlot) continue;
            if (whole_stream) min_index = 0, max_index = vcount - 1;
            Primitive prim;
            prim.first_vertex = min_index;
            const std::string mat = b.cstr(s + 4);
            auto mi = material_index.find(mat);
            prim.material = mi == material_index.end() ? -1 : mi->second;
            const uint32_t n = max_index - min_index + 1;
            prim.vertices.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t v = min_index + i;
                Vertex& out = prim.vertices[i];
                const size_t pa = slots[kSlotPosition];
                out.position = {comp(pa, v, 0), comp(pa, v, 1), comp(pa, v, 2)};
                if (slots[kSlotNormal] != kNoSlot) {
                    const size_t na = slots[kSlotNormal];
                    glm::vec3 nrm{comp(na, v, 0), comp(na, v, 1), comp(na, v, 2)};
                    const float len = glm::length(nrm);
                    out.normal = len > 0 ? nrm / len : glm::vec3(0, 0, 1);
                }
                if (slots[kSlotColor] != kNoSlot) {
                    const size_t ca = slots[kSlotColor];
                    uint32_t c = 0;
                    for (uint32_t k = 0; k < 4; ++k) c |= uint32_t(comp(ca, v, k) * 255.0f + 0.5f) << (8 * k);
                    out.color = c;
                }
                if (slots[kSlotTexcoord0] != kNoSlot) {
                    out.uv0 = {comp(slots[kSlotTexcoord0], v, 0), comp(slots[kSlotTexcoord0], v, 1)};
                }
                if (slots[kSlotTexcoord0 + 1] != kNoSlot) {
                    out.uv1 = {comp(slots[kSlotTexcoord0 + 1], v, 0), comp(slots[kSlotTexcoord0 + 1], v, 1)};
                }
                if (slots[kSlotTexcoord0 + 2] != kNoSlot) {
                    out.uv2 = {comp(slots[kSlotTexcoord0 + 2], v, 0), comp(slots[kSlotTexcoord0 + 2], v, 1)};
                }
                if (slots[kSlotTangent0] != kNoSlot) {
                    const size_t ta = slots[kSlotTangent0];
                    out.tangent = {comp(ta, v, 0), comp(ta, v, 1), comp(ta, v, 2)};
                }
                if (slots[kSlotBinormal0] != kNoSlot) {
                    const size_t ba = slots[kSlotBinormal0];
                    out.binormal = {comp(ba, v, 0), comp(ba, v, 1), comp(ba, v, 2)};
                }
                if (influences) {
                    const size_t rec = influences + size_t(v) * influence_size;
                    check_range(b.data.size(), rec, influence_size);
                    float w[4], sum = 0;
                    uint8_t j[4];
                    for (int k = 0; k < 4; ++k) {
                        j[k] = b.data[rec + k];
                        w[k] = k < influence_weights ? std::max(load_le<float>(&b.data[rec + 4 + 4 * k]), 0.0f) : 0.0f;
                        if (j[k] >= joint_count || !std::isnormal(w[k])) w[k] = 0;   // no such joint: nothing
                        if (w[k] == 0) j[k] = 0;
                        sum += w[k];
                    }
                    if (sum <= 0) w[0] = sum = 1;
                    // Quantize to unorm8 so the four weights still sum to exactly 255.
                    int q[4], total = 0;
                    for (int k = 0; k < 4; ++k) total += q[k] = int(w[k] / sum * 255.0f + 0.5f);
                    q[int(std::max_element(w, w + 4) - w)] += 255 - total;
                    out.joints = uint32_t(j[0]) | uint32_t(j[1]) << 8 | uint32_t(j[2]) << 16 | uint32_t(j[3]) << 24;
                    out.weights = uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24;
                }
            }
            // Indices are relative to min_index; 16-bit unless the range needs 32.
            const bool wide = max_index - min_index >= 0xFFFF;
            const uint32_t idata = ibuf + 4;
            check_range(b.data.size(), idata, size_t(count) * (wide ? 4 : 2));
            prim.indices.resize(count);
            bool ok = true;
            for (uint32_t i = 0; i < count; ++i) {
                prim.indices[i] = wide ? load_le<uint32_t>(&b.data[idata + 4 * i]) : load_le<uint16_t>(&b.data[idata + 2 * i]);
                ok &= prim.indices[i] < n;
            }
            if (ok && count % 3 == 0) mesh.primitives.push_back(std::move(prim));
        }
        return mesh;
    }

    int load_node(uint32_t o) {
        ModelNode node;
        node.id = b.cstr(o);
        node.name = b.cstr(o + 4);
        node.sid = b.cstr(o + 8);
        node.translation = {b.f32(o + 0x0C), b.f32(o + 0x10), b.f32(o + 0x14)};
        node.rotation = glm::quat(b.f32(o + 0x24), b.f32(o + 0x18), b.f32(o + 0x1C), b.f32(o + 0x20));
        node.scale = {b.f32(o + 0x28), b.f32(o + 0x2C), b.f32(o + 0x30)};
        if (const uint32_t extra = b.ptr(o + 0x48)) node.properties = b.cstr(extra);
        for (uint32_t inst : b.array(o, kNodeInstances)) {
            const uint32_t type = b.u32(inst), ip = b.ptr(inst + 4);
            if (!ip) continue;
            std::string url = b.cstr(ip + 4);
            if (!url.empty() && url[0] == '#') url.erase(0, 1);
            int m = -1;
            if (type == kInstanceCamera) {
                for (uint32_t c : b.array(b.root, kCameras))
                    if (b.cstr(c) == url) node.camera_fov = b.f32(c + 8);
                if (node.camera_fov <= 0) node.camera_fov = 1.0f;
                continue;
            }
            if (type == kInstanceLight) {
                for (size_t li = 0; li < model.lights.size(); ++li)
                    if (model.lights[li].id == url) node.lights.push_back(int(li));
                continue;
            }
            if (type == kInstanceGeometry) {
                m = mesh_for(url);
            } else if (type == kInstanceController) {
                auto it = controllers.find(url);
                if (auto mo = morphs.find(url); mo != morphs.end() && mo->second) {
                    m = morph_for(url, mo->second);
                } else if (it != controllers.end() && it->second) {
                    std::string source = b.cstr(it->second + 0x48);
                    if (!source.empty() && source[0] == '#') source.erase(0, 1);
                    m = mesh_for(source, it->second);
                }
            }
            if (m >= 0) node.meshes.push_back(m);
        }
        const int index = int(model.nodes.size());
        model.nodes.push_back(std::move(node));
        for (uint32_t c : b.array(o, kNodeChildren)) {
            const int child = load_node(c);
            model.nodes[index].children.push_back(child);
        }
        return index;
    }

    Bres b;
    Model model;
    std::vector<std::string> images;
    std::unordered_map<std::string, int> material_index;
    std::vector<std::pair<std::string, uint32_t>> geometry_offsets;
    std::unordered_map<std::string, uint32_t> controllers, morphs;
    std::unordered_map<std::string, int> mesh_index;
    std::unordered_map<uint32_t, int> skin_index;
};

}  // namespace

Model load_bdae(const Bytes& data) {
    if (is_zip(data)) {
        // Effect files: a ZIP with builds per endianness; the engine reads the little-endian one.
        for (const char* name : {"little_endian_not_quantized.bdae", "little_endian_quantized.bdae"}) {
            const Bytes inner = unzip_member(data, name);
            if (!inner.empty()) return Loader(inner).run();
        }
        throw FormatError("ZIP-wrapped BDAE without a little-endian build");
    }
    return Loader(data).run();
}

}  // namespace nova
