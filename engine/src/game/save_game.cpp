#include "game/save_game.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include <SDL3/SDL.h>

#include "game/checkpoint.h"

namespace nova {

namespace {

constexpr char kMagic[4] = {'N', '3', 'S', 'V'};
constexpr uint32_t kVersion = 1;

// Little-endian fields; strings and lists carry their lengths.
struct Writer {
    std::vector<uint8_t>& out;
    void raw(const void* p, size_t n) { out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void flag(bool v) { out.push_back(v ? 1 : 0); }
    void str(const std::string& s) { u32(uint32_t(s.size())), raw(s.data(), s.size()); }
    void vec3(const glm::vec3& v) { f32(v.x), f32(v.y), f32(v.z); }
    void bytes(const std::vector<uint8_t>& b) { u32(uint32_t(b.size())), raw(b.data(), b.size()); }
};

struct Reader {
    const std::vector<uint8_t>& in;
    size_t& at;
    bool ok = true;
    bool raw(void* p, size_t n) {
        if (!ok || at + n > in.size()) return ok = false;
        std::memcpy(p, in.data() + at, n);
        at += n;
        return true;
    }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    bool flag() { uint8_t v = 0; raw(&v, 1); return v != 0; }
    // A count that the rest of the data could hold (`each` bytes at least apiece).
    uint32_t count(size_t each) {
        const uint32_t n = u32();
        if (ok && n > (in.size() - at) / std::max<size_t>(each, 1)) ok = false;
        return ok ? n : 0;
    }
    std::string str() {
        const uint32_t n = count(1);
        std::string s(n, '\0');
        raw(s.data(), n);
        return s;
    }
    glm::vec3 vec3() { const float x = f32(), y = f32(); return {x, y, f32()}; }
    std::vector<uint8_t> bytes() {
        std::vector<uint8_t> b(count(1));
        raw(b.data(), b.size());
        return b;
    }
};

void write_stats(Writer& w, const Combat::Stats& s) {
    for (size_t v : {s.targets, s.actors, s.hostile, s.shots, s.hits, s.kills, s.enemy_shots, s.headshot_kills, s.repel_kills,
                     s.paralyze_kills, s.slow_kills, s.deaths, s.criticals})
        w.u32(uint32_t(v));
    w.f32(s.damage_taken), w.f32(s.play_time);
}

void read_stats(Reader& r, Combat::Stats& s) {
    for (size_t* v : {&s.targets, &s.actors, &s.hostile, &s.shots, &s.hits, &s.kills, &s.enemy_shots, &s.headshot_kills,
                      &s.repel_kills, &s.paralyze_kills, &s.slow_kills, &s.deaths, &s.criticals})
        *v = r.u32();
    s.damage_taken = r.f32(), s.play_time = r.f32();
}

void write_post(Writer& w, const PostParams& p) {
    w.flag(p.dof), w.f32(p.dof_focus), w.f32(p.dof_range), w.f32(p.dof_factor), w.f32(p.blur_step);
    w.flag(p.height_fog), w.f32(p.fog_top), w.f32(p.fog_bottom), w.f32(p.fog_opacity), w.vec3(p.fog_color);
    w.flag(p.heat_haze), w.f32(p.heat_strength), w.f32(p.heat_overbright), w.f32(p.heat_blend), w.f32(p.heat_time);
    w.f32(p.sepia), w.f32(p.levels_white), w.f32(p.flash_threshold);
}

void read_post(Reader& r, PostParams& p) {
    p.dof = r.flag(), p.dof_focus = r.f32(), p.dof_range = r.f32(), p.dof_factor = r.f32(), p.blur_step = r.f32();
    p.height_fog = r.flag(), p.fog_top = r.f32(), p.fog_bottom = r.f32(), p.fog_opacity = r.f32(), p.fog_color = r.vec3();
    p.heat_haze = r.flag(), p.heat_strength = r.f32(), p.heat_overbright = r.f32(), p.heat_blend = r.f32(), p.heat_time = r.f32();
    p.sepia = r.f32(), p.levels_white = r.f32(), p.flash_threshold = r.f32();
    // The grading curve is made again from the checkpoint's LUT name.
}

}  // namespace

void write_combat(std::vector<uint8_t>& out, const Combat::Saved& s) {
    Writer w{out};
    w.bytes(s.owned);
    w.u32(uint32_t(s.clip.size()));
    for (int v : s.clip) w.i32(v);
    w.u32(uint32_t(s.reserve.size()));
    for (int v : s.reserve) w.i32(v);
    w.i32(s.current), w.i32(s.grenades), w.i32(s.mines), w.i32(s.power_index);
    w.u32(uint32_t(s.powers.size()));
    for (const std::string& p : s.powers) w.str(p);
    w.u32(uint32_t(s.actors.size()));
    for (const Combat::Saved::Actor& a : s.actors) w.i32(a.id), w.flag(a.dead), w.vec3(a.pos), w.f32(a.yaw);
    write_stats(w, s.stats);
}

bool read_combat(const std::vector<uint8_t>& in, size_t& at, Combat::Saved& s) {
    Reader r{in, at};
    s.owned = r.bytes();
    s.clip.resize(r.count(4));
    for (int& v : s.clip) v = r.i32();
    s.reserve.resize(r.count(4));
    for (int& v : s.reserve) v = r.i32();
    s.current = r.i32(), s.grenades = r.i32(), s.mines = r.i32(), s.power_index = r.i32();
    s.powers.resize(r.count(4));
    for (std::string& p : s.powers) p = r.str();
    s.actors.resize(r.count(21));
    for (Combat::Saved::Actor& a : s.actors) a.id = r.i32(), a.dead = r.flag(), a.pos = r.vec3(), a.yaw = r.f32();
    read_stats(r, s.stats);
    return r.ok;
}

std::vector<uint8_t> LevelLogic::checkpoint_data() const {
    std::vector<uint8_t> out;
    if (!saved_) return out;
    const Checkpoint& cp = *saved_;
    Writer w{out};
    w.i32(cp.spawn), w.u32(cp.section);
    w.u32(uint32_t(cp.objects.size()));
    for (const auto& [id, o] : cp.objects) {
        w.i32(id);
        w.flag(o.enabled), w.flag(o.visible), w.flag(o.dead), w.flag(o.interact_enabled);
        w.f32(o.health), w.str(o.state), w.vec3(o.position), w.f32(o.yaw);
        w.bytes(o.effects);
        w.flag(o.has_clip);
        if (o.has_clip) w.str(o.clip.name), w.f32(o.clip.ms), w.f32(o.clip.length), w.flag(o.clip.reverse), w.flag(o.clip.loop);
    }
    w.u32(uint32_t(cp.trigger_runs.size()));
    for (int v : cp.trigger_runs) w.i32(v);
    w.u32(uint32_t(cp.doors.size()));
    for (const Checkpoint::SavedDoor& d : cp.doors) w.i32(int(d.state)), w.flag(d.locked), w.flag(d.keep_unlocked), w.flag(d.forced_open);
    w.u32(uint32_t(cp.movers.size()));
    for (const auto& [id, at] : cp.movers) w.i32(id), w.vec3(at);
    w.u32(uint32_t(cp.vehicles.size()));
    for (const Checkpoint::SavedVehicle& v : cp.vehicles) w.i32(v.id), w.vec3(v.position), w.f32(v.yaw);
    w.str(cp.objectives.primary), w.flag(cp.objectives.primary_done);
    w.u32(uint32_t(cp.objectives.secondary.size()));
    for (const Objectives::Secondary& s : cp.objectives.secondary) w.str(s.id), w.str(s.text), w.flag(s.done);
    w.u32(uint32_t(cp.markers.size()));
    for (const ObjectiveMarker& m : cp.markers) w.i32(m.id), w.str(m.text), w.str(m.node);
    w.flag(cp.path_marker.on), w.vec3(cp.path_marker.position);
    w.f32(cp.fog_color.r), w.f32(cp.fog_color.g), w.f32(cp.fog_color.b), w.f32(cp.fog_color.a);
    w.f32(cp.fog_start), w.f32(cp.fog_end), w.f32(cp.fog_density), w.flag(cp.fog_locked);
    write_post(w, cp.post);
    w.f32(cp.heat.now), w.f32(cp.heat.max), w.flag(cp.heat.stopping), w.i32(cp.heat.object);
    w.str(cp.grading), w.flag(cp.grading_on);
    w.i32(cp.mech), w.i32(cp.vehicle), w.i32(cp.gun);
    write_combat(out, cp.combat);
    return out;
}

bool LevelLogic::load_checkpoint_data(const std::vector<uint8_t>& data) {
    if (data.empty()) return false;
    auto cp = std::make_shared<Checkpoint>();
    size_t at = 0;
    Reader r{data, at};
    cp->spawn = r.i32(), cp->section = r.u32();
    for (uint32_t n = r.count(20); n-- > 0 && r.ok;) {
        const int id = r.i32();
        Checkpoint::SavedObject& o = cp->objects[id];
        o.enabled = r.flag(), o.visible = r.flag(), o.dead = r.flag(), o.interact_enabled = r.flag();
        o.health = r.f32(), o.state = r.str(), o.position = r.vec3(), o.yaw = r.f32();
        o.effects = r.bytes();
        o.has_clip = r.flag();
        if (o.has_clip) o.clip.name = r.str(), o.clip.ms = r.f32(), o.clip.length = r.f32(), o.clip.reverse = r.flag(), o.clip.loop = r.flag();
    }
    cp->trigger_runs.resize(r.count(4));
    for (int& v : cp->trigger_runs) v = r.i32();
    cp->doors.resize(r.count(7));
    for (Checkpoint::SavedDoor& d : cp->doors) d.state = Door::State(r.i32()), d.locked = r.flag(), d.keep_unlocked = r.flag(), d.forced_open = r.flag();
    for (uint32_t n = r.count(16); n-- > 0 && r.ok;) {
        const int id = r.i32();
        cp->movers.emplace_back(id, r.vec3());
    }
    cp->vehicles.resize(r.count(20));
    for (Checkpoint::SavedVehicle& v : cp->vehicles) v.id = r.i32(), v.position = r.vec3(), v.yaw = r.f32();
    cp->objectives.primary = r.str(), cp->objectives.primary_done = r.flag();
    cp->objectives.secondary.resize(r.count(9));
    for (Objectives::Secondary& s : cp->objectives.secondary) s.id = r.str(), s.text = r.str(), s.done = r.flag();
    cp->markers.resize(r.count(12));
    for (ObjectiveMarker& m : cp->markers) m.id = r.i32(), m.text = r.str(), m.node = r.str();
    cp->path_marker.on = r.flag(), cp->path_marker.position = r.vec3();
    for (int k = 0; k < 4; ++k) cp->fog_color[k] = r.f32();
    cp->fog_start = r.f32(), cp->fog_end = r.f32(), cp->fog_density = r.f32(), cp->fog_locked = r.flag();
    read_post(r, cp->post);
    cp->heat.now = r.f32(), cp->heat.max = r.f32(), cp->heat.stopping = r.flag(), cp->heat.object = r.i32();
    cp->grading = r.str(), cp->grading_on = r.flag();
    cp->mech = r.i32(), cp->vehicle = r.i32(), cp->gun = r.i32();
    if (!r.ok || !read_combat(data, at, cp->combat)) return false;
    checkpoint_ = cp->spawn;
    saved_ = std::move(cp);
    return true;
}

std::string SaveGame::default_path() {
    char* dir = SDL_GetPrefPath("nova3", "nova3");
    if (!dir) return {};
    std::string path = std::string(dir) + "save.bin";
    SDL_free(dir);
    return path;
}

bool SaveGame::write(const std::string& path) const {
    if (path.empty()) return false;
    std::vector<uint8_t> out;
    Writer w{out};
    w.raw(kMagic, 4), w.u32(kVersion);
    w.str(level), w.bytes(checkpoint), w.flag(has_arsenal);
    if (has_arsenal) write_combat(out, arsenal);
    // Written beside, then moved over the old one, so a crash leaves one whole file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()))) return false;
    }
    return SDL_RenamePath(tmp.c_str(), path.c_str());
}

bool SaveGame::read(const std::string& path, SaveGame& out) {
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t at = 0;
    Reader r{in, at};
    char magic[4] = {};
    r.raw(magic, 4);
    if (!r.ok || std::memcmp(magic, kMagic, 4) != 0 || r.u32() != kVersion) return false;
    SaveGame s;
    s.level = r.str(), s.checkpoint = r.bytes(), s.has_arsenal = r.flag();
    if (!r.ok) return false;
    if (s.has_arsenal && !read_combat(in, at, s.arsenal)) return false;
    out = std::move(s);
    return true;
}

void SaveGame::remove(const std::string& path) {
    if (!path.empty()) SDL_RemovePath(path.c_str());
}

}  // namespace nova
