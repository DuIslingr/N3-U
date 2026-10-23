#include "shader/glsl_translate.h"

#include <regex>
#include <sstream>
#include <stdexcept>

namespace nova {

namespace {

// Identifiers that are reserved in GLSL 4.50 but free in GLSL ES 1.00.
const char* const kReserved[] = {"texture", "sample", "input", "output", "filter", "common", "partition", "active",
                                 "smooth", "flat", "noperspective", "centroid", "patch", "subroutine", "buffer",
                                 "shared", "coherent", "volatile", "restrict", "readonly", "writeonly", "resource",
                                 "precise", "uniform_block"};
constexpr const char* kRenamePrefix = "nova_r_";

struct Decl {
    std::string qualifier;   // uniform / attribute / varying
    std::string type;
    std::string name;
    uint32_t array = 0;
};

std::string original_name(const std::string& name) {
    return name.rfind(kRenamePrefix, 0) == 0 ? name.substr(std::char_traits<char>::length(kRenamePrefix)) : name;
}

bool is_precision(const std::string& w) { return w == "lowp" || w == "mediump" || w == "highp"; }
bool is_sampler_type(const std::string& t) { return t.rfind("sampler", 0) == 0; }

// Remove preprocessor leftovers (#version, #line, #extension) and rename reserved identifiers.
std::string clean(const std::string& src) {
    std::istringstream in(src);
    std::string line, out;
    while (std::getline(in, line)) {
        size_t i = line.find_first_not_of(" \t");
        if (i != std::string::npos && line[i] == '#') continue;
        out += line;
        out += '\n';
    }
    for (const char* word : kReserved) {
        out = std::regex_replace(out, std::regex(std::string("\\b") + word + "\\b"), std::string(kRenamePrefix) + word);
    }
    return out;
}

uint32_t parse_array_size(const std::string& s) {
    try {
        size_t used = 0;
        const unsigned long v = std::stoul(s, &used, 0);
        if (used == s.size() && v > 0) return uint32_t(v);
    } catch (const std::exception&) {
    }
    throw std::runtime_error("unsupported array size [" + s + "]");
}

// Pull out every uniform/attribute/varying declaration (they can only appear at
// global scope) and the default precision statements; return the remaining code.
std::string extract_decls(const std::string& src, std::vector<Decl>& decls) {
    static const std::regex decl_re(R"(\b(uniform|attribute|varying)\b([^;{}]*);)");
    static const std::regex precision_re(R"(\bprecision\s+\w+\s+\w+\s*;)");
    std::string rest;
    auto begin = std::sregex_iterator(src.begin(), src.end(), decl_re);
    size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        rest += src.substr(last, size_t(it->position()) - last);
        last = size_t(it->position() + it->length());
        // "[precision] type name[N], name2, ..."
        std::vector<std::string> words;
        {
            std::string spaced;
            for (char c : (*it)[2].str()) {
                if (c == '[') spaced += " [";
                else if (c == ',') spaced += " , ";
                else spaced += c;
            }
            std::istringstream ws(spaced);
            std::string w;
            while (ws >> w) words.push_back(w);
        }
        size_t k = 0;
        while (k < words.size() && (is_precision(words[k]) || words[k] == "const" || words[k] == "invariant")) ++k;
        if (k >= words.size()) throw std::runtime_error("bad declaration: " + it->str());
        const std::string type = words[k++];
        while (k < words.size()) {
            if (words[k] == ",") {
                ++k;
                continue;
            }
            Decl d{(*it)[1].str(), type, words[k++], 0};
            if (k < words.size() && words[k].front() == '[') {
                std::string size = words[k++].substr(1);
                while (!size.empty() && size.back() == ']') size.pop_back();
                // An expression like "[ 30 ]" splits into several words.
                while (size.empty() && k < words.size()) {
                    size = words[k++];
                    while (!size.empty() && size.back() == ']') size.pop_back();
                }
                d.array = parse_array_size(size);
            }
            decls.push_back(d);
        }
    }
    rest += src.substr(last);
    return std::regex_replace(rest, precision_re, "");
}

std::string translate_body(std::string code) {
    static const std::pair<const char*, const char*> kFunctions[] = {
        {"texture2DProjLod", "textureProjLod"}, {"texture2DProj", "textureProj"}, {"texture2DLodEXT", "textureLod"},
        {"texture2DLod", "textureLod"}, {"textureCubeLod", "textureLod"}, {"texture2DRect", "texture"},
        {"texture2D", "texture"}, {"textureCube", "texture"}, {"texture3D", "texture"}, {"texture1D", "texture"},
    };
    for (const auto& [from, to] : kFunctions) {
        code = std::regex_replace(code, std::regex(std::string("\\b") + from + "\\s*\\("), std::string(to) + "(");
    }
    code = std::regex_replace(code, std::regex(R"(\bgl_FragData\s*\[\s*0\s*\])"), "nova_FragColor");
    code = std::regex_replace(code, std::regex(R"(\bgl_FragColor\b)"), "nova_FragColor");
    const std::regex main_re(R"(\bvoid\s+main\s*\(\s*(void)?\s*\))");
    if (!std::regex_search(code, main_re)) throw std::runtime_error("no main()");
    return std::regex_replace(code, main_re, "void nova_main()");
}

uint32_t location_count(const std::string& type, uint32_t array) {
    uint32_t n = 1;
    if (type == "mat2") n = 2;
    else if (type == "mat3") n = 3;
    else if (type == "mat4") n = 4;
    return n * std::max<uint32_t>(array, 1);
}

std::string decl_text(const std::string& type, const std::string& name, uint32_t array) {
    return type + " " + name + (array ? "[" + std::to_string(array) + "]" : "");
}

// Emit the uniform block and sampler declarations; fill `iface`.
std::string emit_resources(const std::vector<Decl>& decls, int uniform_set, int sampler_set, ShaderInterface& iface) {
    std::string out;
    std::string block;
    uint32_t offset = 0;
    for (const Decl& d : decls) {
        if (d.qualifier != "uniform") continue;
        if (is_sampler_type(d.type)) {
            if (d.array) throw std::runtime_error("sampler arrays not supported");
            if (d.type == "sampler2DRect") throw std::runtime_error("sampler2DRect not supported");
            out += "layout(set = " + std::to_string(sampler_set) + ", binding = " + std::to_string(iface.samplers.size()) +
                   ") uniform " + d.type + " " + d.name + ";\n";
            iface.samplers.push_back(original_name(d.name));
            iface.sampler_types.push_back(d.type);
            continue;
        }
        uint32_t size = 0, align = 0;
        if (!std140_type(d.type, size, align)) throw std::runtime_error("unsupported uniform type " + d.type);
        UniformField f;
        f.name = original_name(d.name);
        f.type = d.type;
        f.array = d.array;
        if (d.array) {
            f.stride = (size + 15) & ~15u;
            align = std::max<uint32_t>(align, 16);
            size = f.stride * d.array;
        }
        offset = (offset + align - 1) & ~(align - 1);
        f.offset = offset;
        offset += size;
        iface.uniforms.push_back(f);
        block += "    " + decl_text(d.type, d.name, d.array) + ";\n";
    }
    if (block.empty()) block = "    float nova_unused;\n";   // keep buffer slot 0 present in every stage
    iface.uniform_size = std::max<uint32_t>((offset + 15) & ~15u, 16);
    out += "layout(std140, set = " + std::to_string(uniform_set) + ", binding = 0) uniform NovaUniforms" +
           std::to_string(uniform_set) + " {\n" + block + "};\n";
    return out;
}

int attribute_location(const std::string& name) {
    static const std::map<std::string, int> kLocations = {
        {"Vertex", kLocPosition}, {"Normal", kLocNormal}, {"TexCoord0", kLocUv0}, {"TexCoord1", kLocUv1},
        {"TexCoord2", kLocUv2}, {"Color0", kLocColor}, {"Tangent", kLocTangent}, {"Binormal", kLocBinormal},
    };
    auto it = kLocations.find(name);
    return it == kLocations.end() ? -1 : it->second;
}

// The xyz part of an attribute of `type` (vec3 or vec4), as a vec3 expression.
std::string xyz(const std::string& type, const std::string& v) {
    if (type == "vec4") return v + ".xyz";
    if (type == "vec3") return v;
    return "vec3(" + v + ", 0.0" + (type == "vec2" ? ")" : ", 0.0)");
}

// Expression converting a vec4 object-space value to `type`.
std::string from_vec4(const std::string& type, const std::string& v) {
    if (type == "vec4") return v;
    if (type == "vec3") return v + ".xyz";
    if (type == "vec2") return v + ".xy";
    return v + ".x";
}

}  // namespace

bool std140_type(const std::string& type, uint32_t& size, uint32_t& align) {
    if (type == "float" || type == "int" || type == "bool") size = align = 4;
    else if (type == "vec2" || type == "ivec2" || type == "bvec2") size = align = 8;
    else if (type == "vec3" || type == "ivec3" || type == "bvec3") size = 12, align = 16;
    else if (type == "vec4" || type == "ivec4" || type == "bvec4") size = align = 16;
    else if (type == "mat2") size = 32, align = 16;
    else if (type == "mat3") size = 48, align = 16;
    else if (type == "mat4") size = 64, align = 16;
    else return false;
    return true;
}

TranslatedShader translate_vertex(const std::string& preprocessed, bool skinned, VaryingMap& varyings,
                                  const std::set<std::string>* keep) {
    std::vector<Decl> decls;
    const std::string body = translate_body(extract_decls(clean(preprocessed), decls));
    TranslatedShader t;
    std::string head = "#version 450\n";
    head += emit_resources(decls, 1, 0, t.iface);
    if (skinned) {
        head += "layout(std140, set = 1, binding = 1) uniform NovaJoints {\n    mat4 nova_joints[128];\n};\n";
        head += "layout(location = " + std::to_string(kLocJoints) + ") in uvec4 nova_a_joints;\n";
        head += "layout(location = " + std::to_string(kLocWeights) + ") in vec4 nova_a_weights;\n";
    }
    std::string prologue;
    if (skinned) {
        prologue += "    mat4 nova_skin = nova_joints[nova_a_joints.x] * nova_a_weights.x + "
                    "nova_joints[nova_a_joints.y] * nova_a_weights.y +\n"
                    "                     nova_joints[nova_a_joints.z] * nova_a_weights.z + "
                    "nova_joints[nova_a_joints.w] * nova_a_weights.w;\n";
    }
    int next_varying = 0;
    for (const Decl& d : decls) {
        if (d.qualifier == "attribute") {
            const int loc = attribute_location(d.name);
            head += decl_text(d.type, d.name, d.array) + ";\n";
            if (loc < 0) continue;   // not provided by our vertex format: stays zero
            head += "layout(location = " + std::to_string(loc) + ") in " + d.type + " nova_a_" + d.name + ";\n";
            const bool position = d.name == "Vertex", direction = d.name == "Normal" || d.name == "Tangent" ||
                                                                  d.name == "Binormal";
            if (skinned && position) {
                prologue += "    " + d.name + " = " + from_vec4(d.type, "(nova_skin * vec4(" + xyz(d.type, "nova_a_" + d.name) +
                                                                     ", 1.0))") + ";\n";
            } else if (skinned && direction && d.type != "float") {
                prologue += "    " + d.name + " = " + from_vec4(d.type, "vec4(mat3(nova_skin) * " + xyz(d.type, "nova_a_" + d.name) +
                                                                     ", 0.0)") + ";\n";
            } else {
                prologue += "    " + d.name + " = nova_a_" + d.name + ";\n";
            }
        } else if (d.qualifier == "varying") {
            if (keep && !keep->count(d.name)) {   // nothing reads it: a private variable
                head += decl_text(d.type, d.name, d.array) + ";\n";
                continue;
            }
            varyings[d.name] = {next_varying, decl_text(d.type, d.name, d.array)};
            head += "layout(location = " + std::to_string(next_varying) + ") out " + decl_text(d.type, d.name, d.array) + ";\n";
            next_varying += int(location_count(d.type, d.array));
        }
    }
    t.source = head + body + "\nvoid main() {\n" + prologue + "    nova_main();\n}\n";
    return t;
}

TranslatedShader translate_fragment(const std::string& preprocessed, const VaryingMap& varyings) {
    std::vector<Decl> decls;
    std::string body = translate_body(extract_decls(clean(preprocessed), decls));
    TranslatedShader t;
    std::string head = "#version 450\n";
    head += emit_resources(decls, 3, 2, t.iface);
    head += "layout(location = 0) out vec4 nova_FragColor;\n";
    // The device's copy of the screen holds nothing past the screen's edges: reads pushed out
    // there (the shield's ripple) are black, where a clamped read would smear the edge.
    for (const Decl& d : decls) {
        if (d.qualifier != "uniform" || original_name(d.name) != "ScreenTextureSampler") continue;
        head += "vec4 nova_screen_read(vec2 uv) {\n    vec4 c = texture(" + d.name +
                ", uv);\n    return any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) ? vec4(0.0) : c;\n}\n";
        body = std::regex_replace(body, std::regex("\\btexture\\s*\\(\\s*" + d.name + "\\s*,"), "nova_screen_read(");
    }
    for (const Decl& d : decls) {
        if (d.qualifier != "varying") continue;
        auto it = varyings.find(d.name);
        if (it == varyings.end()) {
            head += decl_text(d.type, d.name, d.array) + ";\n";   // never written by the vertex stage
        } else {
            head += "layout(location = " + std::to_string(it->second.first) + ") in " + decl_text(d.type, d.name, d.array) + ";\n";
        }
    }
    t.source = head + body + "\nvoid main() {\n    nova_main();\n}\n";
    return t;
}

}  // namespace nova
