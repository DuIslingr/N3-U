#include "game/hud.h"

#include <algorithm>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "game/scene.h"
#include "render/text.h"

namespace nova {

void Hud::start() {
    // gol types HUD (nv_hud.bdae) and HUDAmmoIndicator (nv_hud_ammo.bdae); their clips
    // (hud_effect, shield) are not in the files.
    object_ = scene_.spawn_model("NV_hud.bdae", true);
    // The ammo gauge (NV_hud_ammo.bdae) sits at the panel's "ammo" node; its bar is drawn
    // here, cut to the clip's fill (CHudShieldNode::UpdatePercent).
    ammo_ = scene_.spawn_model("NV_hud_ammo.bdae", true);
    scene_.set_node_visible(ammo_, "ammo_bar", false);
    for (const std::string& n : scene_.node_names(object_)) {
        const bool hide = n.rfind("MP_", 0) == 0 || n.find("touch_zone") != std::string::npos ||
                          n.find("_active") != std::string::npos;
        if (hide) scene_.set_node_visible(object_, n, false);
    }
    visible_ = false;
    grenades_ = true;
    placed_for_ = glm::ivec2(0);
}

const Sprite* Hud::sprite(const std::string& name) {
    if (auto it = sprites_.find(name); it != sprites_.end()) return it->second.frames.empty() ? nullptr : &it->second;
    Sprite& s = sprites_[name];
    // The game picks a resolution variant (<name>_<w>x<h>.bsprite); this data has the 1024x768 ones.
    std::string base = name;
    if (base.size() > 8 && base.compare(base.size() - 8, 8, ".bsprite") == 0) base.resize(base.size() - 8);
    for (const std::string& file : {base + "_1024x768.bsprite", base + ".bsprite"}) {
        if (!scene_.vfs().exists(file)) continue;
        try {
            s = load_sprite(scene_.vfs().read(file));
        } catch (const FormatError&) {
        }
        break;
    }
    return s.frames.empty() ? nullptr : &s;
}

void Hud::draw_frame(ParticleList& out, const Sprite& s, int frame, glm::vec2 at, float scale, const glm::vec4& color) {
    if (frame < 0 || size_t(frame) >= s.frames.size()) return;
    const Sprite::Frame& f = s.frames[size_t(frame)];
    const uint32_t rgba = pack_rgba(color);
    for (int k = f.first; k < f.first + f.count && size_t(k) < s.fmodules.size(); ++k) {
        const Sprite::FModule& fm = s.fmodules[size_t(k)];
        if (size_t(fm.module) >= s.modules.size()) continue;
        const Sprite::Module& m = s.modules[size_t(fm.module)];
        if (size_t(m.image) >= s.images.size()) continue;
        const GpuTexture* tex = scene_.texture(s.images[size_t(m.image)]);
        if (!tex || tex->width <= 0) continue;
        float u0 = float(m.x) / float(tex->width), v0 = float(m.y) / float(tex->height);
        float u1 = float(m.x + m.w) / float(tex->width), v1 = float(m.y + m.h) / float(tex->height);
        if (fm.flags & 1) std::swap(u0, u1);
        if (fm.flags & 2) std::swap(v0, v1);
        const float x0 = at.x + float(fm.x) * scale, y0 = at.y + float(fm.y) * scale;
        const float x1 = x0 + float(m.w) * scale, y1 = y0 + float(m.h) * scale;
        const ParticleVertex v[4] = {{{x0, y0, 0}, {u0, v0}, rgba}, {{x1, y0, 0}, {u1, v0}, rgba},
                                     {{x1, y1, 0}, {u1, v1}, rgba}, {{x0, y1, 0}, {u0, v1}, rgba}};
        const uint32_t first = uint32_t(out.vertices.size());
        out.vertices.insert(out.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        out.batches.push_back({tex, false, first, 6});
    }
}

void Hud::place(const std::string& node, glm::vec2 px, const glm::mat4& view_proj, int w, int h) {
    glm::vec3 at;
    if (!scene_.node_position(object_, node, at)) return;
    const glm::vec4 c = view_proj * glm::vec4(at, 1);
    if (c.w <= 0) return;
    const glm::vec2 ndc(px.x / float(w) * 2 - 1, 1 - px.y / float(h) * 2);
    const glm::vec4 p = glm::inverse(view_proj) * glm::vec4(ndc, c.z / c.w, 1);
    const glm::vec3 world = glm::vec3(p) / p.w;
    const glm::mat4 parent = scene_.object_matrix(object_, "dummy_hud");
    scene_.set_node_translation(object_, node, glm::vec3(glm::inverse(parent) * glm::vec4(world, 1)));
}

void Hud::frame(FrameParams& f, int w, int h, const State& s, TextRenderer* numbers, ParticleList& sprites) {
    if (object_ == size_t(-1)) return;
    if (s.visible != visible_) {
        scene_.set_object_visible(object_, s.visible), scene_.set_object_visible(ammo_, s.visible);
        visible_ = s.visible;
    }
    if (s.grenades != grenades_) scene_.set_node_visible(object_, "grenade_button", s.grenades), grenades_ = s.grenades;
    if (!s.visible) return;
    Scene::CameraView cv;
    if (!scene_.camera_view(object_, cv)) return;
    f.hud3d = true;
    f.hud_view = glm::lookAt(cv.position, cv.position + cv.forward, cv.up);
    // Reversed depth like the world's (near and far swapped).
    f.hud_proj = glm::perspectiveRH_ZO(cv.fov, float(w) / float(std::max(h, 1)), 5000.0f, 1.0f);
    // CHud::Init: parts anchored to the screen, in pixels of a 480x320 screen scaled up.
    if (placed_for_ != glm::ivec2(w, h)) {
        placed_for_ = {w, h};
        const float sx = float(std::max(w, h)) / 480.0f, sy = float(std::min(w, h)) / 320.0f;
        const glm::mat4 vp = f.hud_proj * f.hud_view;
        place("weapons", {float(w) - 70 * sx, 40 * sy}, vp, w, h);
        place("grenade_button", {float(w) - 33 * sx, 85 * sy}, vp, w, h);
        place("pause_button", {63 * sx, 40 * sy}, vp, w, h);
    }
    const glm::mat4 vp = f.hud_proj * f.hud_view;
    auto screen = [&](const glm::vec3& p) {
        const glm::vec4 c = vp * glm::vec4(p, 1);
        const glm::vec2 ndc = glm::vec2(c) / std::max(c.w, 1e-4f);
        return glm::vec2((ndc.x * 0.5f + 0.5f) * float(w), (0.5f - ndc.y * 0.5f) * float(h));
    };
    // The ammo gauge follows the panel; its bar fills to the clip's share, left to right.
    // (CHud::Init hangs the HUDAmmoIndicator object on the "weapons" node, at its origin.)
    scene_.set_object_transform(ammo_, scene_.object_matrix(object_, "weapons"));
    Scene::Quad bar;
    if (scene_.node_quad(ammo_, "ammo_bar", bar) && bar.texture) {
        // Corners by U: the left edge has the smaller U.
        std::array<int, 4> order{0, 1, 2, 3};
        std::sort(order.begin(), order.end(), [&](int a, int b) { return bar.uv[size_t(a)].x < bar.uv[size_t(b)].x; });
        const int l0 = order[0], l1 = order[1];
        int r0 = order[2], r1 = order[3];
        if (bar.uv[size_t(r0)].y != bar.uv[size_t(l0)].y) std::swap(r0, r1);   // pair rows by V
        const float k = std::clamp(float(s.clip) / float(std::max(s.clip_size, 1)), 0.0f, 1.0f);
        const glm::vec3 p2 = glm::mix(bar.position[size_t(l0)], bar.position[size_t(r0)], k);
        const glm::vec3 p3 = glm::mix(bar.position[size_t(l1)], bar.position[size_t(r1)], k);
        const glm::vec2 t2 = glm::mix(bar.uv[size_t(l0)], bar.uv[size_t(r0)], k), t3 = glm::mix(bar.uv[size_t(l1)], bar.uv[size_t(r1)], k);
        if (k > 0) {
            const uint32_t white = pack_rgba(glm::vec4(1));
            const glm::vec2 a = screen(bar.position[size_t(l0)]), b = screen(p2), c = screen(p3), d = screen(bar.position[size_t(l1)]);
            const ParticleVertex v[4] = {{{a, 0}, bar.uv[size_t(l0)], white}, {{b, 0}, t2, white},
                                         {{c, 0}, t3, white}, {{d, 0}, bar.uv[size_t(l1)], white}};
            const uint32_t first = uint32_t(sprites.vertices.size());
            sprites.vertices.insert(sprites.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
            sprites.batches.push_back({bar.texture, false, first, 6});
        }
        // The counter under the gauge's left end.
        if (numbers) {
            const glm::vec2 left = glm::max(screen(bar.position[size_t(l0)]), screen(bar.position[size_t(l1)]));
            numbers->draw(sprites, std::to_string(s.clip) + "/" + std::to_string(s.reserve), {left.x - float(h) * 0.02f, left.y},
                          w, glm::vec4(0.75f, 0.95f, 1, 1), {0.0f, 0.0f});
        }
    }
}

}  // namespace nova
