#include "render/material.h"

#include <algorithm>
#include <cstring>
#include <regex>
#include <unordered_map>

#include "core/bytes.h"

namespace nova {

bool semantic_for(const std::string& uniform, Semantic& semantic, int& light) {
    using S = Semantic;
    // Glitch accepts several spellings per semantic; these cover the game's shaders.
    static const std::unordered_map<std::string, Semantic> kNames = {
        {"worldviewprojectionmatrix", S::WorldViewProjection}, {"worldviewprojection", S::WorldViewProjection},
        {"matmodelviewprojection", S::WorldViewProjection}, {"wvp", S::WorldViewProjection},
        {"world", S::World}, {"worldmatrix", S::World}, {"matworld", S::World}, {"matmodel", S::World},
        {"worldinverse", S::WorldInverse}, {"matworldi", S::WorldInverse}, {"matworldinverse", S::WorldInverse},
        {"worldinversetransposed", S::WorldInverseTransposed}, {"worldit", S::WorldInverseTransposed},
        {"matworldit", S::WorldInverseTransposed},
        {"worldview", S::WorldView}, {"matworldview", S::WorldView}, {"worldviewt", S::WorldViewTransposed},
        {"worldviewit", S::WorldViewInverseTransposed}, {"matworldviewit", S::WorldViewInverseTransposed},
        {"worldviewinverse", S::WorldViewInverse},
        {"view", S::View}, {"matview", S::View}, {"viewinverse", S::ViewInverse}, {"matviewi", S::ViewInverse},
        {"viewt", S::ViewTransposed}, {"projection", S::Projection}, {"viewprojection", S::ViewProjection},
        {"cameraposos", S::CameraPositionObject}, {"camerapositionos", S::CameraPositionObject},
        {"eyepositionos", S::CameraPositionObject},
        {"camerapos", S::CameraPositionWorld}, {"cameraposition", S::CameraPositionWorld},
        {"eyeposition", S::CameraPositionWorld}, {"vviewposition", S::CameraPositionWorld},
        {"wcameradir", S::CameraDirectionWorld},
        {"time", S::Time},
        {"fogcolor", S::FogColor}, {"fogstartend", S::FogStartEnd}, {"fogdensity", S::FogDensity},
        {"sceneambientlight", S::SceneAmbient}, {"texlodbias", S::Zero},
        {"ird_cc", S::IrradianceC}, {"ird_specdir", S::IrradiancePeakDirection},
        {"ird_specintensity", S::IrradiancePeakIntensity},
    };
    const std::string name = to_lower(uniform);
    if (auto it = kNames.find(name); it != kNames.end()) {
        semantic = it->second;
        light = 0;
        return true;
    }
    static const std::regex ird_re("ird_c([ab])([rgb])");
    std::smatch im;
    if (std::regex_match(name, im, ird_re)) {
        semantic = im[1].str() == "a" ? S::IrradianceA : S::IrradianceB;
        light = im[2].str() == "r" ? 0 : im[2].str() == "g" ? 1 : 2;
        return true;
    }
    static const std::regex light_re(
        "light([0-9])(positionos|position|attenuation|diffusecolormultiplied|diffusecolor|ambientcolormultiplied|"
        "ambientcolor|specularcolormultiplied|specularcolor)");
    std::smatch m;
    if (!std::regex_match(name, m, light_re)) return false;
    light = m[1].str()[0] - '0';
    const std::string what = m[2].str();
    if (what == "positionos") semantic = S::LightPositionObject;
    else if (what == "position") semantic = S::LightPositionWorld;
    else if (what == "attenuation") semantic = S::LightAttenuation;
    else if (what.rfind("diffuse", 0) == 0) semantic = S::LightDiffuse;
    else if (what.rfind("ambient", 0) == 0) semantic = S::LightAmbient;
    else semantic = S::LightSpecular;
    return true;
}

bool object_override_for(const std::string& param, Semantic& semantic) {
    using S = Semantic;
    static const std::unordered_map<std::string, Semantic> kParams = {
        {"SpecDirection", S::SpecDirection}, {"SpecColor", S::SpecColor}, {"RimFactor", S::RimFactor},
        {"L0_Pos", S::L0PositionObject}, {"L0_PosW", S::L0PositionWorld}, {"L0_Color", S::L0Color},
        {"L0_Att", S::L0Attenuation}, {"L0_LScale", S::L0Scale},
    };
    auto it = kParams.find(param);
    if (it == kParams.end()) return false;
    semantic = it->second;
    return true;
}

void write_uniform(std::vector<uint8_t>& block, uint32_t offset, const std::string& type, const float* v, size_t n) {
    auto put = [&](uint32_t at, const void* src, size_t bytes) {
        if (at + bytes <= block.size()) std::memcpy(block.data() + at, src, bytes);
    };
    if (type == "int" || type == "bool" || type.rfind("ivec", 0) == 0 || type.rfind("bvec", 0) == 0) {
        const size_t comps = type == "int" || type == "bool" ? 1 : size_t(type.back() - '0');
        for (size_t k = 0; k < std::min(n, comps); ++k) {
            const int32_t i = type[0] == 'b' || type == "bool" ? int32_t(v[k] != 0) : int32_t(v[k]);
            put(offset + uint32_t(4 * k), &i, 4);
        }
        return;
    }
    if (type == "mat2" || type == "mat3" || type == "mat4") {
        const size_t dim = size_t(type.back() - '0');
        // Values are column-major 4x4 (glm) when n == 16, else tightly packed dim x dim.
        for (size_t c = 0; c < dim; ++c) {
            for (size_t r = 0; r < dim; ++r) {
                const size_t src = n == 16 ? c * 4 + r : c * dim + r;
                if (src < n) put(offset + uint32_t(16 * c + 4 * r), &v[src], 4);
            }
        }
        return;
    }
    size_t comps = 1;
    if (type.rfind("vec", 0) == 0) comps = size_t(type.back() - '0');
    put(offset, v, 4 * std::min(n, comps));
}

}  // namespace nova
