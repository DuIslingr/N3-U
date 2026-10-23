#include "game/pause_menu.h"

#include <algorithm>
#include <cstdio>

#include "render/text.h"

namespace nova {

namespace {

void rect(Overlay& o, glm::vec4 r, const glm::vec4& color) {
    const uint32_t c = pack_rgba(color);
    const ColorVertex v[4] = {{{r.x, r.y, 0}, c}, {{r.z, r.y, 0}, c}, {{r.z, r.w, 0}, c}, {{r.x, r.w, 0}, c}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

bool inside(const glm::vec4& r, glm::vec2 p) { return p.x >= r.x && p.x <= r.z && p.y >= r.y && p.y <= r.w; }

const glm::vec4 kText(0.75f, 0.95f, 1.0f, 1.0f);   // the HUD's cyan
const glm::vec4 kWhite(1);

// Chatboard.bsprite frames (GS_InGameMenu).
constexpr int kPauseButton = 187, kPauseButtonOn = 188, kButton = 29, kButtonOn = 30;
constexpr int kToggleOn = 123, kToggleOff = 124, kSliderTrack = 127, kSliderKnob = 128, kSliderKnobOn = 129;
constexpr int kArrowLeft = 193, kArrowLeftOn = 194, kArrowRight = 195, kArrowRightOn = 196;

// The display choices: 16:9 sizes, modes, frame-rate caps (0: unlocked).
struct Size {
    int w, h;
};
constexpr Size kResolutions[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
constexpr int kFrameRates[] = {0, 60, 75, 90, 120, 144, 165, 240};

template <typename T, size_t N>
int step(const T (&list)[N], int index, int direction) {
    return (index + (direction == 0 ? 1 : direction) + int(N)) % int(N);
}

}  // namespace

void PauseMenu::show() {
    open = true;
    stack_ = {Page::Pause};
    selected_ = 0, dragging_ = -1;
}

PauseMenu::Action PauseMenu::back() {
    dragging_ = -1;
    if (stack_.size() <= 1) return Action::Resume;
    const Page from = page();
    stack_.pop_back();
    // Back on the parent page, on the entry that led to the page left.
    selected_ = 0;
    const std::vector<Page> order = page() == Page::Options
                                        ? std::vector<Page>{Page::Sound, Page::Controls, Page::Display, Page::Advanced}
                                        : std::vector<Page>{Page::Objectives};
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == from) selected_ = page() == Page::Pause ? 0 : int(i);
    if (page() == Page::Pause && from == Page::Options) selected_ = 3;
    return Action::None;
}

std::vector<PauseMenu::Row> PauseMenu::rows(const Settings& s) {
    std::vector<Row> out;
    auto button = [&](const std::string& label, std::function<Action(Settings&, int)> f) {
        Row r;
        r.kind = Row::Kind::Button, r.label = label, r.change = std::move(f);
        out.push_back(std::move(r));
    };
    auto go = [this](Page p) {
        return [this, p](Settings&, int d) {
            if (d != 0) return Action::None;
            stack_.push_back(p), selected_ = 0;
            return Action::None;
        };
    };
    auto confirm = [this](const std::string& question, Action a) {
        return [this, question, a](Settings&, int d) {
            if (d != 0) return Action::None;
            confirm_text_ = question, confirm_action_ = a;
            stack_.push_back(Page::Confirm), selected_ = 1;   // "No" first, as a guard
            return Action::None;
        };
    };
    auto toggle = [&](const std::string& label, bool on, std::function<void(Settings&)> flip, Action result = Action::Changed) {
        Row r;
        r.kind = Row::Kind::Toggle, r.label = label, r.on = on;
        r.value = text(on ? "FPS_ON" : "FPS_OFF");
        r.change = [flip, result](Settings& st, int) { return flip(st), result; };
        out.push_back(std::move(r));
    };
    auto slider = [&](const std::string& label, float fraction, std::function<void(Settings&, float)> to) {
        Row r;
        r.kind = Row::Kind::Slider, r.label = label, r.fraction = std::clamp(fraction, 0.0f, 1.0f);
        r.set = [to](Settings& st, float f) { return to(st, std::clamp(f, 0.0f, 1.0f)), Action::Changed; };
        const float now = r.fraction;
        r.change = [to, now](Settings& st, int d) {
            if (d == 0) return Action::None;
            return to(st, std::clamp(now + 0.1f * float(d), 0.0f, 1.0f)), Action::Changed;
        };
        out.push_back(std::move(r));
    };
    auto choice = [&](const std::string& label, const std::string& value, std::function<Action(Settings&, int)> f) {
        Row r;
        r.kind = Row::Kind::Choice, r.label = label, r.value = value, r.change = std::move(f);
        out.push_back(std::move(r));
    };
    switch (page()) {
        case Page::Pause:
            button(text("FPS_OBJECTIVES"), go(Page::Objectives));
            button(text("FPS_RESTART_CHECKPOINT"), confirm(text("FPS_CONFIRM_RESTART_CHECKPOINT"), Action::RestartCheckpoint));
            button(text("FPS_RESTART_LEVEL"), confirm(text("FPS_CONFIRM_RESTART_LEVEL"), Action::RestartLevel));
            button(text("FPS_OPTIONS"), go(Page::Options));
            button(text("FPS_EXIT"), confirm(text("FPS_CONFIRM_EXIT_GAME"), Action::Exit));
            break;
        case Page::Objectives:
            break;   // drawn from the objectives; only Back
        case Page::Options:
            button(text("FPS_SOUND"), go(Page::Sound));
            button(text("FPS_CONTROLS"), go(Page::Controls));
            button("Display", go(Page::Display));
            button(text("FPS_ADVANCED"), go(Page::Advanced));
            break;
        case Page::Sound:
            toggle(text("GAME_SOUND"), s.sound, [](Settings& st) { st.sound = !st.sound; });
            slider(text("FPS_FX"), s.sfx_volume, [](Settings& st, float f) { st.sfx_volume = f; });
            slider(text("FPS_MUSIC"), s.music_volume, [](Settings& st, float f) { st.music_volume = f; });
            break;
        case Page::Controls:
            slider(text("FPS_SENSITIVITY"), (s.sensitivity - 0.25f) / 2.25f, [](Settings& st, float f) { st.sensitivity = 0.25f + f * 2.25f; });
            if (touch) toggle(text("FPS_LEFT_HANDED"), s.left_handed, [](Settings& st) { st.left_handed = !st.left_handed; });
            toggle(text("FPS_INVERT_Y_AXIS"), s.invert_y, [](Settings& st) { st.invert_y = !st.invert_y; });
            if (!touch)
                choice("AIM (RIGHT MOUSE BUTTON)", s.aim_toggle ? "TOGGLE" : "HOLD",
                       [](Settings& st, int) { return st.aim_toggle = !st.aim_toggle, Action::Changed; });
            break;
        case Page::Display: {
            int res = 1;
            for (int i = 0; i < int(std::size(kResolutions)); ++i)
                if (kResolutions[i].w == s.width && kResolutions[i].h == s.height) res = i;
            char size[32];
            std::snprintf(size, sizeof size, "%d x %d", s.width, s.height);
            choice("RESOLUTION", size, [res](Settings& st, int d) {
                const int i = step(kResolutions, res, d);
                st.width = kResolutions[i].w, st.height = kResolutions[i].h;
                return Action::DisplayChanged;
            });
            static const char* const kModes[] = {"WINDOWED", "FULLSCREEN", "BORDERLESS"};
            choice("DISPLAY MODE", kModes[int(s.display)], [](Settings& st, int d) {
                st.display = Settings::DisplayMode(step(kModes, int(st.display), d));
                return Action::DisplayChanged;
            });
            toggle("V-SYNC", s.vsync, [](Settings& st) { st.vsync = !st.vsync; }, Action::DisplayChanged);
            int rate = 0;
            for (int i = 0; i < int(std::size(kFrameRates)); ++i)
                if (kFrameRates[i] == s.fps_limit) rate = i;
            choice("FRAME RATE LIMIT", s.fps_limit == 0 ? "UNLOCKED" : std::to_string(s.fps_limit) + " FPS", [rate](Settings& st, int d) {
                st.fps_limit = kFrameRates[step(kFrameRates, rate, d)];
                return Action::DisplayChanged;
            });
            break;
        }
        case Page::Advanced:
            toggle(text("FPS_MAGNETIC_CURSOR"), s.aim_assist, [](Settings& st) { st.aim_assist = !st.aim_assist; });
            toggle(text("FPS_AUTO_SWITCH"), s.auto_switch, [](Settings& st) { st.auto_switch = !st.auto_switch; });
            toggle(text("FPS_AUTO_LEVELING"), s.auto_leveling, [](Settings& st) { st.auto_leveling = !st.auto_leveling; });
            toggle(text("FPS_METRIC"), s.metric, [](Settings& st) { st.metric = !st.metric; });
            break;
        case Page::Confirm: {
            const Action a = confirm_action_;
            button(text("FPS_YES"), [this, a](Settings&, int d) {
                if (d != 0) return Action::None;
                stack_ = {Page::Pause}, selected_ = 0;
                return a;
            });
            button(text("FPS_NO"), [this](Settings&, int d) { return d == 0 ? back() : Action::None; });
            return out;
        }
    }
    if (page() != Page::Pause) button(text("FPS_BACK"), [this](Settings&, int d) { return d == 0 ? back() : Action::None; });
    return out;
}

PauseMenu::Action PauseMenu::activate(int row, int direction, Settings& s) {
    std::vector<Row> list = rows(s);
    if (row < 0 || row >= int(list.size()) || !list[size_t(row)].change) return Action::None;
    return list[size_t(row)].change(s, direction);
}

PauseMenu::Action PauseMenu::key(Key k, Settings& s) {
    const int n = int(rows(s).size());
    switch (k) {
        case Key::Up: selected_ = n ? (selected_ + n - 1) % n : 0; return Action::None;
        case Key::Down: selected_ = n ? (selected_ + 1) % n : 0; return Action::None;
        case Key::Left: return activate(selected_, -1, s);
        case Key::Right: return activate(selected_, 1, s);
        case Key::Activate: return activate(selected_, 0, s);
        case Key::Back: return back();
    }
    return Action::None;
}

PauseMenu::Action PauseMenu::pointer(glm::vec2 at, Pointer phase, Settings& s) {
    if (phase == Pointer::Release) {
        dragging_ = -1;
        return Action::None;
    }
    std::vector<Row> list = rows(s);
    if (dragging_ >= 0 && dragging_ < int(rects_.size()) && dragging_ < int(list.size())) {
        const glm::vec4& c = rects_[size_t(dragging_)].control;
        return list[size_t(dragging_)].set(s, (at.x - c.x) / std::max(c.z - c.x, 1.0f));
    }
    for (int i = 0; i < int(rects_.size()) && i < int(list.size()); ++i) {
        if (!inside(rects_[size_t(i)].row, at)) continue;
        selected_ = i;
        if (phase != Pointer::Press) return Action::None;
        const Row& r = list[size_t(i)];
        const glm::vec4& c = rects_[size_t(i)].control;
        if (r.kind == Row::Kind::Slider) {
            dragging_ = i;
            return r.set(s, (at.x - c.x) / std::max(c.z - c.x, 1.0f));
        }
        // Choices: the left half of the value steps back, the right half forwards.
        if (r.kind == Row::Kind::Choice) return r.change(s, at.x < (c.x + c.z) * 0.5f ? -1 : 1);
        return r.change(s, 0);
    }
    return Action::None;
}

void PauseMenu::draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& title, TextRenderer& items,
                     const std::vector<Hud::ObjectiveLine>& objectives, const Settings& s, int w, int h) {
    const float W = float(w), H = float(h);
    hud.set_screen(w, h);
    const Sprite* art = hud.sprite("Chatboard.bsprite");
    const float k = hud.sprite_scale("Chatboard.bsprite");
    rect(overlay, {0, 0, W, H}, {0, 0.02f, 0.04f, 0.72f});   // the game, dimmed behind
    // The page's title, with the panel's frame line under it.
    static const char* const kTitles[] = {"FPS_PAUSE", "FPS_OBJECTIVES", "FPS_OPTIONS", "FPS_SOUND", "FPS_CONTROLS", "", "FPS_ADVANCED", ""};
    std::string heading = page() == Page::Display ? "DISPLAY" : page() == Page::Confirm ? confirm_text_ : text(kTitles[int(page())]);
    if (page() != Page::Confirm)
        for (char& c : heading) c = char(std::toupper(static_cast<unsigned char>(c)));
    const float line = items.measure("Ag", 100000).y;
    title.draw(out, heading, {W * 0.5f, H * 0.1f}, int(W * 0.8f), kText);
    rect(overlay, {W * 0.2f, H * 0.1f + title.measure(heading, int(W * 0.8f)).y + line * 0.3f, W * 0.8f,
                   H * 0.1f + title.measure(heading, int(W * 0.8f)).y + line * 0.3f + std::max(1.5f, H / 540.0f)},
         {kText.r, kText.g, kText.b, 0.6f});

    std::vector<Row> list = rows(s);
    rects_.assign(list.size(), Rect{});
    const bool buttons_page = page() == Page::Pause || page() == Page::Options || page() == Page::Confirm;
    float y = H * 0.27f;
    if (page() == Page::Objectives) {   // the objectives box's lines, then Back
        for (const Hud::ObjectiveLine& o : objectives) {
            const std::string mark = o.primary ? "" : o.done ? "[x] " : "[ ] ";
            y += items.draw(out, mark + o.text, {W * 0.25f, y}, int(W * 0.5f), o.done && !o.primary ? glm::vec4(0.6f, 0.9f, 0.6f, 1) : kWhite,
                            {0.0f, 0.0f}) + line * 0.5f;
        }
        y = std::max(y + line, H * 0.7f);
    }
    const float row_h = line * (buttons_page ? 2.4f : 2.2f);
    const float left = W * 0.5f - std::min(W * 0.36f, H * 0.62f), right = W * 0.5f + std::min(W * 0.36f, H * 0.62f);
    for (int i = 0; i < int(list.size()); ++i) {
        const Row& r = list[size_t(i)];
        const bool sel = i == selected_;
        if (!buttons_page && i + 1 == int(list.size()) && r.kind == Row::Kind::Button) y += row_h * 0.6f;   // Back, apart
        const float cy = y + row_h * 0.5f;
        Rect& rc = rects_[size_t(i)];
        if (r.kind == Row::Kind::Button) {
            // The original's buttons: the pause page's slim bars, the options pages' others.
            const int frame = page() == Page::Pause || page() == Page::Confirm ? (sel ? kPauseButtonOn : kPauseButton) : (sel ? kButtonOn : kButton);
            float bw = W * 0.3f;
            if (art) {
                const glm::vec4 b = Hud::frame_bounds(*art, frame);
                const float scale = k * 1.1f;
                bw = (b.z - b.x) * scale;
                hud.draw_frame(out, *art, frame, {W * 0.5f - (b.x + b.z) * 0.5f * scale, cy - (b.y + b.w) * 0.5f * scale}, scale, kWhite);
            }
            items.draw(out, r.label, {W * 0.5f, cy - line * 0.5f}, int(W), sel ? kWhite : kText);
            rc.row = rc.control = {W * 0.5f - bw * 0.5f, cy - row_h * 0.45f, W * 0.5f + bw * 0.5f, cy + row_h * 0.45f};
        } else {
            rc.row = {left, y, right, y + row_h};
            if (sel) rect(overlay, rc.row, {0.15f, 0.45f, 0.6f, 0.28f});
            items.draw(out, r.label, {left + line * 0.6f, cy - line * 0.5f}, int(W * 0.4f), sel ? kWhite : kText, {0.0f, 0.0f});
            const float cx1 = right - line * 0.6f, cx0 = W * 0.5f + line;
            rc.control = {cx0, y, cx1, y + row_h};
            if (r.kind == Row::Kind::Toggle) {
                // The original's switch: the lit box when on, the red one when off, with ON/OFF.
                const int frame = r.on ? kToggleOn : kToggleOff;
                float bw = line * 4;
                if (art) {
                    const glm::vec4 b = Hud::frame_bounds(*art, frame);
                    const float scale = std::min(k, row_h * 0.85f / std::max(b.w - b.y, 1.0f));
                    bw = (b.z - b.x) * scale;
                    hud.draw_frame(out, *art, frame, {cx1 - b.z * scale, cy - (b.y + b.w) * 0.5f * scale}, scale, kWhite);
                }
                items.draw(out, r.value, {cx1 - bw * 0.5f, cy - line * 0.5f}, int(W), kWhite);
            } else if (r.kind == Row::Kind::Slider) {
                // The track, and its knob at the value.
                if (art) {
                    const glm::vec4 b = Hud::frame_bounds(*art, kSliderTrack);
                    const float scale = (cx1 - cx0) / std::max(b.z - b.x, 1.0f);
                    hud.draw_frame(out, *art, kSliderTrack, {cx0 - b.x * scale, cy - (b.y + b.w) * 0.5f * scale}, scale, kWhite);
                    const int knob = sel ? kSliderKnobOn : kSliderKnob;
                    const glm::vec4 kb = Hud::frame_bounds(*art, knob);
                    const float ks = row_h * 0.7f / std::max(kb.w - kb.y, 1.0f);
                    const float kx = cx0 + (cx1 - cx0) * r.fraction;
                    hud.draw_frame(out, *art, knob, {kx - (kb.x + kb.z) * 0.5f * ks, cy - (kb.y + kb.w) * 0.5f * ks}, ks, kWhite);
                } else {
                    rect(overlay, {cx0, cy - 2, cx1, cy + 2}, kText);
                }
            } else if (r.kind == Row::Kind::Choice) {
                // The value between the original's arrow brackets.
                const float mid = (cx0 + cx1) * 0.5f;
                items.draw(out, r.value, {mid, cy - line * 0.5f}, int(W * 0.4f), kWhite);
                if (art) {
                    const glm::vec4 b = Hud::frame_bounds(*art, kArrowLeft);
                    const float scale = row_h * 0.6f / std::max(b.w - b.y, 1.0f);
                    hud.draw_frame(out, *art, sel ? kArrowLeftOn : kArrowLeft, {cx0 - b.x * scale, cy - (b.y + b.w) * 0.5f * scale}, scale, kWhite);
                    const glm::vec4 rb = Hud::frame_bounds(*art, kArrowRight);
                    hud.draw_frame(out, *art, sel ? kArrowRightOn : kArrowRight, {cx1 - rb.z * scale, cy - (rb.y + rb.w) * 0.5f * scale}, scale,
                                   kWhite);
                }
            }
        }
        y += row_h;
    }
}

}  // namespace nova
