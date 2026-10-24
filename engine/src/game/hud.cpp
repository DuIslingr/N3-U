#include "game/hud.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

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
        // CHud::Init registers weapons_background_idle as both the weapons button's idle and
        // pressed node, so SetButtonState leaves it shown only while pressed; the panel at rest
        // is weapons_background.
        const bool hide = n.rfind("MP_", 0) == 0 || n.find("touch_zone") != std::string::npos ||
                          n.find("_active") != std::string::npos || n == "weapons_background_idle";
        if (hide) scene_.set_node_visible(object_, n, false);
    }
    visible_ = false;
    grenades_ = true;
    placed_for_ = glm::ivec2(0);
    // HUDShield (nv_hud_shield.bdae): the bar's elements go out along its "shield" clip as
    // the shield (the player's health) drops (CHudShieldNode::UpdatePercent).
    shield_ = scene_.spawn_model("nv_hud_shield.bdae", true, "nv_hud_shield.bdae", "shield");
    shield_shown_ = -1, core_shown_ = -1;
    if (shield_ != size_t(-1)) scene_.play_clip(shield_, "shield", false);
    // HUDShield_Jeep (nv_hud_shield_jeep.bdae): in the jeep, its "shield_jeep" clip posed by
    // the jeep's health (CHud::SetShieldJeepPercent).
    jeep_shield_ = scene_.spawn_model("nv_hud_shield_jeep.bdae", true, "nv_hud_shield_jeep.bdae", "shield_jeep");
    jeep_shown_ = -1, jeep_visible_ = false;
    if (jeep_shield_ != size_t(-1)) scene_.play_clip(jeep_shield_, "shield_jeep", false), scene_.set_object_visible(jeep_shield_, false);
    // HUD_Frost: its clip runs as the frost spreads.
    frost_object_ = scene_.spawn_model("hud_frost.bdae", true, "hud_frost.bdae", "hud_frost");
    if (frost_object_ != size_t(-1)) {   // made for the HUD camera as it is
        scene_.play_clip(frost_object_, "hud_frost", false);
        scene_.set_object_visible(frost_object_, false);
    }
    frost_ = frost_target_ = 0, frost_max = 1, frost_shown_ = false;
    damage_ = scene_.spawn_model("hud_damage.bdae", true, "hud_damage.bdae", "hud_damage");
    blood_ = scene_.spawn_model("hud_blood_2.bdae", true, "hud_blood_2.bdae", "hud_blood");
    scratch_ = scene_.spawn_model("nv_hud_scratch.bdae", true, "nv_hud_scratch.bdae", "spider_scratch_idle");
    for (size_t o : {damage_, blood_, scratch_})
        if (o != size_t(-1)) scene_.set_object_visible(o, false);
    damage_shown_ = blood_shown_ = blood_fading_ = scratch_shown_ = false;
}

void Hud::damage_effect() {
    if (damage_ == size_t(-1)) return;
    scene_.play_clip(damage_, "hud_damage", false);
    if (!damage_shown_) scene_.set_object_visible(damage_, true), damage_shown_ = true;
}

void Hud::scratch(bool right) {
    if (scratch_ == size_t(-1)) return;
    const std::string clip = std::string("spider_scratch_") + (right ? "right_0" : "left_0") + char('1' + std::rand() % 3);
    scene_.play_clip(scratch_, clip, false);
    if (!scratch_shown_) scene_.set_object_visible(scratch_, true), scratch_shown_ = true;
}

void Hud::show_blood(float fraction) {
    if (blood_ == size_t(-1)) return;
    // A quarter of the way into the first half of the clip, or its start under a quarter.
    Scene::ClipState cs;
    if (!scene_.play_clip(blood_, "hud_blood", false) || !scene_.clip_state(blood_, cs)) return;
    scene_.set_clip_time(blood_, fraction < 0.25f ? 0.0f : std::max(cs.length - 1, 0.0f) * 0.5f * 0.25f / 1000.0f);
    if (!blood_shown_) scene_.set_object_visible(blood_, true), blood_shown_ = true;
    blood_fading_ = false;
}

void Hud::fade_blood() {
    if (!blood_shown_ || blood_fading_) return;
    scene_.set_clip_speed(blood_, 1);
    blood_fading_ = true;
}

void Hud::update_effects(float) {
    // Each goes once its clip has played through ("Finished": the effect stops).
    if (damage_shown_ && scene_.clip_finished(damage_)) scene_.set_object_visible(damage_, false), damage_shown_ = false;
    if (scratch_shown_ && scene_.clip_finished(scratch_)) scene_.set_object_visible(scratch_, false), scratch_shown_ = false;
    if (blood_fading_ && scene_.clip_finished(blood_))
        scene_.set_object_visible(blood_, false), blood_shown_ = blood_fading_ = false;
}

void Hud::update_frost(float dt) {
    if (frost_object_ == size_t(-1)) return;
    const float target = frost_target_ * frost_max;
    frost_ = frost_ < target ? std::min(frost_ + frost_rate_ * dt, target) : std::max(frost_ - frost_rate_ * dt, target);
    const bool shown = frost_ > 0;
    if (shown != frost_shown_) scene_.set_object_visible(frost_object_, shown), frost_shown_ = shown;
    Scene::ClipState cs;
    if (shown && scene_.clip_state(frost_object_, cs)) scene_.set_clip_time(frost_object_, frost_ * cs.length / 1000.0f);
}

const Sprite* Hud::sprite(const std::string& name) {
    const std::string key = large_ ? name + "@2x" : name;
    if (auto it = sprites_.find(key); it != sprites_.end()) return it->second.frames.empty() ? nullptr : &it->second;
    Sprite& s = sprites_[key];
    // The game picks a resolution variant (<name>_<w>x<h>.bsprite) by screen size; the
    // 1720x1280 and 2560x1504 ones are the 960x640 frames at twice the size, the 1024x768
    // ones (the weapons' crosshairs) laid out for 768 high.
    std::string base = name;
    if (base.size() > 8 && base.compare(base.size() - 8, 8, ".bsprite") == 0) base.resize(base.size() - 8);
    std::vector<std::pair<std::string, float>> files;
    if (large_) files = {{base + "_1720x1280.bsprite", 2.0f}, {base + "_2560x1504.bsprite", 2.0f}};
    files.insert(files.end(), {{base + "_1024x768.bsprite", 768.0f / 640.0f}, {base + "_960x640.bsprite", 1.0f}, {base + ".bsprite", 1.0f}});
    sprite_factor_[key] = 1;
    for (const auto& [file, factor] : files) {
        if (!scene_.vfs().exists(file)) continue;
        try {
            s = load_sprite(scene_.vfs().read(file));
            sprite_factor_[key] = factor;
        } catch (const FormatError&) {
        }
        break;
    }
    return s.frames.empty() ? nullptr : &s;
}

const Sprite* Hud::sprite_file(const std::string& file) {
    const std::string key = "file:" + file;
    if (auto it = sprites_.find(key); it != sprites_.end()) return it->second.frames.empty() ? nullptr : &it->second;
    Sprite& s = sprites_[key];
    if (scene_.vfs().exists(file)) {
        try {
            s = load_sprite(scene_.vfs().read(file));
        } catch (const FormatError&) {
        }
    }
    return s.frames.empty() ? nullptr : &s;
}

void Hud::draw_frame_rotated(ParticleList& out, const Sprite& s, int frame, glm::vec2 at, float scale, float radians,
                             const glm::vec4& color) {
    if (frame < 0 || size_t(frame) >= s.frames.size()) return;
    const Sprite::Frame& f = s.frames[size_t(frame)];
    const uint32_t rgba = pack_rgba(color);
    const float c = std::cos(radians), sn = std::sin(radians);
    auto place = [&](float x, float y) { return glm::vec3(at.x + (x * c - y * sn) * scale, at.y + (x * sn + y * c) * scale, 0); };
    for (int k = f.first; k < f.first + f.count && size_t(k) < s.fmodules.size(); ++k) {
        const Sprite::FModule& fm = s.fmodules[size_t(k)];
        if ((fm.flags & 16) || size_t(fm.module) >= s.modules.size()) continue;
        const Sprite::Module& m = s.modules[size_t(fm.module)];
        if (size_t(m.image) >= s.images.size()) continue;
        const GpuTexture* tex = scene_.texture(s.images[size_t(m.image)]);
        if (!tex || tex->width <= 0) continue;
        float u0 = float(m.x) / float(tex->width), v0 = float(m.y) / float(tex->height);
        float u1 = float(m.x + m.w) / float(tex->width), v1 = float(m.y + m.h) / float(tex->height);
        if (fm.flags & 1) std::swap(u0, u1);
        if (fm.flags & 2) std::swap(v0, v1);
        const float x0 = float(fm.x), y0 = float(fm.y), x1 = x0 + float(m.w), y1 = y0 + float(m.h);
        const ParticleVertex v[4] = {{place(x0, y0), {u0, v0}, rgba}, {place(x1, y0), {u1, v0}, rgba},
                                     {place(x1, y1), {u1, v1}, rgba}, {place(x0, y1), {u0, v1}, rgba}};
        const uint32_t first = uint32_t(out.vertices.size());
        out.vertices.insert(out.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        out.batches.push_back({tex, false, first, 6});
    }
}

float Hud::sprite_scale(const std::string& name) {
    sprite(name);
    const std::string key = large_ ? name + "@2x" : name;
    auto it = sprite_factor_.find(key);
    return screen_min_ / (640.0f * (it == sprite_factor_.end() ? 1.0f : it->second));
}

void Hud::draw_frame(ParticleList& out, const Sprite& s, int frame, glm::vec2 at, glm::vec2 scale, const glm::vec4& color,
                     const glm::vec4& clip) {
    if (frame < 0 || size_t(frame) >= s.frames.size()) return;
    const Sprite::Frame& f = s.frames[size_t(frame)];
    const uint32_t rgba = pack_rgba(color);
    for (int k = f.first; k < f.first + f.count && size_t(k) < s.fmodules.size(); ++k) {
        const Sprite::FModule& fm = s.fmodules[size_t(k)];
        const glm::vec2 pos = at + glm::vec2(float(fm.x), float(fm.y)) * scale;
        if (fm.flags & 16) {   // a "hyper" piece: another frame, placed (and flipped) here
            if (fm.module != frame && std::find(skip_frames.begin(), skip_frames.end(), fm.module) == skip_frames.end())
                draw_frame(out, s, fm.module, pos, scale * glm::vec2(fm.flags & 1 ? -1 : 1, fm.flags & 2 ? -1 : 1), color, clip);
            continue;
        }
        if (size_t(fm.module) >= s.modules.size()) continue;
        const Sprite::Module& m = s.modules[size_t(fm.module)];
        if (size_t(m.image) >= s.images.size()) continue;
        const GpuTexture* tex = scene_.texture(s.images[size_t(m.image)]);
        if (!tex || tex->width <= 0) continue;
        float u0 = float(m.x) / float(tex->width), v0 = float(m.y) / float(tex->height);
        float u1 = float(m.x + m.w) / float(tex->width), v1 = float(m.y + m.h) / float(tex->height);
        if (fm.flags & 1) std::swap(u0, u1);
        if (fm.flags & 2) std::swap(v0, v1);
        float x0 = pos.x, y0 = pos.y, x1 = x0 + float(m.w) * scale.x, y1 = y0 + float(m.h) * scale.y;
        // Left to right and top to bottom whatever the mirroring, then cut to the clip.
        if (x1 < x0) std::swap(x0, x1), std::swap(u0, u1);
        if (y1 < y0) std::swap(y0, y1), std::swap(v0, v1);
        auto cut = [](float& a0, float& a1, float& t0, float& t1, float lo, float hi) {
            if (a1 <= lo || a0 >= hi || a1 <= a0) return false;
            const float s0 = std::max(a0, lo), s1 = std::min(a1, hi);
            const float d = t1 - t0, n0 = t0 + d * (s0 - a0) / (a1 - a0), n1 = t0 + d * (s1 - a0) / (a1 - a0);
            a0 = s0, a1 = s1, t0 = n0, t1 = n1;
            return true;
        };
        if (!cut(x0, x1, u0, u1, clip.x, clip.z) || !cut(y0, y1, v0, v1, clip.y, clip.w)) continue;
        const ParticleVertex v[4] = {{{x0, y0, 0}, {u0, v0}, rgba}, {{x1, y0, 0}, {u1, v0}, rgba},
                                     {{x1, y1, 0}, {u1, v1}, rgba}, {{x0, y1, 0}, {u0, v1}, rgba}};
        const uint32_t first = uint32_t(out.vertices.size());
        out.vertices.insert(out.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        out.batches.push_back({tex, false, first, 6});
    }
}

void Hud::draw_anim(ParticleList& out, const Sprite& s, int anim, glm::vec2 at, float scale, float seconds, const glm::vec4& color) {
    if (anim < 0 || size_t(anim) >= s.anims.size()) return;
    const Sprite::Anim& a = s.anims[size_t(anim)];
    int total = 0;
    for (int k = a.first; k < a.first + a.count && size_t(k) < s.aframes.size(); ++k) total += std::max(s.aframes[size_t(k)].time, 1);
    if (total <= 0) return;
    int tick = int(seconds * 30.0f) % total;
    for (int k = a.first; k < a.first + a.count && size_t(k) < s.aframes.size(); ++k) {
        const Sprite::AFrame& f = s.aframes[size_t(k)];
        if ((tick -= std::max(f.time, 1)) < 0) {
            draw_frame(out, s, f.frame, at + glm::vec2(float(f.x), float(f.y)) * scale, scale, color);
            return;
        }
    }
}

glm::vec4 Hud::frame_bounds(const Sprite& s, int frame) {
    glm::vec4 b(1e9f, 1e9f, -1e9f, -1e9f);
    if (frame < 0 || size_t(frame) >= s.frames.size()) return glm::vec4(0);
    const Sprite::Frame& f = s.frames[size_t(frame)];
    for (int k = f.first; k < f.first + f.count && size_t(k) < s.fmodules.size(); ++k) {
        const Sprite::FModule& fm = s.fmodules[size_t(k)];
        glm::vec4 m(0);
        if (fm.flags & 16) {   // another frame's box, flipped as placed
            if (fm.module == frame) continue;
            m = frame_bounds(s, fm.module);
            if (m == glm::vec4(0)) continue;
            if (fm.flags & 1) m = glm::vec4(-m.z, m.y, -m.x, m.w);
            if (fm.flags & 2) m = glm::vec4(m.x, -m.w, m.z, -m.y);
        } else {
            if (size_t(fm.module) >= s.modules.size()) continue;
            m = glm::vec4(0, 0, float(s.modules[size_t(fm.module)].w), float(s.modules[size_t(fm.module)].h));
        }
        m += glm::vec4(float(fm.x), float(fm.y), float(fm.x), float(fm.y));
        b = glm::vec4(std::min(b.x, m.x), std::min(b.y, m.y), std::max(b.z, m.z), std::max(b.w, m.w));
    }
    return b.x > b.z ? glm::vec4(0) : b;
}

void Hud::draw_prompt(ParticleList& out, TextRenderer& text, const std::string& s, glm::vec2 at, const glm::vec4& color) {
    // Pieces: text, or a glyph (frame number).
    struct Piece {
        std::string text;
        int glyph = -1;
        glm::vec2 size{0};
    };
    std::vector<Piece> pieces;
    for (size_t p = 0; p < s.size();) {
        const size_t a = s.find('^', p), b = a == std::string::npos ? a : s.find('^', a + 1);
        if (b == std::string::npos) {
            pieces.push_back({s.substr(p)});
            break;
        }
        if (a > p) pieces.push_back({s.substr(p, a - p)});
        pieces.push_back({"", std::atoi(s.substr(a + 1, b - a - 1).c_str())});
        p = b + 1;
    }
    const Sprite* glyphs = sprite("tutorial.bsprite");
    float line = text.measure("Ag", 100000).y, width = 0;
    for (Piece& pc : pieces) {
        if (pc.glyph >= 0 && glyphs) {
            const glm::vec4 fb = frame_bounds(*glyphs, pc.glyph);
            const float scale = fb.w > fb.y ? line * 1.6f / (fb.w - fb.y) : 0;
            pc.size = glm::vec2((fb.z - fb.x) * scale, line);
        } else if (pc.glyph < 0) {
            pc.size = text.measure(pc.text, 100000);
        }
        width += pc.size.x;
    }
    float x = at.x - width * 0.5f;
    for (const Piece& pc : pieces) {
        if (pc.glyph >= 0 && glyphs) {
            const glm::vec4 fb = frame_bounds(*glyphs, pc.glyph);
            const float scale = fb.w > fb.y ? line * 1.6f / (fb.w - fb.y) : 0;
            // Anchor so the frame's box sits at x, centred on the line.
            draw_frame(out, *glyphs, pc.glyph, {x - fb.x * scale, at.y + line * 0.5f - (fb.y + fb.w) * 0.5f * scale}, scale, color);
        } else if (pc.glyph < 0) {
            text.draw(out, pc.text, {x, at.y}, 100000, color, {0.0f, 0.0f});
        }
        x += pc.size.x;
    }
}

void Hud::draw_objectives(ParticleList& out, TextRenderer& text, const std::vector<ObjectiveLine>& lines, float alpha, int w, int h) {
    set_screen(w, h);
    const Sprite* icons = sprite("controls.bsprite");   // 16 unchecked, 17 checked, 18 primary marker
    if (lines.empty() || alpha <= 0) return;
    const float sx = float(std::max(w, h)) / 480.0f, sy = float(std::min(w, h)) / 320.0f;
    const float scale = sprite_scale("controls.bsprite");
    const float x0 = 40 * sx;
    float y = 100 * sy;
    const float line = text.measure("Ag", 100000).y;
    for (const ObjectiveLine& l : lines) {
        const int frame = l.primary ? 18 : l.done ? 17 : 16;
        float x = x0;
        if (icons) {
            const glm::vec4 b = frame_bounds(*icons, frame) * scale;
            // Primary: the marker left of the text; secondaries: the box starts the line.
            const float left = l.primary ? x0 - (b.z - b.x) - 4 * sx : x0;
            draw_frame(out, *icons, frame, {left - b.x, y + line * 0.5f - (b.y + b.w) * 0.5f}, scale, glm::vec4(1, 1, 1, alpha));
            if (!l.primary) x = x0 + (b.z - b.x) + 4 * sx;
        }
        const int wrap = int(float(w) - 5 * sx - x);
        const glm::vec4 color = l.done && !l.primary ? glm::vec4(0.6f, 0.9f, 0.6f, alpha) : glm::vec4(1, 1, 1, alpha);
        const float off = std::max(1.0f, float(h) / 480.0f);
        text.draw(out, l.text, {x + off, y + off}, wrap, glm::vec4(0, 0, 0, alpha * 0.7f), {0.0f, 0.0f});   // shadow
        text.draw(out, l.text, {x, y}, wrap, color, {0.0f, 0.0f});
        y += text.measure(l.text, wrap).y + 6 * sy;
    }
}

void Hud::draw_grenade_warning(ParticleList& out, const glm::mat4& view, const glm::vec3& at, int w, int h) {
    set_screen(w, h);
    const Sprite* s = sprite("hud3d.bsprite");
    if (!s) return;
    const glm::vec4 v = view * glm::vec4(at, 1);
    const float a = std::atan2(v.x, -v.z);   // 0 ahead, positive to the right
    const float r = 110.0f * float(h) / 640.0f, scale = sprite_scale("hud3d.bsprite");
    const glm::vec4 b = frame_bounds(*s, 15) * scale;
    const glm::vec2 c(float(w) * 0.5f + std::sin(a) * r, float(h) * 0.5f - std::cos(a) * r);
    draw_frame(out, *s, 15, c - glm::vec2(b.x + b.z, b.y + b.w) * 0.5f, scale, glm::vec4(1));
}

void Hud::draw_timer(ParticleList& out, TextRenderer& text, float seconds, int w, int h) {
    set_screen(w, h);
    // In the 960x640 sheet's pixels (the game draws at the device's resolution); the glow
    // sits behind the digits.
    const float scale = sprite_scale("chatboard.bsprite");
    const float y = 72 * scale;
    if (const Sprite* cb = sprite("chatboard.bsprite")) {
        const glm::vec4 b = frame_bounds(*cb, 230) * scale;
        draw_frame(out, *cb, 230, {(float(w) - (b.z - b.x)) * 0.5f - b.x, y - (b.y + b.w) * 0.5f}, scale, glm::vec4(1));
    }
    const int s = std::max(int(seconds), 0);
    char line[16];
    std::snprintf(line, sizeof line, "%02d:%02d", s / 60, s % 60);
    text.draw(out, line, {float(w) * 0.5f, y}, w, glm::vec4(1), {0.5f, 0.5f});
}

void Hud::draw_pickup(ParticleList& out, TextRenderer& text, const std::string& name, int amount, int w, int h) {
    set_screen(w, h);
    const float sx = float(w) / 480, sy = float(h) / 320;
    const glm::vec2 at(float(w) - 40 * sx, 120 * sy);
    if (const Sprite* s = sprite(name)) draw_frame(out, *s, 0, at, sprite_scale(name), glm::vec4(1));
    if (amount > 0) text.draw(out, std::to_string(amount), at + glm::vec2(0, 20 * sy), w, glm::vec4(1), {0.5f, 1.0f});
}

void Hud::draw_marker(ParticleList& out, TextRenderer& text, const glm::mat4& view_proj, const glm::vec3& at, float distance, int w,
                      int h, bool metric) {
    set_screen(w, h);
    const Sprite* s = sprite("Chatboard.bsprite");
    if (!s) return;
    // Frames: 226 on screen, 222 left edge, 223 right, 224 top, 225 bottom.
    const float scale = sprite_scale("Chatboard.bsprite");
    glm::vec4 c = view_proj * glm::vec4(at, 1);
    const bool behind = c.w <= 1e-3f;
    if (behind) c = -c;   // mirrored: off screen the arrow still points the right way
    glm::vec2 p((c.x / std::max(std::abs(c.w), 1e-3f) * 0.5f + 0.5f) * float(w), (0.5f - c.y / std::max(std::abs(c.w), 1e-3f) * 0.5f) * float(h));
    if (behind) p = glm::vec2(float(w) * 0.5f, float(h) * 0.5f) + glm::normalize(p - glm::vec2(float(w), float(h)) * 0.5f + 1e-3f) * float(w);
    const float margin_x = 24 * scale, margin_y = 24 * scale + text.measure("0", 1000).y;
    int frame = 226;
    if (p.x < margin_x) frame = 222;
    else if (p.x > float(w) - margin_x) frame = 223;
    else if (p.y < margin_y) frame = 224;
    else if (p.y > float(h) - margin_y) frame = 225;
    p = glm::clamp(p, glm::vec2(margin_x, margin_y), glm::vec2(float(w) - margin_x, float(h) - margin_y));
    const glm::vec4 white(1);
    draw_frame(out, *s, frame, p - glm::vec2(0, frame == 226 ? 9 * scale : 0), scale, white);
    char label[32];
    // The game shows half the world distance, in metres or (Metric off) feet.
    if (metric) std::snprintf(label, sizeof label, "%dm", int(distance * 0.5f));
    else std::snprintf(label, sizeof label, "%dft", int(distance * 0.5f * 3.2808f));
    // The distance above the marker (below it when the marker sits on the top edge).
    const glm::vec2 at_text = frame == 224 ? p + glm::vec2(0, 26 * scale) : p - glm::vec2(0, 26 * scale + text.measure(label, 1000).y);
    text.draw(out, label, at_text + glm::vec2(1), w, glm::vec4(0, 0, 0, 0.7f));
    text.draw(out, label, at_text, w, white);
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

void Hud::frame(FrameParams& f, int w, int h, const State& s, ParticleList& sprites) {
    if (object_ == size_t(-1)) return;
    const bool jeep = s.jeep_shield > -1;
    if (s.visible != visible_ || jeep != jeep_visible_ || s.mech != mech_) {
        scene_.set_object_visible(object_, s.visible), scene_.set_object_visible(ammo_, s.visible && s.weapons);
        if (shield_ != size_t(-1)) scene_.set_object_visible(shield_, s.visible && !jeep && !s.mech);
        if (jeep_shield_ != size_t(-1)) scene_.set_object_visible(jeep_shield_, s.visible && jeep);
        visible_ = s.visible, jeep_visible_ = jeep, mech_ = s.mech;
    }
    if (Scene::ClipState cs; jeep && jeep_shield_ != size_t(-1) && std::abs(s.jeep_shield - jeep_shown_) > 1e-3f && scene_.clip_state(jeep_shield_, cs)) {
        jeep_shown_ = s.jeep_shield;
        scene_.set_clip_time(jeep_shield_, std::clamp(s.jeep_shield, 0.0f, 1.0f) * cs.length * 0.999f / 1000.0f);
    }
    if (s.grenades != grenades_) scene_.set_node_visible(object_, "grenade_button", s.grenades), grenades_ = s.grenades;
    if (s.weapons != weapons_) {
        scene_.set_node_visible(object_, "weapons", s.weapons), weapons_ = s.weapons;
        scene_.set_object_visible(ammo_, s.visible && s.weapons);
    }
    if (shield_ != size_t(-1) && s.core != core_shown_) {   // HUDShield's states: Normal, or a core's bar
        static const char* const kClips[] = {"shield_upgrade_vanguard", "shield_upgrade_crypton", "shield_upgrade_plasma"};
        scene_.play_clip(shield_, s.core >= 0 && s.core < 3 ? kClips[s.core] : "shield", false);
        core_shown_ = s.core, shield_shown_ = -1;
    }
    const float fill = s.core >= 0 ? s.core_left : s.shield;
    if (Scene::ClipState cs; shield_ != size_t(-1) && std::abs(fill - shield_shown_) > 1e-3f && scene_.clip_state(shield_, cs)) {
        shield_shown_ = fill;
        // The cores' clips fill with time (CHud::SetShieldPercent), the shield's empties.
        const float t = s.core >= 0 ? std::clamp(fill, 0.0f, 1.0f) : 1 - std::clamp(fill, 0.0f, 1.0f);
        scene_.set_clip_time(shield_, t * cs.length * 0.999f / 1000.0f);
    }
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
        // The full-screen effects (frost, blood, the shield's ripple) are 4:3 planes before the
        // camera: wider screens get them stretched across.
        const float widen = std::max(1.0f, float(w) / float(std::max(h, 1)) / (4.0f / 3.0f));
        for (size_t o : {frost_object_, blood_, damage_, scratch_})
            if (o != size_t(-1)) scene_.set_object_transform(o, glm::scale(glm::mat4(1), glm::vec3(widen, 1, 1)));
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
    // The grenade button: the grenade (hud3d.bsprite frame 15, placed after the reference
    // footage) on the panel; the count in CHud's quad buffer under grenade_button at
    // (5, 0, -15), scale 0.5 (CHud::Init).
    if (s.grenades) {
        Scene::Quad panel;
        if (scene_.node_quad(object_, "grenade_idle", panel) && panel.texture) {
            glm::vec2 lo(1e9f), hi(-1e9f);
            for (const glm::vec3& c : panel.position) lo = glm::min(lo, screen(c)), hi = glm::max(hi, screen(c));
            const glm::vec2 centre(glm::mix(lo.x, hi.x, 0.46f), glm::mix(lo.y, hi.y, 0.5f));
            const float half = (hi.y - lo.y) * 0.19f;
            const glm::vec2 t0 = glm::vec2(870, 7) / 1024.0f, t1 = glm::vec2(948, 85) / 1024.0f;   // the icon and its glow
            const uint32_t tint = pack_rgba(glm::vec4(1, 1, 1, 0.75f));
            const ParticleVertex v[4] = {{{centre + glm::vec2(-half, -half), 0}, t0, tint},
                                         {{centre + glm::vec2(half, -half), 0}, {t1.x, t0.y}, tint},
                                         {{centre + glm::vec2(half, half), 0}, t1, tint},
                                         {{centre + glm::vec2(-half, half), 0}, {t0.x, t1.y}, tint}};
            const uint32_t first = uint32_t(sprites.vertices.size());
            sprites.vertices.insert(sprites.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
            sprites.batches.push_back({panel.texture, false, first, 6});
            numeric_text(sprites, s.grenade_count < 0 ? "--" : std::to_string(s.grenade_count), "grenade_button", {5, 0, -15}, 0.5f, vp,
                         w, h);
        }
    }
    // The ammo gauge follows the panel; its bar fills to the clip's share, left to right.
    // (CHud::Init hangs the HUDAmmoIndicator object on the "weapons" node, at its origin.)
    scene_.set_object_transform(ammo_, scene_.object_matrix(object_, "weapons"));
    Scene::Quad bar;
    if (s.weapons && scene_.node_quad(ammo_, "ammo_bar", bar) && bar.texture) {
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
    }
    // The ammo counter (CLevelInterfaceNormal::Draw): clip, and "/reserve" unless the reserve
    // is endless (the handgun's), in the quad buffer at the "ammo" node, scale 0.6.
    auto endless = [](int n) { return n < 0 || n >= 100000; };
    if (s.weapons && !endless(s.clip)) {
        const std::string text = endless(s.reserve) ? std::to_string(s.clip) : std::to_string(s.clip) + "/" + std::to_string(s.reserve);
        numeric_text(sprites, text, "ammo", {0, 0, 0}, 0.6f, vp, w, h);
    }
}

void Hud::numeric_text(ParticleList& out, const std::string& text, const std::string& node, glm::vec3 offset, float scale,
                       const glm::mat4& view_proj, int w, int h) {
    const Sprite* sp = sprite("hud3d.bsprite");
    if (!sp || text.empty()) return;
    auto glyph = [](char c) { return c == '/' ? 14 : c >= '0' && c <= '9' ? c - '0' : -1; };
    // GetNumericTextSize: each glyph advances its width less 8 pixels.
    float width = 0, height = 0;
    for (char c : text) {
        const glm::vec4 b = frame_bounds(*sp, glyph(c));
        width += b.z - b.x - 8, height = std::max(height, b.w - b.y);
    }
    const glm::mat4 m = view_proj * scene_.object_matrix(object_, node) * glm::translate(glm::mat4(1), offset) *
                        glm::scale(glm::mat4(1), glm::vec3(scale));
    auto screen = [&](float x, float y) {
        const glm::vec4 c = m * glm::vec4(x, 0, -y, 1);
        const glm::vec2 ndc = glm::vec2(c) / std::max(c.w, 1e-4f);
        return glm::vec3((ndc.x * 0.5f + 0.5f) * float(w), (0.5f - ndc.y * 0.5f) * float(h), 0);
    };
    float x = -width;
    const float y = -height * 0.5f;
    const uint32_t white = pack_rgba(glm::vec4(1));
    for (char c : text) {
        const int f = glyph(c);
        if (f < 0 || size_t(f) >= sp->frames.size()) continue;
        const Sprite::Frame& fr = sp->frames[size_t(f)];
        for (int k = fr.first; k < fr.first + fr.count; ++k) {
            const Sprite::FModule& fm = sp->fmodules[size_t(k)];
            const Sprite::Module& md = sp->modules[size_t(fm.module)];
            const GpuTexture* tex = scene_.texture(sp->images[size_t(md.image)]);
            if (!tex || tex->width <= 0) continue;
            const glm::vec2 t0(float(md.x) / float(tex->width), float(md.y) / float(tex->height));
            const glm::vec2 t1(float(md.x + md.w) / float(tex->width), float(md.y + md.h) / float(tex->height));
            const float x0 = x + float(fm.x), y0 = y + float(fm.y), x1 = x0 + float(md.w), y1 = y0 + float(md.h);
            const ParticleVertex v[4] = {{screen(x0, y0), t0, white}, {screen(x1, y0), {t1.x, t0.y}, white},
                                         {screen(x1, y1), t1, white}, {screen(x0, y1), {t0.x, t1.y}, white}};
            const uint32_t first = uint32_t(out.vertices.size());
            out.vertices.insert(out.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
            out.batches.push_back({tex, false, first, 6});
        }
        const glm::vec4 b = frame_bounds(*sp, f);
        x += b.z - b.x - 8;
    }
}

}  // namespace nova
