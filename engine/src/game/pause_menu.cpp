#include "game/pause_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/combat.h"
#include "game/shop.h"
#include "render/text.h"

namespace nova {

namespace {

void rect(Overlay& o, glm::vec4 r, const glm::vec4& color) {
    const uint32_t c = pack_rgba(color);
    const ColorVertex v[4] = {{{r.x, r.y, 0}, c}, {{r.z, r.y, 0}, c}, {{r.z, r.w, 0}, c}, {{r.x, r.w, 0}, c}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

bool inside(const glm::vec4& r, glm::vec2 p) { return p.x >= r.x && p.x <= r.z && p.y >= r.y && p.y <= r.w; }

const glm::vec4 kWhite(1);
const glm::vec4 kLabel(0.88f, 0.94f, 1.0f, 1.0f);
const glm::vec4 kHighlight(1.0f, 0.72f, 0.3f, 1.0f);   // the orange of the original's pressed buttons

// Chatboard.bsprite frames (GS_InGameMenu).
constexpr int kWindow = 112, kPanelTop = 197, kPanelBottom = 198;
constexpr int kPauseButton = 187, kPauseButtonOn = 188, kButton = 29, kButtonOn = 30, kSmallButton = 26, kSmallButtonOn = 27;
constexpr int kToggleOn = 123, kToggleOff = 124, kSlider = 127, kSliderKnob = 128, kSliderKnobOn = 129;
const glm::vec2 kSliderTrack(0.085f, 0.903f);   // the slider's track (between - and +), across its frame
constexpr int kArrowLeft = 193, kArrowLeftOn = 194, kArrowRight = 195, kArrowRightOn = 196;
// The Military Shop (GS_InGameMenu::Create, RenderPausePage): tabs, the drum, the credits sign
// and the credits bar.
constexpr int kShopTab = 189, kShopTabOn = 190, kDrum = 191, kCoin = 192, kCreditsBar = 252;
constexpr int kShopRow = 3;   // Row::slot of the shop's tabs and BUY

// The display choices: 16:9 sizes, modes, frame-rate caps (0: unlocked).
struct Size {
    int w, h;
};
constexpr Size kResolutions[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
constexpr int kFrameRates[] = {0, 60, 75, 90, 120, 144, 165, 240};

template <typename T, size_t N>
int step(const T (&)[N], int index, int direction) {
    return (index + (direction == 0 ? 1 : direction) + int(N)) % int(N);
}

}  // namespace

void PauseMenu::show() {
    open = true;
    stack_ = {Page::Pause};
    selected_ = -1, dragging_ = -1;
    shop_tab_ = 1, shop_pick_ = 0, shop_drum_ = 0;
}

std::vector<int> PauseMenu::shop_list() const {
    std::vector<int> out;
    if (shop)
        for (const Shop::Item& it : shop->items())
            if (it.item == (shop_tab_ == 0)) out.push_back(it.index);
    return out;
}

int PauseMenu::shop_selected() const {
    const std::vector<int> list = shop_list();
    if (list.empty()) return -1;
    const int n = int(list.size());
    return list[size_t((shop_pick_ % n + n) % n)];
}

void PauseMenu::shop_turn(int d) { shop_pick_ += d; }

void PauseMenu::update(float dt) {
    // The drum eases round to the pick.
    shop_drum_ += (float(shop_pick_) - shop_drum_) * std::min(dt * 10.0f, 1.0f);
    if (std::abs(float(shop_pick_) - shop_drum_) < 1e-3f) shop_drum_ = float(shop_pick_);
}

void PauseMenu::show_options() {
    open = true;
    stack_ = {Page::Options};
    selected_ = -1, dragging_ = -1;
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
    auto button = [&](const std::string& label, std::function<Action(Settings&, int)> f, int slot = 0) {
        Row r;
        r.kind = Row::Kind::Button, r.label = label, r.change = std::move(f), r.slot = slot;
        out.push_back(std::move(r));
    };
    auto go = [this](Page p) {
        return [this, p](Settings&, int d) {
            if (d != 0) return Action::None;
            stack_.push_back(p), selected_ = -1;
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
            button(text("FPS_MAIN_MENU"), confirm(text("FPS_CONFIRM_MAINMENU"), Action::Exit));
            if (shop) {   // the shop's tabs and BUY
                for (int tab = 0; tab < 2; ++tab)
                    button(text(tab == 0 ? "FPS_SHOP_ITEM" : "FPS_SHOP_UPGRADE"), [this, tab](Settings&, int d) {
                        if (d == 0 && shop_tab_ != tab) shop_tab_ = tab, shop_pick_ = 0, shop_drum_ = 0;
                        return Action::None;
                    }, kShopRow);
                const int pick = shop_selected();
                button(text(pick >= 0 && shop->owned(pick, s) ? "FPS_MP_OWNED" : "FPS_MP_STORE_BUY"), [this](Settings& st, int d) {
                    if (d != 0 || !combat) return Action::None;
                    return shop->buy(shop_selected(), *combat, st) ? Action::Changed : Action::None;
                }, kShopRow);
            }
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
                stack_ = {Page::Pause}, selected_ = -1;
                return a;
            }, 1);
            button(text("FPS_NO"), [this](Settings&, int d) { return d == 0 ? back() : Action::None; }, 2);
            return out;
        }
    }
    // The original's pair at the bottom: Back and Resume (the pause page has only Resume).
    if (page() != Page::Pause) button(text("FPS_BACK"), [this](Settings&, int d) { return d == 0 ? back() : Action::None; }, 1);
    if (stack_.front() == Page::Pause)   // (from the main menu there is nothing to resume)
        button(text("FPS_RESUME"), [](Settings&, int d) { return d == 0 ? Action::Resume : Action::None; }, 2);
    return out;
}

PauseMenu::Action PauseMenu::activate(int row, int direction, Settings& s) {
    std::vector<Row> list = rows(s);
    if (row < 0 || row >= int(list.size()) || !list[size_t(row)].change) return Action::None;
    return list[size_t(row)].change(s, direction);
}

PauseMenu::Action PauseMenu::key(Key k, Settings& s) {
    const std::vector<Row> list = rows(s);
    const int n = int(list.size());
    switch (k) {
        case Key::Up: selected_ = n ? (selected_ < 0 ? 0 : (selected_ + n - 1) % n) : 0; return Action::None;
        case Key::Down: selected_ = n ? (selected_ < 0 ? 0 : (selected_ + 1) % n) : 0; return Action::None;
        case Key::Left:
        case Key::Right:
            if (page() == Page::Pause && shop) return shop_turn(k == Key::Left ? -1 : 1), Action::None;   // the shop's drum
            // Across the bottom pair; on the page's rows, a step of the setting.
            if (selected_ >= 0 && selected_ < n && list[size_t(selected_)].slot != 0) {
                for (int i = 0; i < n; ++i)
                    if (list[size_t(i)].slot == (k == Key::Left ? 1 : 2)) selected_ = i;
                return Action::None;
            }
            return activate(selected_, k == Key::Left ? -1 : 1, s);
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
    if (page() == Page::Pause && shop && phase == Pointer::Press)   // the drum's arrows and side items turn it
        for (int side = 0; side < 2; ++side)
            if (inside(shop_arrows_[side], at) || inside(shop_sides_[side], at)) return shop_turn(side == 0 ? -1 : 1), Action::None;
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
    // Laid out as the original at 1280x720 (units `u` from the screen's centre), its art at 0.9u
    // per pixel of the 960x640 sheet (the 2x sheets' pixels are half that).
    const float u = std::min(H / 720.0f, W / 1280.0f);
    const float art_scale = 0.905f * u * hud.sprite_scale("Chatboard.bsprite") * 640.0f / H;
    const glm::vec2 centre(W * 0.5f, H * 0.5f);
    auto at = [&](float x, float y) { return centre + glm::vec2(x, y) * u; };
    // The game behind, dimmed, darker at the top and bottom.
    rect(overlay, {0, 0, W, H}, {0, 0, 0, 0.3f});
    rect(overlay, {0, 0, W, 65 * u}, {0, 0.01f, 0.02f, 0.5f});
    rect(overlay, {0, H - 65 * u, W, H}, {0, 0.01f, 0.02f, 0.5f});
    // A frame centred on `c`, its screen box returned.
    auto frame = [&](int f, glm::vec2 c) {
        if (!art) return glm::vec4(c, c);
        const glm::vec4 b = Hud::frame_bounds(*art, f);
        const glm::vec2 o = c - glm::vec2(b.x + b.z, b.y + b.w) * 0.5f * art_scale;
        hud.draw_frame(out, *art, f, o, art_scale, kWhite);
        return glm::vec4(o + glm::vec2(b.x, b.y) * art_scale, o + glm::vec2(b.z, b.w) * art_scale);
    };
    auto say = [&](const std::string& t, glm::vec2 p, const glm::vec4& color, glm::vec2 anchor = {0.5f, 0.5f}, float wrap = 0) {
        items.draw(out, t, p, int(wrap > 0 ? wrap : W), color, anchor);
    };

    std::vector<Row> list = rows(s);
    rects_.assign(list.size(), Rect{});
    auto pair_button = [&](int i, glm::vec2 c) {   // Back / Resume / Yes / No: the slanted buttons
        const bool sel = i == selected_;
        const glm::vec4 box = frame(sel ? kSmallButtonOn : kSmallButton, c);
        say(list[size_t(i)].label, c, kWhite);
        rects_[size_t(i)].row = rects_[size_t(i)].control = box;
    };

    if (page() == Page::Pause) {
        // On the left, the original's panel: the Military Shop (or, without one, the objectives).
        if (art) {
            const glm::vec4 a = Hud::frame_bounds(*art, kPanelTop), b = Hud::frame_bounds(*art, kPanelBottom);
            const glm::vec2 mid((std::min(a.x, b.x) + std::max(a.z, b.z)) * 0.5f, (a.y + b.w) * 0.5f);
            const glm::vec2 o = at(-207, 18) - mid * art_scale;
            hud.draw_frame(out, *art, kPanelTop, o, art_scale, kWhite);
            hud.draw_frame(out, *art, kPanelBottom, o, art_scale, kWhite);
            if (shop) {
                draw_shop(out, hud, *art, title, items, list, s, o - glm::vec2(25, 0) * art_scale, art_scale, W);
            } else {
                title.draw(out, text("FPS_OBJECTIVES"), o + glm::vec2(mid.x, a.y + 26) * art_scale, int(W), kWhite, {0.5f, 0.5f});
                float y = o.y + (a.y + 80) * art_scale;
                for (const Hud::ObjectiveLine& line : objectives) {
                    const std::string mark = line.primary ? "" : line.done ? "[x] " : "[ ] ";
                    const glm::vec4 color = line.done && !line.primary ? glm::vec4(0.6f, 0.9f, 0.6f, 1) : kWhite;
                    y += items.draw(out, mark + line.text, {o.x + 40 * art_scale, y}, int((mid.x * 2 - 80) * art_scale), color, {0.0f, 0.0f}) +
                         12 * u;
                }
            }
        }
        // The column of buttons on the right, Resume under it.
        int n = 0;
        for (int i = 0; i < int(list.size()); ++i) {
            if (list[size_t(i)].slot == kShopRow) continue;   // drawn with the shop
            if (list[size_t(i)].slot != 0) {
                pair_button(i, at(227, 205));
                continue;
            }
            const glm::vec2 c = at(224, -149 + 66 * float(n++));
            const glm::vec4 box = frame(i == selected_ ? kPauseButtonOn : kPauseButton, c);
            say(list[size_t(i)].label, c, kWhite);
            rects_[size_t(i)].row = rects_[size_t(i)].control = box;
        }
        return;
    }

    // The other pages: the original's window, its title in the bar along the top.
    if (art) {
        const glm::vec4 b = Hud::frame_bounds(*art, kWindow);
        const glm::vec2 o = centre - glm::vec2(b.x + b.z, b.y + b.w) * 0.5f * art_scale;
        hud.draw_frame(out, *art, kWindow, o, art_scale, kWhite);
    }
    static const char* const kTitles[] = {"FPS_PAUSE", "FPS_OBJECTIVES", "FPS_OPTIONS", "FPS_SOUND", "FPS_CONTROLS", "", "FPS_ADVANCED", ""};
    const std::string heading = page() == Page::Display ? "Display" : page() == Page::Confirm ? "" : text(kTitles[int(page())]);
    title.draw(out, heading, at(0, -229), int(W), kWhite, {0.5f, 0.5f});
    if (page() == Page::Objectives) {
        float y = (at(0, -170)).y;
        for (const Hud::ObjectiveLine& line : objectives) {
            const std::string mark = line.primary ? "" : line.done ? "[x] " : "[ ] ";
            const glm::vec4 color = line.done && !line.primary ? glm::vec4(0.6f, 0.9f, 0.6f, 1) : kWhite;
            y += items.draw(out, mark + line.text, {at(-300, 0).x, y}, int(600 * u), color, {0.0f, 0.0f}) + 12 * u;
        }
    }
    if (page() == Page::Confirm) say(confirm_text_, at(0, -40), kWhite, {0.5f, 0.5f}, 560 * u);

    float y = -185;   // the top of the page's rows
    for (int i = 0; i < int(list.size()); ++i) {
        const Row& r = list[size_t(i)];
        const bool sel = i == selected_;
        Rect& rc = rects_[size_t(i)];
        if (r.slot != 0) {
            pair_button(i, at(r.slot == 1 ? -186 : 170, 205));
            continue;
        }
        const glm::vec4 label_color = sel ? kHighlight : kLabel;
        switch (r.kind) {
            case Row::Kind::Button: {
                const glm::vec2 c = at(-13, y + 33);
                rc.row = rc.control = frame(sel ? kButtonOn : kButton, c);
                say(r.label, c, kWhite);
                y += 66;
                break;
            }
            case Row::Kind::Toggle: {
                // The label on the left, the original's switch on the right: blue on, red off.
                const float cy = y + 33;
                say(r.label, at(-305, cy), label_color, {0.0f, 0.5f});
                const glm::vec4 b = art ? Hud::frame_bounds(*art, kToggleOn) : glm::vec4(0, 0, 133, 60);
                const glm::vec2 c = at(275, cy) - glm::vec2((b.z - b.x) * 0.5f * art_scale, 0);
                rc.control = frame(r.on ? kToggleOn : kToggleOff, c);
                say(r.value, c - glm::vec2(0, 2 * u), kWhite);
                rc.row = {at(-320, cy - 33), at(290, cy + 33)};
                y += 67;
                break;
            }
            case Row::Kind::Choice: {
                // The label on the left, the value between the original's arrow brackets.
                const float cy = y + 33;
                say(r.label, at(-305, cy), label_color, {0.0f, 0.5f});
                frame(sel ? kArrowLeftOn : kArrowLeft, at(70, cy));
                frame(sel ? kArrowRightOn : kArrowRight, at(265, cy));
                say(r.value, at(167, cy), kWhite);
                rc.control = {at(50, cy - 30), at(285, cy + 30)};
                rc.row = {at(-320, cy - 33), at(290, cy + 33)};
                y += 67;
                break;
            }
            case Row::Kind::Slider: {
                // The label above; the original's slider (- track +), its knob at the value.
                say(r.label, at(-7, y + 13), label_color);
                const glm::vec2 c = at(-7, y + 50);
                const glm::vec4 box = frame(kSlider, c);
                float x0 = c.x - 220 * u, x1 = c.x + 215 * u;
                if (art) x0 = glm::mix(box.x, box.z, kSliderTrack.x), x1 = glm::mix(box.x, box.z, kSliderTrack.y);
                frame(sel ? kSliderKnobOn : kSliderKnob, {x0 + (x1 - x0) * r.fraction, c.y});
                rc.control = {x0, c.y - 25 * u, x1, c.y + 25 * u};
                rc.row = {at(-320, y), at(290, y + 85)};
                y += 85;
                break;
            }
        }
    }
}

void PauseMenu::draw_shop(ParticleList& out, Hud& hud, const Sprite& art, TextRenderer& title, TextRenderer& items,
                          const std::vector<Row>& list, const Settings& s, glm::vec2 origin, float k, float W) {
    // GS_InGameMenu::Create / RenderPausePage, at their 960x640 positions.
    auto P = [&](float x, float y) { return origin + glm::vec2(x, y) * k; };
    auto frame_at = [&](int f, glm::vec2 p, const glm::vec4& color) {
        hud.draw_frame(out, art, f, p, k, color);
        const glm::vec4 b = Hud::frame_bounds(art, f);
        return glm::vec4(p + glm::vec2(b.x, b.y) * k, p + glm::vec2(b.z, b.w) * k);
    };
    // Text with the string tables' colour codes ("#6FD7FFFF#BUY: #FFFFFFFF#%s"), centred on `c`.
    auto coded = [&](const std::string& t, glm::vec2 c) {
        std::vector<std::pair<std::string, glm::vec4>> parts;
        glm::vec4 color = kWhite;
        size_t i = 0;
        while (i < t.size()) {
            if (t[i] == '#' && i + 9 < t.size() + 0 && t[i + 9] == '#') {
                const unsigned v = unsigned(std::stoul(t.substr(i + 1, 8), nullptr, 16));
                color = glm::vec4(float(v >> 24 & 255), float(v >> 16 & 255), float(v >> 8 & 255), float(v & 255)) / 255.0f;
                i += 10;
                continue;
            }
            const size_t next = t.find('#', i + 1);
            const std::string piece = t.substr(i, next == std::string::npos ? std::string::npos : next - i);
            parts.emplace_back(piece, color);
            i += piece.size();
        }
        float width = 0;
        for (const auto& [piece, c2] : parts) width += items.measure(piece, int(W)).x;
        float x = c.x - width * 0.5f;
        for (const auto& [piece, c2] : parts) {
            items.draw(out, piece, {x, c.y}, int(W), c2, {0.0f, 0.5f});
            x += items.measure(piece, int(W)).x;
        }
    };
    const Sprite* icons = hud.sprite("upgrades.bsprite");
    const float icon_k = k * hud.sprite_scale("upgrades.bsprite") / std::max(hud.sprite_scale("Chatboard.bsprite"), 1e-6f);

    // The credits held: the bar at the menu's corner, its sign on the left.
    const glm::vec4 bar = frame_at(kCreditsBar, P(0, 0), kWhite);
    items.draw(out, std::to_string(s.credits), {(bar.x + bar.z) * 0.5f + 18 * k, (bar.y + bar.w) * 0.5f}, int(W), kWhite, {0.5f, 0.5f});
    title.draw(out, text("FPS_MILITARY_SHOP"), P(247, 92), int(W), kWhite, {0.5f, 0.5f});

    // The tabs and BUY (rows of their own, for the keys).
    std::vector<int> rows_here;
    for (int i = 0; i < int(list.size()); ++i)
        if (list[size_t(i)].slot == kShopRow) rows_here.push_back(i);
    const int pick = shop_selected();
    for (size_t t = 0; t < rows_here.size() && t < 2; ++t) {
        const int i = rows_here[t];
        const glm::vec4 box = frame_at(int(t) == shop_tab_ ? kShopTabOn : kShopTab, P(t == 0 ? 49 : 267, 154), kWhite);
        items.draw(out, list[size_t(i)].label, (glm::vec2(box.x, box.y) + glm::vec2(box.z, box.w)) * 0.5f, int(W),
                   i == selected_ ? kHighlight : kWhite, {0.5f, 0.5f});
        rects_[size_t(i)].row = rects_[size_t(i)].control = box;
    }

    // The drum (CButtonCircularSweep): the tab's goods round a cylinder, the pick in front;
    // further round, smaller and fainter, each with its cost under it.
    const std::vector<int> ids = shop_list();
    const glm::vec4 drum_b = Hud::frame_bounds(art, kDrum);
    const glm::vec2 drum = P(252, 351);
    frame_at(kDrum, drum, kWhite);
    const float drum_w = drum_b.z - drum_b.x, radius = drum_w * 0.5f - 30, spacing = drum_w * 2 / 5;
    const int n = int(ids.size());
    std::vector<std::pair<float, int>> order;   // furthest first
    for (int j = 0; j < n; ++j) {
        float off = std::fmod(float(j) - shop_drum_, float(n));
        if (off > n * 0.5f) off -= float(n);
        if (off < -n * 0.5f) off += float(n);
        order.emplace_back(off, j);
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return std::abs(a.first) > std::abs(b.first); });
    const glm::vec4 coin_b = Hud::frame_bounds(art, kCoin);
    for (const auto& [off, j] : order) {
        const float angle = off * spacing / radius;
        if (std::abs(angle) >= 1.5707963f) continue;
        const float x = std::sin(angle) * radius, size = std::cos(angle) * 0.6f + 0.4f, alpha = std::cos(angle) * 0.8f + 0.2f;
        const Shop::Item& it = shop->items()[size_t(ids[size_t(j)])];
        const glm::vec4 c(1, 1, 1, alpha);
        if (icons) hud.draw_frame(out, *icons, it.icon, drum + glm::vec2(x, 0) * k, icon_k * size, c);
        // The cost and the credits sign after it.
        const std::string cost = std::to_string(shop->cost(it.index));
        const glm::vec2 at = drum + glm::vec2(x, 50 * size) * k;
        const float tw = items.measure(cost, int(W)).x;
        items.draw(out, cost, at, int(W), c, {0.5f, 0.5f});
        const glm::vec2 coin = glm::vec2(coin_b.z - coin_b.x, coin_b.w - coin_b.y) * k * size * 0.6f;
        hud.draw_frame(out, art, kCoin, at + glm::vec2(tw * 0.5f + 3 * k, -coin.y * 0.5f) - glm::vec2(coin_b.x, coin_b.y) * k * size * 0.6f,
                       k * size * 0.6f, c);
    }
    // The items either side of the pick turn the drum when touched.
    const float side_x = std::sin(spacing / radius) * radius;
    for (int side = 0; side < 2; ++side) {
        const glm::vec2 c = drum + glm::vec2(side == 0 ? -side_x : side_x, 0) * k;
        shop_sides_[side] = {c - glm::vec2(45, 50) * k, c + glm::vec2(45, 70) * k};
    }
    shop_arrows_[0] = frame_at(kArrowLeft, P(39, 322), kWhite);
    shop_arrows_[1] = frame_at(kArrowRight, P(442, 322), kWhite);
    for (glm::vec4& a : shop_arrows_) a += glm::vec4(-12, -12, 12, 12) * k;   // easier to hit

    // The pick: "BUY: <name>" and what it does.
    if (pick >= 0) {
        const Shop::Item& it = shop->items()[size_t(pick)];
        std::string line = text("FPS_SHOP_BUY");
        if (const size_t p = line.find("%s"); p != std::string::npos) line.replace(p, 2, text(it.name));
        coded(line, P(253, 230));
        items.draw(out, text(it.description), P(253, 470), int(400 * k), kWhite, {0.5f, 0.5f});
    }
    // BUY (Owned once a lasting upgrade is had), faint when it cannot be bought.
    if (rows_here.size() > 2) {
        const int i = rows_here[2];
        const bool can = pick >= 0 && combat && shop->can_buy(pick, *combat, s);
        const glm::vec4 color(1, 1, 1, can ? 1.0f : 0.45f);
        const glm::vec4 box = frame_at(can && i == selected_ ? kSmallButtonOn : kSmallButton, P(89, 511), color);
        items.draw(out, list[size_t(i)].label, (glm::vec2(box.x, box.y) + glm::vec2(box.z, box.w)) * 0.5f, int(W), color, {0.5f, 0.5f});
        rects_[size_t(i)].row = rects_[size_t(i)].control = box;
    }
}

}  // namespace nova
