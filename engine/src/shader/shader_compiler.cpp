#include "shader/shader_compiler.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

#include <shaderc/shaderc.h>

namespace nova {

namespace {

// Bump when the translation changes in a way the cache key would not notice.
constexpr const char* kCacheVersion = "nova3-shader-1";

// Defines the game's driver adds to every shader (libNOVA3_neon.so strings). We leave
// out GLITCH_USE_HIGHP: it turns on SPLIT_ALPHA (alpha in a second texture), and our
// textures already carry alpha.
constexpr const char* kDriverDefines[] = {"GLITCH_OPENGLES_2"};

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

struct Result {
    shaderc_compilation_result_t r;
    ~Result() { shaderc_result_release(r); }
};

}  // namespace

std::string ShaderCompiler::default_cache_dir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return std::string(xdg) + "/nova3/shaders";
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) return std::string(local) + "/nova3/shaders";
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/Library/Caches/nova3/shaders";
#endif
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/.cache/nova3/shaders";
    return {};
}

ShaderCompiler::ShaderCompiler(std::string cache_dir) : cache_dir_(std::move(cache_dir)) {
    compiler_ = shaderc_compiler_initialize();
    if (!compiler_) throw std::runtime_error("shaderc_compiler_initialize failed");
    if (!cache_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir_, ec);
        if (ec) cache_dir_.clear();
    }
}

ShaderCompiler::~ShaderCompiler() { shaderc_compiler_release(compiler_); }

std::string ShaderCompiler::preprocess(const std::string& source, const std::vector<std::string>& defines, bool vertex) {
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_source_language(opts, shaderc_source_language_glsl);
    // Only macros are expanded here, so the language version does not matter; shaderc
    // refuses ES 1.00 when targeting SPIR-V.
    shaderc_compile_options_set_forced_version_profile(opts, 450, shaderc_profile_core);
    auto define = [&](const std::string& d) {
        const size_t space = d.find(' ');
        const std::string name = d.substr(0, space), value = space == std::string::npos ? "" : d.substr(space + 1);
        shaderc_compile_options_add_macro_definition(opts, name.data(), name.size(), value.data(), value.size());
    };
    for (const char* d : kDriverDefines) define(d);
    for (const std::string& d : defines) define(d);
    Result res{shaderc_compile_into_preprocessed_text(compiler_, source.data(), source.size(),
                                                       vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
                                                       vertex ? "vertex" : "fragment", "main", opts)};
    shaderc_compile_options_release(opts);
    if (shaderc_result_get_compilation_status(res.r) != shaderc_compilation_status_success) {
        throw std::runtime_error(std::string("preprocess: ") + shaderc_result_get_error_message(res.r));
    }
    return std::string(shaderc_result_get_bytes(res.r), shaderc_result_get_length(res.r));
}

std::vector<uint32_t> ShaderCompiler::to_spirv(const std::string& source, bool vertex, const std::string& what) {
    const uint64_t key = fnv1a(std::string(kCacheVersion) + (vertex ? "v" : "f") + source);
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.spv", static_cast<unsigned long long>(key));
    const std::string path = cache_dir_.empty() ? std::string() : cache_dir_ + "/" + name;
    if (!path.empty()) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f && f.tellg() > 0 && f.tellg() % 4 == 0) {
            std::vector<uint32_t> code(size_t(f.tellg()) / 4);
            f.seekg(0);
            f.read(reinterpret_cast<char*>(code.data()), std::streamsize(code.size() * 4));
            if (f) {
                ++cache_hits;
                return code;
            }
        }
    }
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_source_language(opts, shaderc_source_language_glsl);
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    shaderc_compile_options_set_optimization_level(opts, shaderc_optimization_level_performance);
    Result res{shaderc_compile_into_spv(compiler_, source.data(), source.size(),
                                        vertex ? shaderc_vertex_shader : shaderc_fragment_shader, what.c_str(), "main", opts)};
    shaderc_compile_options_release(opts);
    if (shaderc_result_get_compilation_status(res.r) != shaderc_compilation_status_success) {
        throw std::runtime_error(what + ": " + shaderc_result_get_error_message(res.r));
    }
    const auto* bytes = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(res.r));
    std::vector<uint32_t> code(bytes, bytes + shaderc_result_get_length(res.r) / 4);
    ++compiled;
    if (!path.empty()) {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size() * 4));
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
    }
    return code;
}

namespace {

// The Location decorations of a SPIR-V module's Input variables.
std::set<int> input_locations(const std::vector<uint32_t>& code) {
    constexpr uint32_t kOpVariable = 59, kOpDecorate = 71, kStorageInput = 1, kDecorationLocation = 30;
    std::set<uint32_t> inputs;
    std::map<uint32_t, int> location;
    for (size_t i = 5; i < code.size();) {
        const uint32_t count = code[i] >> 16, op = code[i] & 0xFFFF;
        if (count == 0 || i + count > code.size()) break;
        if (op == kOpVariable && count >= 4 && code[i + 3] == kStorageInput) inputs.insert(code[i + 2]);
        if (op == kOpDecorate && count >= 4 && code[i + 2] == kDecorationLocation) location[code[i + 1]] = int(code[i + 3]);
        i += count;
    }
    std::set<int> out;
    for (uint32_t id : inputs)
        if (auto it = location.find(id); it != location.end()) out.insert(it->second);
    return out;
}

}  // namespace

CompiledProgram ShaderCompiler::compile(const std::string& vertex_source, const std::vector<std::string>& vertex_defines,
                                        const std::string& fragment_source,
                                        const std::vector<std::string>& fragment_defines, bool skinned) {
    const std::string vertex_text = preprocess(vertex_source, vertex_defines, true);
    const std::string fragment_text = preprocess(fragment_source, fragment_defines, false);
    // First pass: which varyings the optimized fragment stage still reads.
    VaryingMap all;
    translate_vertex(vertex_text, skinned, all);
    const std::vector<uint32_t> probe = to_spirv(translate_fragment(fragment_text, all).source, false, "fragment");
    const std::set<int> read = input_locations(probe);
    std::set<std::string> keep;
    for (const auto& [name, v] : all)
        if (read.count(v.first)) keep.insert(name);
    // Second pass: only those cross between the stages, at matching consecutive locations.
    VaryingMap varyings;
    const TranslatedShader vs = translate_vertex(vertex_text, skinned, varyings, &keep);
    const TranslatedShader fs = translate_fragment(fragment_text, varyings);
    CompiledProgram p;
    p.vertex_spirv = to_spirv(vs.source, true, "vertex");
    p.fragment_spirv = to_spirv(fs.source, false, "fragment");
    p.vertex = vs.iface;
    p.fragment = fs.iface;
    p.skinned = skinned;
    return p;
}

}  // namespace nova
