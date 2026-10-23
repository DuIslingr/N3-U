#include "game/death_screen.h"

#include <algorithm>
#include <utility>

#include "render/text.h"

namespace nova {

namespace {

constexpr int kButton = 26, kButtonPressed = 27;   // Chatboard.bsprite

void rect(Overlay& o, glm::vec4 r, const glm::vec4& color) {
    const uint32_t c = pack_rgba(color);
    const ColorVertex v[4] = {{{r.x, r.y, 0}, c}, {{r.z, r.y, 0}, c}, {{r.z, r.w, 0}, c}, {{r.x, r.w, 0}, c}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

}  // namespace

void DeathScreen::show() {
    open = true;
    time_ = 0, music_ = false;
    selected_ = 0, pressed_ = -1;
}

void DeathScreen::update(float dt) {
    if (open) time_ += dt;
}

bool DeathScreen::music_due() {
    if (!open || !ready() || music_) return false;
    return music_ = true;
}

DeathScreen::Action DeathScreen::key(Key k) {
    if (!ready()) return Action::None;
    switch (k) {
        case Key::Up: selected_ = 0; return Action::None;
        case Key::Down: selected_ = 1; return Action::None;
        case Key::Activate: return selected_ == 0 ? Action::Retry : Action::Menu;
    }
    return Action::None;
}

DeathScreen::Action DeathScreen::pointer(glm::vec2 at, Pointer phase) {
    if (!ready()) return Action::None;
    int over = -1;
    for (int i = 0; i < 2; ++i)
        if (at.x >= rects_[i].x && at.x <= rects_[i].z && at.y >= rects_[i].y && at.y <= rects_[i].w) over = i;
    if (over >= 0) selected_ = over;
    if (phase == Pointer::Press) pressed_ = over;
    if (phase != Pointer::Release) return Action::None;
    const int p = std::exchange(pressed_, -1);
    if (p < 0 || p != over) return Action::None;
    return p == 0 ? Action::Retry : Action::Menu;
}

void DeathScreen::draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& font, int w, int h) {
    const float W = float(w), H = float(h);
    // From clear to SColor(200, 20, 20, 20) over the first second.
    const float t = std::clamp(time_, 0.0f, 1.0f);
    rect(overlay, {0, 0, W, H}, {20 / 255.0f, 20 / 255.0f, 20 / 255.0f, t * 200 / 255.0f});
    hud.set_screen(w, h);
    const Sprite* s = hud.sprite("Chatboard.bsprite");
    if (!s) return;
    // Centred on the 480x320 screen's middle (240, 160), 5 px above and below it.
    const float k = hud.sprite_scale("Chatboard.bsprite");
    const float sy = H / 320.0f;
    const glm::vec4 b = Hud::frame_bounds(*s, kButton);
    const glm::vec2 size = glm::vec2(b.z - b.x, b.w - b.y) * k;
    const std::string labels[2] = {text("FPS_RETRY"), text("FPS_MAIN_MENU")};
    const glm::vec4 color(1, 1, 1, ready() ? 1.0f : 0.5f);
    for (int i = 0; i < 2; ++i) {
        const glm::vec2 c(W * 0.5f, H * 0.5f + (i == 0 ? -1.0f : 1.0f) * (5 * sy + size.y * 0.5f));
        const glm::vec2 at = c - glm::vec2(b.x + b.z, b.y + b.w) * 0.5f * k;
        hud.draw_frame(out, *s, ready() && (selected_ == i || pressed_ == i) ? kButtonPressed : kButton, at, k, color);
        font.draw(out, labels[i], {c.x, c.y - 1.5f * sy}, w, color, {0.5f, 0.5f});
        rects_[i] = glm::vec4(c - size * 0.5f, c + size * 0.5f);
    }
}

}  // namespace nova
