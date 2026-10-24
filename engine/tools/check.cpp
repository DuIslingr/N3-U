// nova3-check: exercise the engine's decoders on the real game data.
//
//   nova3-check vfs DATA_DIR                  mount every .gla, read and verify every file
//   nova3-check texture DATA_DIR NAME OUT.ppm decode one texture
//   nova3-check textures DATA_DIR             decode every texture
//   nova3-check shaders DATA_DIR [DUMP_DIR]   translate and compile every effect pass (plain and skinned)
#include <functional>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>

#include "assets/animation.h"
#include "assets/bdae.h"
#include "assets/effect.h"
#include "assets/level.h"
#include "assets/texture.h"
#include "core/vfs.h"
#include "core/zipfile.h"
#include "shader/glsl_translate.h"
#include "shader/shader_compiler.h"
#ifdef NOVA3_SHADERCROSS
#include <SDL3_shadercross/SDL_shadercross.h>
#endif

using namespace nova;

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int cmd_vfs(const std::string& dir) {
    auto t0 = std::chrono::steady_clock::now();
    Vfs vfs;
    const size_t n = vfs.mount_directory(dir);
    std::printf("mounted %zu archives, %zu files in %.2fs\n", n, vfs.entry_count(), seconds_since(t0));
    t0 = std::chrono::steady_clock::now();
    size_t bad = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < vfs.entry_count(); ++i) {
        try {
            total += vfs.read(i).size();
        } catch (const FormatError& ex) {
            if (bad++ < 10) std::printf("  %s: %s\n", vfs.entry(i).name.c_str(), ex.what());
        }
    }
    std::printf("read %zu files (%.1f MB) in %.2fs, %zu failed\n", vfs.entry_count(), total / 1e6,
                seconds_since(t0), bad);
    return bad ? 1 : 0;
}

bool is_texture(const std::string& name) {
    const std::string n = to_lower(name);
    for (const char* ext : {".pvr", ".pvr2", ".etc"}) {
        if (n.size() > std::strlen(ext) && n.compare(n.size() - std::strlen(ext), std::string::npos, ext) == 0) return true;
    }
    return false;
}

// Raw dump: u32 width, u32 height, then RGBA bytes.
int cmd_texture(const std::string& dir, const std::string& name, const std::string& out) {
    Vfs vfs;
    vfs.mount_directory(dir);
    const Image img = load_texture(vfs.read(name));
    std::ofstream f(out, std::ios::binary);
    const uint32_t wh[2] = {uint32_t(img.width), uint32_t(img.height)};
    f.write(reinterpret_cast<const char*>(wh), sizeof wh);
    f.write(reinterpret_cast<const char*>(img.rgba.data()), std::streamsize(img.rgba.size()));
    std::printf("%s: %dx%d\n", name.c_str(), img.width, img.height);
    return 0;
}

int cmd_textures(const std::string& dir) {
    Vfs vfs;
    vfs.mount_directory(dir);
    auto t0 = std::chrono::steady_clock::now();
    size_t n = 0, bad = 0;
    uint64_t pixels = 0;
    for (uint32_t i = 0; i < vfs.entry_count(); ++i) {
        if (!is_texture(vfs.entry(i).name)) continue;
        ++n;
        try {
            const Image img = load_texture(vfs.read(i));
            pixels += uint64_t(img.width) * img.height;
        } catch (const FormatError& ex) {
            if (bad++ < 10) std::printf("  %s: %s\n", vfs.entry(i).name.c_str(), ex.what());
        }
    }
    std::printf("decoded %zu textures (%.1f Mpixels) in %.2fs, %zu failed\n", n - bad, pixels / 1e6,
                seconds_since(t0), bad);
    return bad ? 1 : 0;
}

// Per-file "name meshes triangles" lines for comparison with the Python parser.
int cmd_models(const std::string& dir, const std::string& out_path) {
    Vfs vfs;
    vfs.mount_directory(dir);
    std::ofstream out(out_path);
    auto t0 = std::chrono::steady_clock::now();
    size_t n = 0, bad = 0;
    uint64_t tris = 0;
    for (uint32_t i = 0; i < vfs.entry_count(); ++i) {
        const std::string name = vfs.entry(i).name;
        if (to_lower(name).size() < 5 || to_lower(name).substr(name.size() - 5) != ".bdae") continue;
        ++n;
        try {
            const Model m = load_bdae(vfs.read(i));
            uint64_t t = 0;
            for (const auto& mesh : m.meshes)
                for (const auto& p : mesh.primitives) t += p.indices.size() / 3;
            tris += t;
            out << name << " " << m.meshes.size() << " " << t << "\n";
        } catch (const FormatError& ex) {
            out << name << " ERROR " << ex.what() << "\n";
            if (bad++ < 10) std::printf("  %s: %s\n", name.c_str(), ex.what());
        }
    }
    std::printf("loaded %zu models (%.1f M triangles) in %.2fs, %zu failed\n", n - bad, tris / 1e6,
                seconds_since(t0), bad);
    return bad ? 1 : 0;
}

// The gol types holding component `comp` (hex), with its template values ("-" for any:
// every type's component list).
int cmd_gol(const std::string& dir, const std::string& comp) {
    Vfs vfs;
    vfs.mount_directory(dir);
    const Gol gol = load_gol(vfs.read("gol.bin"));
    std::function<std::string(const Value&)> show = [&](const Value& v) -> std::string {
        switch (v.type) {
            case Value::Int: return std::to_string(v.i);
            case Value::Float: return std::to_string(v.f);
            case Value::String: return "'" + v.s + "'";
            default: {
                std::string out = "{";
                for (size_t k = 0; k < v.list.size(); ++k) out += (k ? ", " : "") + show(v.list[k]);
                return out + "}";
            }
        }
    };
    std::vector<int> ids;
    for (const auto& [id, t] : gol) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    const int c = comp == "-" ? -1 : int(std::stoul(comp, nullptr, 16));
    for (int id : ids) {
        const GolType& t = gol.at(id);
        if (c < 0) {
            std::printf("%d %s:", id, t.name.c_str());
            for (const auto& [cc, flag] : t.components) std::printf(" %02x", cc);
            std::printf("\n");
            continue;
        }
        const bool has = std::any_of(t.components.begin(), t.components.end(), [&](const auto& p) { return p.first == c; });
        if (!has) continue;
        const Value* v = resolved_template(t, gol, c);
        std::printf("%d %s: %s\n", id, t.name.c_str(), v ? show(*v).c_str() : "(per instance)");
    }
    return 0;
}

// The objects of gol type `type_name` in level `lvc`: id, position, rotation, scale, and
// their per-instance components (`comp` hex, or "-" for all of them).
int cmd_instances(const std::string& dir, const std::string& lvc, const std::string& type_name, const std::string& comp) {
    Vfs vfs;
    vfs.mount_directory(dir);
    const Gol gol = load_gol(vfs.read("gol.bin"));
    const Level level = load_level(vfs.read(lvc), gol);
    std::function<std::string(const Value&)> show = [&](const Value& v) -> std::string {
        switch (v.type) {
            case Value::Int: return std::to_string(v.i);
            case Value::Float: { char b[32]; std::snprintf(b, sizeof b, "%g", double(v.f)); return b; }
            case Value::String: return "'" + v.s + "'";
            default: {
                std::string out = "{";
                for (size_t k = 0; k < v.list.size(); ++k) out += (k ? ", " : "") + show(v.list[k]);
                return out + "}";
            }
        }
    };
    const int c = comp == "-" ? -1 : int(std::stoul(comp, nullptr, 16));
    for (const LevelObject& o : level.objects) {
        if (o.name != type_name) continue;
        if (o.base)
            std::printf("%d at (%.2f %.2f %.2f) rot (%.0f %.0f %.0f) scale (%.2f %.2f %.2f)%s", o.base->id, o.base->position.x,
                        o.base->position.y, o.base->position.z, o.base->rotation.x, o.base->rotation.y, o.base->rotation.z,
                        o.base->scale.x, o.base->scale.y, o.base->scale.z, o.base->visible ? "" : " hidden");
        for (const auto& [cc, v] : o.components)
            if (c < 0 || cc == c) std::printf(" [%02x] %s", cc, show(v).c_str());
        std::printf("\n");
    }
    return 0;
}

// The node tree of one model: name, translation, meshes, camera field of view.
int cmd_nodes(const std::string& dir, const std::string& model) {
    Vfs vfs;
    vfs.mount_directory(dir);
    const Model m = load_bdae(vfs.read(model));
    std::function<void(int, int)> walk = [&](int i, int depth) {
        const ModelNode& n = m.nodes[size_t(i)];
        std::printf("%*s%s (%.2f %.2f %.2f) meshes %zu%s\n", depth * 2, "", n.name.c_str(), n.translation.x, n.translation.y,
                    n.translation.z, n.meshes.size(), n.camera_fov > 0 ? " camera" : "");
        for (int c : n.children) walk(c, depth + 1);
    };
    for (int r : m.roots) walk(r, 0);
    return 0;
}

// Parse every level; dump "offset type kind name model x y z" lines per object.
int cmd_levels(const std::string& dir, const std::string& out_dir) {
    Vfs vfs;
    vfs.mount_directory(dir);
    auto t0 = std::chrono::steady_clock::now();
    const Gol gol = load_gol(vfs.read("gol.bin"));
    size_t ok_templates = 0;
    for (const auto& [id, t] : gol) ok_templates += t.templates_ok;
    std::printf("gol.bin: %zu types, %zu with validated templates (%.2fs)\n", gol.size(), ok_templates, seconds_since(t0));
    size_t bad = 0;
    for (uint32_t i = 0; i < vfs.entry_count(); ++i) {
        const std::string name = vfs.entry(i).name;
        if (to_lower(name).size() < 4 || to_lower(name).substr(name.size() - 4) != ".lvc") continue;
        try {
            t0 = std::chrono::steady_clock::now();
            const Level level = load_level(vfs.read(i), gol);
            const auto models = model_placements(level, gol);
            const auto cols = collision_placements(level, gol);
            std::printf("OK   %-34s %5zu objects, %4zu models, %4zu collision (%.2fs)\n", name.c_str(),
                        level.objects.size(), models.size(), cols.size(), seconds_since(t0));
            std::ofstream out(out_dir + "/" + name + ".txt");
            for (const auto& o : level.objects) {
                char pos[96] = "-";
                if (o.base) std::snprintf(pos, sizeof pos, "%.3f %.3f %.3f", o.base->position.x, o.base->position.y, o.base->position.z);
                out << o.offset << " " << o.type << " " << o.kind << " [" << o.name << "] [" << o.model << "] " << pos << "\n";
            }
        } catch (const FormatError& ex) {
            ++bad;
            std::printf("FAIL %-34s %s\n", name.c_str(), ex.what());
        }
    }
    return bad > 1 ? 1 : 0;   // menus.lvc is known not to parse yet
}

// Sample every track of an animation file at the given times (ms); one line per node and time.
int cmd_anim(const std::string& dir, const std::string& file, const std::vector<float>& times) {
    Vfs vfs;
    vfs.mount_directory(dir);
    const AnimationLibrary lib = load_animation(vfs.read(file));
    std::printf("%zu animated nodes, %zu clips, timeline %.1f-%.1f ms\n", lib.tracks.size(), lib.clips.size(),
                lib.timeline_start, lib.timeline_end);
    for (const AnimClip& c : lib.clips) std::printf("clip %s %.1f-%.1f ms\n", c.name.c_str(), c.start, c.end);
    for (const ValueTrack& vt : lib.values) {
        for (float t : times) {
            float v[16] = {};
            sample_value(vt, t, v);
            std::printf("%s %s%s first %d %.1f:", vt.visibility ? "visibility" : vt.light ? "light" : "material", vt.target.c_str(),
                        vt.light || vt.visibility ? "" : ("/" + vt.param).c_str(), vt.first, t);
            for (int c = 0; c < vt.stride && c < 16; ++c) std::printf(" %.4f", v[c]);
            std::printf("\n");
        }
    }
    std::vector<std::string> names;
    for (const auto& [n, t] : lib.tracks) names.push_back(n);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        for (float t : times) {
            NodePose p{glm::vec3(0), glm::quat(1, 0, 0, 0), glm::vec3(1)};
            sample_tracks(lib.tracks.at(n), t, p);
            std::printf("%s %.1f T %.4f %.4f %.4f R %.4f %.4f %.4f %.4f S %.4f %.4f %.4f\n", n.c_str(), t, p.translation.x,
                        p.translation.y, p.translation.z, p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w,
                        p.scale.x, p.scale.y, p.scale.z);
        }
    }
    return 0;
}

// A node's tracks: per track, its property and the key times within [from, to] ms.
int cmd_keys(const std::string& dir, const std::string& file, const std::string& node, float from, float to) {
    Vfs vfs;
    vfs.mount_directory(dir);
    const AnimationLibrary lib = load_animation(vfs.read(file));
    auto it = lib.tracks.find(node);
    if (it == lib.tracks.end()) {
        std::printf("no tracks for %s\n", node.c_str());
        return 1;
    }
    for (const AnimTrack& t : it->second) {
        std::printf("%s comp %d, %zu keys:", t.property == AnimTrack::Property::Rotation ? "rotation" : t.property == AnimTrack::Property::Scale ? "scale" : "translation",
                    t.component, t.times.size());
        for (float k : t.times)
            if (k >= from && k <= to) std::printf(" %.0f", k);
        std::printf("\n");
    }
    return 0;
}

std::string read_text(const Vfs& vfs, const std::string& name) {
    const auto& hits = vfs.find(name);
    if (hits.empty()) throw std::runtime_error("shader file not found: " + name);
    const Bytes b = vfs.read(hits.front());
    return std::string(b.begin(), b.end());
}

// SPIR-V (SDL's Vulkan layout) to MSL and HLSL, as the Metal and Direct3D 12 renderers would
// get it; the first error, or empty. Without SDL_shadercross in the build: nothing to check.
// `dump`: also write them to <dump>.msl / <dump>.hlsl.
std::string cross_check(const std::vector<uint32_t>& spirv, bool vertex, const std::string& dump = {}) {
#ifdef NOVA3_SHADERCROSS
    SDL_ShaderCross_SPIRV_Info info{};
    info.bytecode = reinterpret_cast<const Uint8*>(spirv.data());
    info.bytecode_size = spirv.size() * 4;
    info.entrypoint = "main";
    info.shader_stage = vertex ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
    for (bool msl : {true, false}) {
        void* out = msl ? SDL_ShaderCross_TranspileMSLFromSPIRV(&info) : SDL_ShaderCross_TranspileHLSLFromSPIRV(&info);
        if (!out) return std::string(msl ? "MSL: " : "HLSL: ") + SDL_GetError();
        if (!dump.empty()) std::ofstream(dump + (msl ? ".msl" : ".hlsl")) << static_cast<const char*>(out);
        SDL_free(out);
    }
#else
    (void)spirv, (void)vertex, (void)dump;
#endif
    return {};
}

int cmd_shaders(const std::string& dir, const std::string& dump_dir) {
    Vfs vfs;
    vfs.mount_directory(dir);
    ShaderCompiler compiler("");   // no cache: compile everything
    const auto t0 = std::chrono::steady_clock::now();
    size_t effects = 0, passes = 0, failed = 0;
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < vfs.entry_count(); ++i) {
        const std::string name = vfs.entry(i).name;
        if (to_lower(name).ends_with(".bdae") == false) continue;
        if (std::find(seen.begin(), seen.end(), to_lower(name)) != seen.end()) continue;
        const Bytes data = vfs.read(i);
        if (!is_zip(data)) continue;
        Effect fx;
        try {
            fx = load_effect(data);
        } catch (const FormatError&) {
            continue;
        }
        seen.push_back(to_lower(name));
        ++effects;
        for (const Technique& t : fx.techniques) {
            for (const EffectPass& p : t.passes) {
                for (bool skinned : {false, true}) {
                    ++passes;
                    try {
                        const CompiledProgram prog = compiler.compile(read_text(vfs, p.vertex_shader), p.vertex_defines,
                                                                      read_text(vfs, p.fragment_shader), p.fragment_defines,
                                                                      skinned);
                        for (bool vertex : {true, false}) {
                            const std::string dump = dump_dir.empty() ? std::string()
                                                     : dump_dir + "/" + fx.id + "." + t.name + (skinned ? ".skinned" : "") +
                                                           (vertex ? ".vert" : ".frag");
                            const std::string err = cross_check(vertex ? prog.vertex_spirv : prog.fragment_spirv, vertex, dump);
                            if (!err.empty()) throw std::runtime_error((vertex ? "vertex " : "fragment ") + err);
                        }
                        if (!dump_dir.empty() && !skinned) {
                            std::ofstream f(dump_dir + "/" + fx.id + "." + t.name + ".txt");
                            for (const auto* st : {&prog.vertex, &prog.fragment}) {
                                f << (st == &prog.vertex ? "vertex" : "fragment") << " block " << st->uniform_size << "\n";
                                for (const auto& u : st->uniforms)
                                    f << "  " << u.offset << " " << u.type << " " << u.name << (u.array ? "[" + std::to_string(u.array) + "]" : "") << "\n";
                                for (size_t k = 0; k < st->samplers.size(); ++k) f << "  sampler " << k << " " << st->sampler_types[k] << " " << st->samplers[k] << "\n";
                            }
                        }
                    } catch (const std::exception& ex) {
                        if (failed++ < 200) {
                            std::string msg = ex.what();
                            if (msg.size() > 400) msg.resize(400);
                            std::printf("FAIL %s %s/%s%s: %s\n", name.c_str(), fx.id.c_str(), t.name.c_str(),
                                        skinned ? " (skinned)" : "", msg.c_str());
                        }
                    }
                }
            }
        }
    }
    std::printf("%zu effects, %zu passes (plain + skinned), %zu failed, %zu stages compiled in %.1fs\n", effects, passes,
                failed, compiler.compiled, seconds_since(t0));
    return failed ? 1 : 0;
}

}  // namespace

// The renderer's own shaders (DIR/*.spv) through the same conversions.
int cmd_cross(const std::string& dir) {
    size_t n = 0, failed = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".spv") continue;
        std::ifstream f(e.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), {});
        std::vector<uint32_t> code(bytes.size() / 4);
        std::memcpy(code.data(), bytes.data(), code.size() * 4);
        const bool vertex = e.path().filename().string().find(".vert.") != std::string::npos;
        ++n;
        if (const std::string err = cross_check(code, vertex); !err.empty())
            ++failed, std::printf("FAIL %s: %s\n", e.path().filename().string().c_str(), err.c_str());
    }
    std::printf("%zu shaders, %zu failed\n", n, failed);
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
#ifdef NOVA3_SHADERCROSS
    if (!SDL_ShaderCross_Init()) std::fprintf(stderr, "SDL_ShaderCross_Init: %s\n", SDL_GetError());
#endif
    if (argc < 3) {
        std::fprintf(stderr, "usage: nova3-check vfs DATA_DIR\n");
        return 2;
    }
    const std::string cmd = argv[1];
    try {
        if (cmd == "vfs") return cmd_vfs(argv[2]);
        if (cmd == "textures") return cmd_textures(argv[2]);
        if (cmd == "shaders") return cmd_shaders(argv[2], argc >= 4 ? argv[3] : "");
        if (cmd == "cross") return cmd_cross(argv[2]);
        if (cmd == "models" && argc >= 4) return cmd_models(argv[2], argv[3]);
        if (cmd == "nodes" && argc >= 4) return cmd_nodes(argv[2], argv[3]);
        if (cmd == "gol" && argc >= 4) return cmd_gol(argv[2], argv[3]);
        if (cmd == "levels" && argc >= 4) return cmd_levels(argv[2], argv[3]);
        if (cmd == "instances" && argc >= 6) return cmd_instances(argv[2], argv[3], argv[4], argv[5]);
        if (cmd == "anim" && argc >= 5) {
            std::vector<float> times;
            for (int k = 4; k < argc; ++k) times.push_back(std::stof(argv[k]));
            return cmd_anim(argv[2], argv[3], times);
        }
        if (cmd == "texture" && argc >= 5) return cmd_texture(argv[2], argv[3], argv[4]);
        if (cmd == "keys" && argc >= 7) return cmd_keys(argv[2], argv[3], argv[4], std::stof(argv[5]), std::stof(argv[6]));
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
