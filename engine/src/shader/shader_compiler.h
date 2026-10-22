// Runtime compilation of the game's GLSL ES shaders to SPIR-V (shaderc), with an
// on-disk cache keyed by the translated source.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shader/glsl_translate.h"

struct shaderc_compiler;

namespace nova {

struct CompiledProgram {
    std::vector<uint32_t> vertex_spirv, fragment_spirv;
    ShaderInterface vertex, fragment;
    bool skinned = false;
};

class ShaderCompiler {
public:
    // `cache_dir` may be empty to disable the cache.
    explicit ShaderCompiler(std::string cache_dir);
    ~ShaderCompiler();
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    // Compile a vertex/fragment pair from GLSL ES sources and the pass's #defines
    // ("NAME" or "NAME VALUE"). Throws std::runtime_error with the compiler log.
    CompiledProgram compile(const std::string& vertex_source, const std::vector<std::string>& vertex_defines,
                            const std::string& fragment_source, const std::vector<std::string>& fragment_defines,
                            bool skinned);

    size_t compiled = 0, cache_hits = 0;   // shader stages, for statistics

    // Default cache location: $XDG_CACHE_HOME/nova3/shaders (or ~/.cache/...).
    static std::string default_cache_dir();

private:
    std::string preprocess(const std::string& source, const std::vector<std::string>& defines, bool vertex);
    std::vector<uint32_t> to_spirv(const std::string& source, bool vertex, const std::string& what);

    shaderc_compiler* compiler_ = nullptr;
    std::string cache_dir_;
};

}  // namespace nova
