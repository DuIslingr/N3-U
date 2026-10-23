#include "game/loading_screen.h"

#include <algorithm>
#include <cstdlib>

#include "assets/dict_stream.h"
#include "assets/strings.h"
#include "core/vfs.h"
#include "render/text.h"

namespace nova {

namespace {

constexpr int kCompLevels = 92;   // CComponentLevels

void rect(Overlay& o, glm::vec4 r, const glm::vec4& color) {
    const uint32_t c = pack_rgba(color);
    const ColorVertex v[4] = {{{r.x, r.y, 0}, c}, {{r.z, r.y, 0}, c}, {{r.z, r.w, 0}, c}, {{r.x, r.w, 0}, c}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

float random(float lo, float hi) { return lo + (hi - lo) * float(std::rand()) / float(RAND_MAX); }

}  // namespace

std::vector<CampaignLevel> campaign_levels(const Vfs& vfs, const Gol& gol) {
    // levels/locations.bin: the locations' names, then the levels' (CLevelsManager's file list).
    std::vector<std::string> files;
    if (vfs.exists("locations.bin")) {
        DictStream d(vfs.read("locations.bin"));
        const int locations = d.i();
        for (int k = 0; k < locations; ++k) d.s();
        const int levels = d.i();
        for (int k = 0; k < levels; ++k) files.push_back(d.s());
    }
    std::vector<CampaignLevel> out;
    for (const auto& [id, type] : gol) {
        if (type.name != "GameLevels") continue;
        const Value* v = resolved_template(type, gol, kCompLevels);
        if (!v || v->size() < 1) break;
        // {name, file index, menu picture, loading picture, title, loading text, location,
        //  climate, alliance, image frames x3, locked, ?, ?}
        for (const Value& e : (*v)[0].list) {
            if (e.size() < 9 || e[1].i < 0 || size_t(e[1].i) >= files.size()) continue;
            out.push_back({files[size_t(e[1].i)], e[3].s, e[4].s, e[5].s, e[6].s, e[7].s, e[8].s});
        }
        break;
    }
    return out;
}

void LoadingScreen::begin(const CampaignLevel* level) {
    active = true, loaded = false;
    has_level_ = level != nullptr;
    if (level) level_ = *level;
    time_ = loaded_for_ = 0;
}

void LoadingScreen::update(float dt) {
    if (!active) return;
    time_ += dt;
    if (loaded) loaded_for_ += dt;
    for (Ring& r : rings_) {
        if ((r.left -= dt) <= 0) {
            r.left = random(1, 3);
            r.speed = random(40, 140) * (std::rand() & 1 ? -1.0f : 1.0f);
        }
        r.angle += r.speed * dt;
    }
}

bool LoadingScreen::press() {
    // The prompt shows a quarter of a second after the level is in; a press before it does nothing.
    if (!active || !loaded || loaded_for_ < 0.25f) return false;
    active = false;
    return true;
}

void LoadingScreen::draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& title, TextRenderer& text,
                         const StringTable& strings, int w, int h) {
    const float W = float(w), H = float(h);
    const float s = H / 640.0f;   // the 960x640 layout's pixels
    rect(overlay, {0, 0, W, H}, {0, 0, 0, 1});
    auto say = [&](const std::string& id) {
        const std::string t = strings.find("STR_" + id);
        return t.empty() ? strings.find(id) : t;
    };
    if (has_level_) {
        // The picture covering the screen, centred: the shipped size nearest the screen's.
        const std::string base = "LOADING_" + level_.picture;
        const Sprite* pic = nullptr;
        for (const char* size : {h > 1000 ? "_2560x1600" : "_1280x720", "_1280x720", "_1280x800", "_960x640", ""})
            if ((pic = hud.sprite_file(base + size + ".bsprite"))) break;
        if (pic) {
            const glm::vec4 b = Hud::frame_bounds(*pic, 0);
            const glm::vec2 size(std::max(b.z - b.x, 1.0f), std::max(b.w - b.y, 1.0f));
            const float k = std::max(W / size.x, H / size.y);
            hud.draw_frame(out, *pic, 0, glm::vec2(W, H) * 0.5f - (glm::vec2(b.x, b.y) + size * 0.5f) * k, k, glm::vec4(1));
        }
        // The title at the top; the story under the picture's middle, from the bottom up.
        title.draw(out, say(level_.title), {W * 0.5f, H * 0.03f}, int(W), glm::vec4(1), {0.5f, 0.0f});
        text.draw(out, say(level_.text), {W * 0.5f, H * 0.86f}, int(W * 0.94f), glm::vec4(1), {0.5f, 1.0f});
    }
    if (!loaded || loaded_for_ < 0.25f) {
        // "LOADING" with dots counting up, and the rings beside it in the corner.
        std::string label = say("FPS_LOADING");
        label += std::string(size_t(time_ * 4) % 4, '.');
        const glm::vec2 ring(W - 0.1f * H, H * 0.905f);
        if (const Sprite* sp = hud.sprite("loading.bsprite"))
            for (int k = 0; k < 6; ++k)
                hud.draw_frame_rotated(out, *sp, k + 1, ring, H * 0.135f / 80.0f,
                                       glm::radians(rings_[k].angle), glm::vec4(1));
        text.draw(out, label, {ring.x, ring.y}, int(W), glm::vec4(1), {0.5f, 0.5f});
    } else {
        const std::string prompt = touch ? say("FPS_LOADING_TOUCH_SCREEN") : "Press any key or click to continue";
        text.draw(out, prompt, {W * 0.5f, H - 7.5f * s}, int(W), glm::vec4(1, 1, 1, 0.6f + 0.4f * std::abs(std::sin(loaded_for_ * 3))),
                  {0.5f, 1.0f});
    }
}

}  // namespace nova
