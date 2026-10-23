#include "game/end_level_screen.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include "render/text.h"

namespace nova {

namespace {

constexpr int kButton = 26, kButtonPressed = 27, kSeparator = 210, kCredits = 192;   // Chatboard.bsprite
constexpr float kRowTime = 0.3f;   // a row every 300 ms
constexpr int kRows = 16;

void rect(Overlay& o, glm::vec4 r, const glm::vec4& color) {
    const uint32_t c = pack_rgba(color);
    const ColorVertex v[4] = {{{r.x, r.y, 0}, c}, {{r.z, r.y, 0}, c}, {{r.z, r.w, 0}, c}, {{r.x, r.w, 0}, c}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

struct Row {
    std::string label, value;
    bool header = false, separator = false, total = false;
};

// int(x + 0.5) as the game rounds (toward zero below -0.5).
int round_credits(float x) { return int(x + 0.5f); }

}  // namespace

EndLevelScreen::Credits EndLevelScreen::credits(const Stats& st, const Rules& r) {
    // GS_EndLevel::Create.
    Credits c;
    c.kills = st.headshots * r.per_kill[0] + st.blast_kills * r.per_kill[1] + st.paralyze_kills * r.per_kill[2] +
              st.slow_kills * r.per_kill[3] + st.kills * r.per_kill[4];
    const float base = float(c.kills);
    c.shield = st.criticals == 0 ? r.shield[0] : st.criticals < 4 ? r.shield[1] : -r.shield[2];
    int bonus = round_credits(base * 0.01f * float(c.shield));
    c.accuracy = int(std::min(st.accuracy, 1.0f) * 100);
    c.accuracy_bonus = round_credits(float(r.accuracy) * 0.01f * std::min(st.accuracy, 1.0f) * 100);
    bonus += round_credits(base * 0.01f * float(c.accuracy_bonus));
    const float minutes = st.play_time / 60;
    c.time = int(minutes <= r.limit_minutes[0] ? r.time[0] : minutes <= r.limit_minutes[1] ? r.time[1] : r.time[2]);
    bonus += round_credits(base * 0.01f * float(c.time));
    bonus -= round_credits(base * float(st.retries) * 0.01f * float(r.retry));
    c.total = c.kills + bonus;
    return c;
}

EndLevelScreen::Rules EndLevelScreen::read_rules(const Gol& gol, float limit_1, float limit_2) {
    Rules r;
    if (limit_1 > 0 && limit_2 > 0) r.limit_minutes[0] = limit_1, r.limit_minutes[1] = limit_2;
    auto it = gol.find(3038);
    const Value* v = it != gol.end() ? resolved_template(it->second, gol, 0xa4) : nullptr;
    if (!v || v->size() < 13) return r;
    for (int k = 0; k < 5; ++k) r.per_kill[k] = int((*v)[size_t(k)].as_float());
    for (int k = 0; k < 3; ++k) r.shield[k] = int((*v)[size_t(5 + k)].as_float());
    r.accuracy = int((*v)[8].as_float()), r.retry = int((*v)[9].as_float());
    for (int k = 0; k < 3; ++k) r.time[k] = (*v)[size_t(10 + k)].as_float();
    return r;
}

void EndLevelScreen::show(const Stats& stats, const Rules& rules, bool game_finished) {
    open = true;
    stats_ = stats, rules_ = rules, finished_ = game_finished;
    credits_ = credits(stats, rules);
    time_ = 0, selected_ = 1, pressed_ = -1;
}

void EndLevelScreen::update(float dt) {
    if (open) time_ += dt;
}

bool EndLevelScreen::ready() const { return time_ > kRows * kRowTime; }

EndLevelScreen::Action EndLevelScreen::key(Key k) {
    if (!ready()) {   // a press shows the rest at once
        if (k == Key::Activate) time_ = kRows * kRowTime + 0.01f;
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
        if (phase == Pointer::Release) time_ = kRows * kRowTime + 0.01f;
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
    // Each row 300 ms after the one before: its label slides in from the left and its value,
    // counting up, from the right as they fade in.
    auto row_t = [&](size_t i) { return std::clamp((time_ - float(i) * kRowTime) / kRowTime, 0.0f, 1.0f); };
    auto up = [&](size_t i, float value) { return int(value * row_t(i)); };
    char buf[64];
    auto number = [](int n) { return std::to_string(n); };
    const float shown_time = stats_.play_time * row_t(10);
    std::snprintf(buf, sizeof buf, "%d:%02d", int(shown_time) / 60, int(shown_time) % 60);
    const std::string play_time = buf;
    const int counts[5] = {up(1, float(stats_.headshots)), up(2, float(stats_.blast_kills)), up(3, float(stats_.paralyze_kills)),
                           up(4, float(stats_.slow_kills)), up(5, float(stats_.kills))};
    const int shield = up(8, float(credits_.shield)), accuracy = up(9, float(credits_.accuracy)), retries = up(13, float(stats_.retries));
    // TOTAL CREDITS as the rows have counted so far (GS_EndLevel::Render).
    const float base = float(credits_.kills);
    int total = round_credits(base * float(shield) * 0.01f) + round_credits(base * float(up(9, float(credits_.accuracy_bonus))) * 0.01f) -
                round_credits(base * float(retries) * 0.01f * float(rules_.retry));
    for (int k = 0; k < 5; ++k) total += counts[k] * rules_.per_kill[k];
    if (row_t(10) >= 1) total += round_credits(float(credits_.time) * 0.01f * base);
    const std::vector<Row> list = {
        {text("FPS_STATISTICS"), "", true},
        {text("FPS_HEADSHOTS"), number(counts[0])},
        {text("FPS_BLAST_KILLS"), number(counts[1])},
        {text("FPS_PARALYZE_KILLS"), number(counts[2])},
        {text("FPS_SLOW_TIME_KILLS"), number(counts[3])},
        {text("FPS_TOTAL_ENEMIES"), number(counts[4])},
        {"", "", false, true},
        {text("FPS_BONUS_CREDITS"), "", true},
        {text("FPS_SHIELD_BONUS"), number(shield) + "%"},
        {text("FPS_ACCURACY"), number(accuracy) + "%"},
        {text("FPS_PLAY_TIME"), play_time},
        {"", "", false, true},
        {text("FPS_PENALTIES"), "", true},
        {text("FPS_RETRIES"), number(retries)},
        {"", "", false, true},
        {text("FPS_TOTAL_CREDITS"), number(total), false, false, true},
    };
    hud.set_screen(w, h);
    const Sprite* cb = hud.sprite("Chatboard.bsprite");
    const float k = hud.sprite_scale("Chatboard.bsprite");
    for (size_t i = 0; i < list.size(); ++i) {
        const float t = row_t(i);
        if (t <= 0) break;
        const float slide = (1 - t) * 100;
        const glm::vec4 color(1, 1, 1, t);
        const Row& r = list[i];
        if (r.separator) {
            if (cb) {   // centred under the rows
                const glm::vec4 b = Hud::frame_bounds(*cb, kSeparator);
                hud.draw_frame(out, *cb, kSeparator, px(240, y + 1) - glm::vec2((b.x + b.z) * 0.5f * k, 0), k, color);
            }
            y += 10;
            continue;
        }
        const glm::vec4 label_color = r.header ? glm::vec4(1, 0.72f, 0.3f, t) : r.total ? glm::vec4(72, 163, 203, 255 * t) / 255.0f : color;
        rows.draw(out, r.label, px(128 - slide, y), w, label_color, {0.0f, 0.0f});
        float right = 352 + slide;
        if (r.total && cb) {   // the credits sign after the sum
            const glm::vec4 b = Hud::frame_bounds(*cb, kCredits);
            const float icon_w = (b.z - b.x) * k, icon_h = (b.w - b.y) * k;
            const glm::vec2 at = px(right, y) + glm::vec2(-icon_w, rows.measure("0", w).y * 0.5f - icon_h * 0.5f);
            hud.draw_frame(out, *cb, kCredits, at - glm::vec2(b.x, b.y) * k, k, color);
            right -= icon_w / s + 2;
        }
        if (!r.value.empty()) rows.draw(out, r.value, px(right, y), w, color, {1.0f, 0.0f});
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
