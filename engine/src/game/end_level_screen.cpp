#include "game/end_level_screen.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include "render/text.h"

namespace nova {

namespace {

constexpr int kButton = 26, kButtonPressed = 27, kSeparator = 210;   // Chatboard.bsprite
constexpr float kRowTime = 0.3f;                                     // a row every 300 ms

void rect(Overlay& o, glm::vec4 r, const glm::vec4& color) {
    const uint32_t c = pack_rgba(color);
    const ColorVertex v[4] = {{{r.x, r.y, 0}, c}, {{r.z, r.y, 0}, c}, {{r.z, r.w, 0}, c}, {{r.x, r.w, 0}, c}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

struct Row {
    std::string label, value;
    bool header = false, separator = false;
};

}  // namespace

void EndLevelScreen::show(const Stats& stats, bool game_finished) {
    open = true;
    stats_ = stats, finished_ = game_finished;
    time_ = 0, selected_ = 1, pressed_ = -1;
}

void EndLevelScreen::update(float dt) {
    if (open) time_ += dt;
}

bool EndLevelScreen::ready() const { return time_ > 15 * kRowTime; }

EndLevelScreen::Action EndLevelScreen::key(Key k) {
    if (!ready()) {   // a press shows the rest at once
        if (k == Key::Activate) time_ = 15 * kRowTime + 0.01f;
        return Action::None;
    }
    switch (k) {
        case Key::Left: selected_ = 0; return Action::None;
        case Key::Right: selected_ = 1; return Action::None;
        case Key::Activate: return selected_ == 0 ? Action::Menu : Action::Continue;
    }
    return Action::None;
}

EndLevelScreen::Action EndLevelScreen::pointer(glm::vec2 at, Pointer phase) {
    if (!ready()) {
        if (phase == Pointer::Release) time_ = 15 * kRowTime + 0.01f;
        return Action::None;
    }
    int over = -1;
    for (int i = 0; i < 2; ++i)
        if (at.x >= rects_[i].x && at.x <= rects_[i].z && at.y >= rects_[i].y && at.y <= rects_[i].w) over = i;
    if (over >= 0) selected_ = over;
    if (phase == Pointer::Press) pressed_ = over;
    if (phase != Pointer::Release) return Action::None;
    const int p = std::exchange(pressed_, -1);
    if (p < 0 || p != over) return Action::None;
    return p == 0 ? Action::Menu : Action::Continue;
}

void EndLevelScreen::draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& title, TextRenderer& rows, int w, int h) {
    const float W = float(w), H = float(h);
    // The 480x320 screen scaled to the height, centred.
    const float s = H / 320.0f;
    const glm::vec2 origin((W - 480 * s) * 0.5f, 0);
    auto px = [&](float x, float y) { return origin + glm::vec2(x, y) * s; };
    rect(overlay, {0, 0, W, H}, {0, 0, 0, std::min(time_ / 0.5f, 1.0f) * 0.8f});
    title.draw(out, text("FPS_CONGRATULATIONS"), px(240, 10), w, glm::vec4(1), {0.5f, 0.0f});
    float y = 42;
    if (finished_) {
        rows.draw(out, text("FPS_GAME_FINISHED_02"), px(240, y - 5), w, glm::vec4(1), {0.5f, 0.0f});
        y += 20;
    }
    char buf[64];
    auto number = [](int n) { return std::to_string(n); };
    std::snprintf(buf, sizeof buf, "%d:%02d", int(stats_.play_time) / 60, int(stats_.play_time) % 60);
    const std::string play_time = buf;
    const std::vector<Row> list = {
        {text("FPS_STATISTICS"), "", true},
        {text("FPS_HEADSHOTS"), number(stats_.headshots)},
        {text("FPS_BLAST_KILLS"), number(stats_.blast_kills)},
        {text("FPS_PARALYZE_KILLS"), number(stats_.paralyze_kills)},
        {text("FPS_SLOW_TIME_KILLS"), number(stats_.slow_kills)},
        {text("FPS_TOTAL_ENEMIES"), number(stats_.kills)},
        {"", "", false, true},
        {text("FPS_BONUS_CREDITS"), "", true},
        {text("FPS_SHIELD_BONUS"), number(int(stats_.shield * 100 + 0.5f)) + "%"},
        {text("FPS_ACCURACY"), number(int(stats_.accuracy * 100 + 0.5f)) + "%"},
        {text("FPS_PLAY_TIME"), play_time},
        {"", "", false, true},
        {text("FPS_PENALTIES"), "", true},
        {text("FPS_RETRIES"), number(stats_.retries)},
    };
    hud.set_screen(w, h);
    const Sprite* cb = hud.sprite("Chatboard.bsprite");
    const float k = hud.sprite_scale("Chatboard.bsprite");
    for (size_t i = 0; i < list.size(); ++i) {
        // Each row: 300 ms after the one before, sliding in from the left as it fades in.
        const float t = std::clamp((time_ - float(i) * kRowTime) / kRowTime, 0.0f, 1.0f);
        if (t <= 0) break;
        const float slide = (1 - t) * -60;
        const glm::vec4 color(1, 1, 1, t);
        const Row& r = list[i];
        if (r.separator) {
            if (cb) {   // centred under the rows
                const glm::vec4 b = Hud::frame_bounds(*cb, kSeparator);
                hud.draw_frame(out, *cb, kSeparator, px(240 + slide, y + 1) - glm::vec2((b.x + b.z) * 0.5f * k, 0), k, color);
            }
            y += 10;
            continue;
        }
        rows.draw(out, r.label, px(128 + slide, y), w, r.header ? glm::vec4(1, 0.72f, 0.3f, t) : color, {0.0f, 0.0f});
        if (!r.value.empty()) rows.draw(out, r.value, px(352 + slide, y), w, color, {1.0f, 0.0f});
        y += 15;
    }
    if (!cb) return;
    // Back to Menu and Continue, once the rows are in.
    const std::string labels[2] = {text("FPS_MAIN_MENU"), text("FPS_COTINUE")};
    const glm::vec2 corners[2] = {px(59, 280), px(252, 280)};
    const glm::vec4 b = Hud::frame_bounds(*cb, kButton);
    const glm::vec4 color(1, 1, 1, ready() ? 1.0f : 0.4f);
    for (int i = 0; i < 2; ++i) {
        const glm::vec2 at = corners[i] - glm::vec2(b.x, b.y) * k;
        hud.draw_frame(out, *cb, ready() && (selected_ == i || pressed_ == i) ? kButtonPressed : kButton, at, k, color);
        rects_[i] = glm::vec4(corners[i], corners[i] + glm::vec2(b.z - b.x, b.w - b.y) * k);
        rows.draw(out, labels[i], (glm::vec2(rects_[i].x, rects_[i].y) + glm::vec2(rects_[i].z, rects_[i].w)) * 0.5f, w, color, {0.5f, 0.5f});
    }
}

}  // namespace nova
