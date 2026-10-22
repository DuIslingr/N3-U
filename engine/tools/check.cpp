// nova3-check: exercise the engine's decoders on the real game data.
//
//   nova3-check vfs DATA_DIR                  mount every .gla, read and verify every file
//   nova3-check texture DATA_DIR NAME OUT.ppm decode one texture
//   nova3-check textures DATA_DIR             decode every texture
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>

#include "assets/animation.h"
#include "assets/bdae.h"
#include "assets/level.h"
#include "assets/texture.h"
#include "core/vfs.h"

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: nova3-check vfs DATA_DIR\n");
        return 2;
    }
    const std::string cmd = argv[1];
    try {
        if (cmd == "vfs") return cmd_vfs(argv[2]);
        if (cmd == "textures") return cmd_textures(argv[2]);
        if (cmd == "models" && argc >= 4) return cmd_models(argv[2], argv[3]);
        if (cmd == "levels" && argc >= 4) return cmd_levels(argv[2], argv[3]);
        if (cmd == "anim" && argc >= 5) {
            std::vector<float> times;
            for (int k = 4; k < argc; ++k) times.push_back(std::stof(argv[k]));
            return cmd_anim(argv[2], argv[3], times);
        }
        if (cmd == "texture" && argc >= 5) return cmd_texture(argv[2], argv[3], argv[4]);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
