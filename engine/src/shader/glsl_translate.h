// Translation of the game's GLSL ES 1.00 shaders to Vulkan GLSL 4.50.
//
// The input is a shader after preprocessing (the effect pass's #defines applied).
// Loose uniforms move into one std140 block per stage, samplers get explicit
// bindings, attributes and varyings get locations, and gl_FragColor becomes an
// output. The original main() is kept as nova_main(); a new main() first fills the
// attribute globals the shader reads (skinning them when asked), so the game's
// shader code itself is untouched.
//
// SDL GPU resource sets: vertex samplers set 0, vertex uniforms set 1, fragment
// samplers set 2, fragment uniforms set 3.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nova {

// Vertex attribute locations shared by every pipeline (see Renderer).
enum VertexLocation : int {
    kLocPosition = 0, kLocNormal = 1, kLocUv0 = 2, kLocUv1 = 3, kLocColor = 4, kLocJoints = 5, kLocWeights = 6,
    kLocUv2 = 7, kLocTangent = 8, kLocBinormal = 9,
};

struct UniformField {
    std::string name;
    std::string type;          // GLSL type, e.g. "vec3", "mat4"
    uint32_t array = 0;        // element count, 0 for a plain value
    uint32_t offset = 0;       // std140 offset in the stage's block
    uint32_t stride = 0;       // array element stride (std140: 16-byte multiples)
};

struct ShaderInterface {
    std::vector<UniformField> uniforms;
    uint32_t uniform_size = 0;                 // block size, 0 if there are no uniforms
    std::vector<std::string> samplers;         // in binding order
    std::vector<std::string> sampler_types;    // "sampler2D", "samplerCube", ...
};

struct TranslatedShader {
    std::string source;
    ShaderInterface iface;
};

// Varying name -> (location, type). The vertex stage fills it; the fragment stage reads it.
using VaryingMap = std::map<std::string, std::pair<int, std::string>>;

// `skinned`: blend Vertex/Normal/Tangent/Binormal with a joint palette (uniform buffer
// slot 1, mat4[128]) into object space before the game's code runs.
TranslatedShader translate_vertex(const std::string& preprocessed, bool skinned, VaryingMap& varyings);
TranslatedShader translate_fragment(const std::string& preprocessed, const VaryingMap& varyings);

// Size and base alignment of a std140 member of `type` (arrays: per element).
bool std140_type(const std::string& type, uint32_t& size, uint32_t& align);

}  // namespace nova
