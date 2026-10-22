#include "assets/level.h"

#include <algorithm>
#include <functional>
#include <regex>
#include <type_traits>

namespace nova {

namespace {

#include "assets/level_specs.inc"

const char* kBaseSpec = "i f f f f f f f f f c c";
const char* kMeshSpec = "s i c c c c c c c c c c c";

template <typename K, typename V, size_t N>
auto table(const std::pair<K, V> (&rows)[N]) {
    using Key = std::conditional_t<std::is_same_v<K, const char*>, std::string, K>;
    std::unordered_map<Key, std::string> m;
    for (const auto& [k, v] : rows) m.emplace(k, v);
    return m;
}

const auto& component_classes() { static auto t = table(kComponentClasses); return t; }
const auto& template_specs() { static auto t = table(kTemplateSpecs); return t; }
const auto& loader_specs() { static auto t = table(kLoaderSpecs); return t; }
const auto& always_specs() { static auto t = table(kAlwaysSpecs); return t; }
const auto& per_instance_specs() { static auto t = table(kPerInstanceSpecs); return t; }

// Inline nested <Name::Load> references.
std::string expand(const std::string& spec, int depth = 0) {
    if (depth > 16) throw FormatError("spec nesting too deep");
    static const std::regex nested("<([^>]+)>");
    std::string out;
    auto begin = std::sregex_iterator(spec.begin(), spec.end(), nested);
    size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        out.append(spec, last, it->position() - last);
        auto ls = loader_specs().find((*it)[1].str());
        if (ls == loader_specs().end()) throw FormatError("no spec for " + (*it)[1].str());
        out += expand(ls->second, depth + 1);
        last = it->position() + it->length();
    }
    out.append(spec, last);
    return out;
}

const std::string& component_spec(int comp) {
    static std::unordered_map<int, std::string> cache;
    auto it = cache.find(comp);
    if (it != cache.end()) return it->second;
    auto cls = component_classes().find(comp);
    std::string spec = cls == component_classes().end() ? "" : expand("<" + cls->second + "::Load>");
    return cache.emplace(comp, std::move(spec)).first->second;
}

const std::string& template_spec(int comp) {
    static std::unordered_map<int, std::string> cache;
    auto it = cache.find(comp);
    if (it != cache.end()) return it->second;
    auto t = template_specs().find(comp);
    if (t == template_specs().end()) throw FormatError("no template reader for component " + std::to_string(comp));
    return cache.emplace(comp, expand(t->second)).first->second;
}

Transform read_base(DictStream& st) { return transform_from_values(st.spec(kBaseSpec)); }
std::string read_mesh(DictStream& st) { return st.spec(kMeshSpec)[0].s; }

std::string strip(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// ------------------------------------------------------------------ level-structure readers

void read_nav_mesh(DictStream& st, LevelObject& o) {
    o.base = read_base(st);
    const int nverts = st.h();
    for (int k = 0; k < nverts; ++k) o.nav.vertices.push_back({st.f(), st.f(), st.f()});
    st.c();
    const int npolys = st.h();
    for (int k = 0; k < npolys; ++k) {
        std::array<int16_t, 8> p{};
        for (int a = 0; a < 6; ++a) p[a] = st.h();
        p[6] = st.c();
        p[7] = st.c();
        o.nav.polygons.push_back(p);
    }
    const int nlinks = st.H();
    for (int k = 0; k < nlinks; ++k) st.spec("h c f f f i");
    st.spec("f f f f f f H H");
    const int nnodes = st.H();
    for (int k = 0; k < nnodes; ++k) st.spec("H H f f i H H");
}

void read_nav_space(DictStream& st, LevelObject& o) {
    o.fields = st.spec("i f f f f f f");
    const uint32_t nx = uint32_t(st.i()) & 0xFFFF, ny = uint32_t(st.i()) & 0xFFFF, nz = uint32_t(st.i()) & 0xFFFF;
    st.f();
    st.skip((size_t(nx) * ny * nz + 3) >> 2);   // 2-bit occupancy grid
    const int n = st.H();
    for (int k = 0; k < n; ++k) st.spec("h h h i");
}

using Reader = std::function<void(DictStream&, LevelObject&)>;

const std::unordered_map<int, std::pair<const char*, Reader>>& level_types() {
    static const std::unordered_map<int, std::pair<const char*, Reader>> t = {
        {0x3A, {"level_properties", [](DictStream& st, LevelObject& o) {
             o.fields = st.spec("i c c c c f f f f f [c c c c f f f] f f f f [f] c c c c f f f f s c c c [s] s "
                                "c c c c c c c c s s i s s f f");
         }}},
        {0x3B, {"nav_mesh", read_nav_mesh}},
        {0x3E9, {"nav_mesh", read_nav_mesh}},
        {0x3C, {"portal", [](DictStream& st, LevelObject& o) {
             o.base = read_base(st);
             o.fields = st.spec("i i c f f i c c c c");
         }}},
        {0x3D, {"skybox", [](DictStream& st, LevelObject& o) {
             o.fields = st.spec("s i c");
             o.model = o.fields[0].s;
         }}},
        {0x3E, {"zone", [](DictStream& st, LevelObject& o) {
             o.base = read_base(st);
             o.fields = st.spec("i c c c c c f f f [c c c c c f f]");
             o.model = st.s();
             st.i();
         }}},
        {0x6C, {"spawn_point", [](DictStream& st, LevelObject& o) {
             o.base = read_base(st);
             o.fields = st.spec("c c c s i i i i i i i");
         }}},
        {0x6D, {"waypoint", [](DictStream& st, LevelObject& o) { o.fields = st.spec("i c f f f i c c c c c s f c"); }}},
        {0x7E9, {"waypoint", [](DictStream& st, LevelObject& o) { o.fields = st.spec("i c f f f i c c c c c s f c"); }}},
        {0x6E, {"zone_end", [](DictStream&, LevelObject&) {}}},
        {0x71, {"nav_space", read_nav_space}},
        {0x401, {"ai_group", [](DictStream& st, LevelObject& o) { o.fields = st.spec("i c c"); }}},
        {0x421, {"ai_region", [](DictStream& st, LevelObject& o) { o.base = read_base(st); }}},
        {0xA23, {"nav_point", [](DictStream& st, LevelObject& o) { o.fields = st.spec("i f f f c i"); }}},
        {0xA25, {"patrol_point", [](DictStream& st, LevelObject& o) { o.fields = st.spec("i f f f [i]"); }}},
        {0xBB9, {"link", [](DictStream& st, LevelObject& o) { o.fields = st.spec("i i i"); }}},
        {0xBBA, {"meta_zone", [](DictStream& st, LevelObject& o) {
             o.base = read_base(st);
             o.fields = st.spec("[i] i");
             o.model = st.s();
             st.i();
         }}},
        {0xBC1, {"occluder", [](DictStream& st, LevelObject& o) {
             o.base = read_base(st);
             o.model = read_mesh(st);
             o.fields = st.spec("f");
         }}},
        {0x1011, {"irradiance_volume", [](DictStream& st, LevelObject& o) {
             o.base = read_base(st);
             o.model = read_mesh(st);
             o.fields = st.spec("c f c f f f");
         }}},
    };
    return t;
}

void read_game_object(DictStream& st, const GolType& type, const Gol& gol, LevelObject& o) {
    for (const auto& [comp, flag] : resolved_components(type, gol)) {
        if (auto a = always_specs().find(comp); a != always_specs().end()) {
            o.components[comp] = st.spec(a->second);
        } else if (flag != 0) {
            continue;
        } else if (comp == kCompBase) {
            o.base = read_base(st);
        } else if (comp == kCompMesh) {
            o.model = read_mesh(st);
        } else if (auto p = per_instance_specs().find(comp); p != per_instance_specs().end()) {
            o.components[comp] = st.spec(p->second);
        } else {
            const std::string& spec = component_spec(comp);
            if (!spec.empty()) o.components[comp] = st.spec(spec);
        }
    }
}

}  // namespace

Transform transform_from_values(const Value& v, size_t first) {
    Transform t;
    t.id = v[first].i;
    t.position = {v[first + 1].as_float(), v[first + 2].as_float(), v[first + 3].as_float()};
    t.rotation = {v[first + 4].as_float(), v[first + 5].as_float(), v[first + 6].as_float()};
    t.scale = {v[first + 7].as_float(), v[first + 8].as_float(), v[first + 9].as_float()};
    if (v.size() > first + 10) t.visible = v[first + 10].i != 0;
    return t;
}

Gol load_gol(const Bytes& data) {
    DictStream st(data);
    if (st.h() != 0x474F || st.h() != 3) throw FormatError("not a version-3 GO library");
    const int n = st.i();
    std::vector<size_t> offsets(n);
    for (auto& off : offsets) off = size_t(st.i());
    std::vector<size_t> sorted = offsets;
    std::sort(sorted.begin(), sorted.end());
    Gol gol;
    for (size_t off : offsets) {
        st.pos = off;
        GolType t;
        t.id = st.i();
        const int np = st.i();
        for (int k = 0; k < np; ++k) t.parents.push_back(st.i());
        const int nc = st.h();
        t.name = strip(st.s());
        for (int k = 0; k < nc; ++k) {
            const int comp = st.h();
            const int flag = st.h();
            t.components.emplace_back(comp, flag);
        }
        // Templates follow in listed order; each entry must end where the next begins.
        auto next = std::upper_bound(sorted.begin(), sorted.end(), off);
        const size_t entry_end = next == sorted.end() ? st.end : *next;
        try {
            for (const auto& [comp, flag] : t.components) t.templates[comp] = st.spec(template_spec(comp));
            t.templates_ok = st.pos == entry_end;
        } catch (const FormatError&) {
            t.templates_ok = false;
        }
        if (!t.templates_ok) t.templates.clear();   // menu-only components don't validate yet
        gol[t.id] = std::move(t);
    }
    return gol;
}

Level load_level(const Bytes& data, const Gol& gol) {
    DictStream st(data);
    if (st.h() != 0x4E56) throw FormatError("missing 'NV' tag");
    if (st.h() != 3) throw FormatError("unsupported level version");
    Level level;
    level.header_count = st.i();
    level.strings = st.strings();
    for (int k = 0; k < level.header_count; ++k) {
        LevelObject o;
        o.offset = st.pos;
        o.type = st.i();
        if (o.type == -1) break;
        try {
            if (auto lt = level_types().find(o.type); lt != level_types().end()) {
                o.kind = lt->second.first;
                lt->second.second(st, o);
            } else if (auto g = gol.find(o.type); g != gol.end()) {
                o.kind = "object";
                o.name = g->second.name;
                read_game_object(st, g->second, gol, o);
            } else {
                throw FormatError("unknown object type " + std::to_string(o.type));
            }
        } catch (const FormatError& ex) {
            throw FormatError("object at " + std::to_string(o.offset) + " (type " + std::to_string(o.type) + "): " + ex.what());
        }
        level.objects.push_back(std::move(o));
    }
    // The real end check: a -1 terminator exactly at the string table.
    if (st.pos + 4 <= st.end && st.i() != -1) throw FormatError("missing end marker");
    if (st.pos != st.end) throw FormatError(std::to_string(st.end - st.pos) + " bytes left after the last object");
    return level;
}

std::vector<std::pair<int, int>> resolved_components(const GolType& type, const Gol& gol) {
    std::map<int, int> comps;
    std::function<void(const GolType&, int)> add = [&](const GolType& t, int depth) {
        if (depth > 16) return;
        for (int p : t.parents) {
            if (auto it = gol.find(p); it != gol.end()) add(it->second, depth + 1);
        }
        for (const auto& [c, f] : t.components) comps[c] = f;
    };
    add(type, 0);
    return {comps.begin(), comps.end()};
}

const Value* resolved_template(const GolType& type, const Gol& gol, int comp) {
    if (auto it = type.templates.find(comp); it != type.templates.end()) return &it->second;
    for (int p : type.parents) {
        if (auto it = gol.find(p); it != gol.end() && it->second.id != type.id) {
            if (const Value* v = resolved_template(it->second, gol, comp)) return v;
        }
    }
    return nullptr;
}

namespace {
bool is_helper_model(const std::string& model) {
    const std::string m = to_lower(model);
    return m.find("helpers_box") != std::string::npos || m.find("staticmeshplaceholder") != std::string::npos;
}

std::optional<Transform> object_transform(const LevelObject& o, const GolType& type, const Gol& gol) {
    if (o.base) return o.base;
    if (const Value* t = resolved_template(type, gol, kCompBase)) return transform_from_values(*t);
    return std::nullopt;
}
}  // namespace

std::vector<Placement> model_placements(const Level& level, const Gol& gol, bool library) {
    std::vector<Placement> out;
    for (size_t index = 0; index < level.objects.size(); ++index) {
        const LevelObject& o = level.objects[index];
        if (o.kind != "object") continue;
        auto g = gol.find(o.type);
        if (g == gol.end()) continue;
        std::string model = o.model;
        if (model.empty() && library) {
            if (const Value* t = resolved_template(g->second, gol, kCompMesh)) model = (*t)[0].s;
        }
        auto tf = library ? object_transform(o, g->second, gol) : o.base;
        if (model.empty() || !tf || is_helper_model(model)) continue;
        Placement p;
        p.object_index = index;
        p.model = model;
        p.transform = *tf;
        p.object = o.name;
        const Value* anim = nullptr;
        if (auto it = o.components.find(kCompAnimation); it != o.components.end()) anim = &it->second;
        else if (library) anim = resolved_template(g->second, gol, kCompAnimation);
        if (anim && anim->size() >= 3 && (*anim)[0].type == Value::String) {
            p.anim_file = (*anim)[0].s;
            p.anim_clip = (*anim)[1].s;
            p.anim_speed = (*anim)[2].as_float();
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Placement> collision_placements(const Level& level, const Gol& gol) {
    std::vector<Placement> out;
    for (size_t index = 0; index < level.objects.size(); ++index) {
        const LevelObject& o = level.objects[index];
        if (o.kind != "object") continue;
        auto g = gol.find(o.type);
        if (g == gol.end()) continue;
        const Value* data = nullptr;
        if (auto it = o.components.find(kCompCollision); it != o.components.end()) data = &it->second;
        else data = resolved_template(g->second, gol, kCompCollision);
        auto tf = object_transform(o, g->second, gol);
        if (data && tf && data->size() > 5 && (*data)[5].type == Value::String && !(*data)[5].s.empty()) {
            Placement p;
            p.object_index = index;
            p.model = (*data)[5].s;
            p.transform = *tf;
            p.object = o.name;
            out.push_back(std::move(p));
        }
    }
    return out;
}

std::vector<SkyboxRef> level_skyboxes(const Level& level) {
    std::vector<SkyboxRef> out;
    for (const auto& o : level.objects) {
        if (o.kind != "skybox" || o.fields.size() < 3) continue;
        out.push_back({o.fields[0].s, o.fields[1].i, o.fields[2].i != 0});
    }
    return out;
}

std::vector<LevelLight> level_lights(const Level& level, const Gol& gol) {
    std::vector<LevelLight> out;
    for (const auto& o : level.objects) {
        if (o.kind != "object") continue;
        auto g = gol.find(o.type);
        if (g == gol.end()) continue;
        const Value* v = nullptr;
        if (auto it = o.components.find(kCompLight); it != o.components.end()) v = &it->second;
        else v = resolved_template(g->second, gol, kCompLight);
        auto tf = object_transform(o, g->second, gol);
        if (!v || v->size() < 10 || !tf) continue;
        LevelLight l;
        l.object = o.name;
        l.position = tf->position;
        for (int k = 0; k < 4; ++k) l.color[k] = float(uint8_t((*v)[size_t(k)].i)) / 255.0f;
        l.intensity = (*v)[4].as_float();
        l.radius = (*v)[5].as_float();
        l.fake = (*v)[9].i != 0;
        out.push_back(l);
    }
    return out;
}

}  // namespace nova
