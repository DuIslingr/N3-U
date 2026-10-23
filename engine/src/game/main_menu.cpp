#include "game/main_menu.h"

#include <algorithm>
#include <cmath>

#include "render/text.h"

namespace nova {

namespace {

const char* kSprite = "menu_elements_nova3.bsprite";
// menu_elements_nova3 frames.
constexpr int kMainLayout = 16, kChaptersLayout = 17;
constexpr int kCard = 135, kCardSelected = 136, kCardLock = 142;
constexpr int kUp = 140, kUpPressed = 141, kDown = 148, kDownPressed = 149, kBack = 151, kBackPressed = 152;
constexpr int kOptions = 117, kOptionsPressed = 118, kInfo = 113;
constexpr int kCampaign = 132, kMultiplayer = 130;
// Layout marker frames and modules: positions only.
bool marker_frame(int f) { return f == 1 || f == 3 || f == 4 || f == 5 || f == 7; }
// Gameloft's online services in the main menu (the currency button, its logo, Live!).
bool service_frame(int f) { return f == 186 || f == 723 || f == 725; }

const glm::vec4 kWhite(1), kDim(0.55f, 0.62f, 0.7f, 1), kOrange(1.0f, 0.72f, 0.3f, 1);

void rect(Overlay& o, glm::vec4 r, const glm::vec4& color) {
    const uint32_t c = pack_rgba(color);
    const ColorVertex v[4] = {{{r.x, r.y, 0}, c}, {{r.z, r.y, 0}, c}, {{r.z, r.w, 0}, c}, {{r.x, r.w, 0}, c}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

// A part's markers (sprite pixels from its anchor).
struct Markers {
    glm::vec4 touch{0};          // from module 3 to module 4
    glm::vec2 text{0};           // module 5
    glm::vec2 left{0}, right{0}; // modules 17, 18
    bool has_touch = false, has_text = false, has_span = false;
};
Markers markers(const Sprite& s, int frame) {
    Markers m;
    if (frame < 0 || size_t(frame) >= s.frames.size()) return m;
    const Sprite::Frame& f = s.frames[size_t(frame)];
    glm::vec2 a(0), b(0);
    bool has_a = false, has_b = false, has_l = false, has_r = false;
    for (int k = f.first; k < f.first + f.count; ++k) {
        const Sprite::FModule& fm = s.fmodules[size_t(k)];
        if (fm.flags & 16) continue;
        const glm::vec2 p(float(fm.x), float(fm.y));
        switch (fm.module) {
            case 3: a = p, has_a = true; break;
            case 4: b = p, has_b = true; break;
            case 5: if (!m.has_text) m.text = p, m.has_text = true; break;
            case 17: m.left = p, has_l = true; break;
            case 18: m.right = p, has_r = true; break;
            default: break;
        }
    }
    if (has_a && has_b) m.touch = glm::vec4(glm::min(a, b), glm::max(a, b)), m.has_touch = true;
    m.has_span = has_l && has_r;
    return m;
}

}  // namespace

void MainMenu::show() {
    open = true;
    page_ = Page::Main;
    hover_ = pressed_ = Part::None;
}

void MainMenu::select(int chapter) {
    const int n = chapters();
    if (n == 0) return;
    selected_ = std::clamp(chapter, 0, n - 1);
    // Keep the selection among the three cards shown.
    if (selected_ < first_) first_ = selected_;
    if (selected_ > first_ + 2) first_ = selected_ - 2;
    first_ = std::clamp(first_, 0, std::max(n - 3, 0));
}

MainMenu::Action MainMenu::act(Part p) {
    switch (p) {
        case Part::Campaign:
            page_ = Page::Chapters;
            // The latest chapter reached, at the bottom of the three.
            first_ = std::max(0, std::min(unlocked, chapters()) - 3);
            select(std::min(unlocked, chapters()) - 1);
            return Action::None;
        case Part::Options: return Action::Options;
        case Part::Back:
            if (page_ == Page::Chapters) page_ = Page::Main;
            else return Action::Quit;
            return Action::None;
        case Part::Up: first_ = std::max(first_ - 1, 0), select(std::clamp(selected_, first_, first_ + 2)); return Action::None;
        case Part::Down: first_ = std::clamp(first_ + 1, 0, std::max(chapters() - 3, 0)), select(std::clamp(selected_, first_, first_ + 2)); return Action::None;
        case Part::Card0:
        case Part::Card1:
        case Part::Card2: {
            const int c = first_ + (int(p) - int(Part::Card0));
            if (c >= chapters()) return Action::None;
            if (c != selected_) return select(c), Action::None;   // the first press picks the card
            if (c >= unlocked) return Action::None;               // not reached yet
            chosen = c;
            return Action::Play;
        }
        default: return Action::None;
    }
}

MainMenu::Action MainMenu::key(Key k) {
    if (page_ == Page::Main) {
        switch (k) {
            case Key::Up:
            case Key::Left: hover_ = Part::Campaign; return Action::None;
            case Key::Down:
            case Key::Right: hover_ = hover_ == Part::Campaign ? Part::Options : Part::Campaign; return Action::None;
            case Key::Activate: return act(hover_ == Part::None ? Part::Campaign : hover_);
            case Key::Back: return Action::Quit;
        }
        return Action::None;
    }
    switch (k) {
        case Key::Up:
        case Key::Left: select(selected_ - 1); return Action::None;
        case Key::Down:
        case Key::Right: select(selected_ + 1); return Action::None;
        case Key::Activate: return act(Part(int(Part::Card0) + selected_ - first_));
        case Key::Back: return act(Part::Back);
    }
    return Action::None;
}

MainMenu::Action MainMenu::pointer(glm::vec2 at, Pointer phase) {
    Part over = Part::None;
    for (const Hit& h : hits_)
        if (at.x >= h.rect.x && at.x <= h.rect.z && at.y >= h.rect.y && at.y <= h.rect.w) over = h.part;
    hover_ = over;
    if (phase == Pointer::Press) pressed_ = over;
    if (phase != Pointer::Release) return Action::None;
    const Part p = pressed_;
    pressed_ = Part::None;
    return p != Part::None && p == over ? act(p) : Action::None;   // a press and release on the same part
}

void MainMenu::draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& large, TextRenderer& small, int w, int h) {
    const float W = float(w), H = float(h);
    hud.set_screen(w, h);
    rect(overlay, {0, 0, W, H}, {0, 0, 0, 1});
    hits_.clear();
    // The starfield behind, covering the screen.
    if (const Sprite* bg = hud.sprite("menu_background.bsprite")) {
        const glm::vec4 b = Hud::frame_bounds(*bg, 0);
        const glm::vec2 size(std::max(b.z - b.x, 1.0f), std::max(b.w - b.y, 1.0f));
        const float k = std::max(W / size.x, H / size.y);
        hud.draw_frame(out, *bg, 0, glm::vec2(W, H) * 0.5f - (glm::vec2(b.x, b.y) + size * 0.5f) * k, k, kWhite);
    }
    const Sprite* s = hud.sprite(kSprite);
    if (!s) return;
    hud.skip_frames = {3, 4, 5, 7};
    struct Restore {
        Hud& hud;
        ~Restore() { hud.skip_frames.clear(); }
    } restore{hud};
    // The 960x640 layout scaled to the screen's height, centred.
    const float k = hud.sprite_scale(kSprite);
    const int layout = page_ == Page::Main ? kMainLayout : kChaptersLayout;
    const glm::vec4 lb = Hud::frame_bounds(*s, layout);
    const glm::vec2 origin(W * 0.5f - (lb.x + lb.z) * 0.5f * k, H * 0.5f - (lb.y + lb.w) * 0.5f * k);
    auto px = [&](glm::vec2 p) { return origin + p * k; };
    // A part at `at` (layout pixels) as `frame`, its touch rectangle registered as `part`.
    auto part = [&](int frame, glm::vec2 at, Part p, const glm::vec4& color = kWhite) {
        hud.draw_frame(out, *s, frame, px(at), k, color);
        const Markers m = markers(*s, frame);
        if (p != Part::None) {
            const glm::vec4 b = m.has_touch ? m.touch : Hud::frame_bounds(*s, frame);
            hits_.push_back({p, glm::vec4(px(at + glm::vec2(b.x, b.y)), px(at + glm::vec2(b.z, b.w)))});
        }
        return m;
    };
    // The layout's other parts as they are.
    const Sprite::Frame& lf = s->frames[size_t(layout)];
    std::vector<std::pair<int, glm::vec2>> placed;   // interactive parts, drawn below
    for (int i = lf.first; i < lf.first + lf.count; ++i) {
        const Sprite::FModule& fm = s->fmodules[size_t(i)];
        const glm::vec2 at(float(fm.x), float(fm.y));
        if (fm.flags & 16) {
            if (marker_frame(fm.module) || service_frame(fm.module)) continue;
            const bool interactive = fm.module == kCard || fm.module == kUp || fm.module == kDown || fm.module == kBack ||
                                     fm.module == kOptions || fm.module == kCampaign || fm.module == kMultiplayer;
            if (interactive) placed.push_back({fm.module, at});
            else hud.draw_frame(out, *s, fm.module, px(at), k, kWhite);
        }   // (the layouts' lone modules are all markers)
    }
    auto pressed = [&](Part p) { return hover_ == p || pressed_ == p; };
    auto say = [&](const std::string& id) { return text(id); };
    if (page_ == Page::Main) {
        for (const auto& [frame, at] : placed) {
            if (frame == kCampaign) {
                const Markers m = part(kCampaign, at, Part::Campaign);
                large.draw(out, say("FPS_CAMPAIGN"), px(at + m.text), int(W), pressed(Part::Campaign) ? kOrange : kWhite, {0.5f, 0.5f});
            } else if (frame == kMultiplayer) {   // not in this port: shown, but not offered
                const Markers m = part(kMultiplayer, at, Part::None, glm::vec4(0.6f, 0.6f, 0.6f, 1));
                large.draw(out, say("FPS_MULTIPLAYER_CAPITALIZED"), px(at + m.text), int(W), kDim, {0.5f, 0.5f});
            } else if (frame == kOptions) {
                part(pressed(Part::Options) ? kOptionsPressed : kOptions, at, Part::Options);
            }
        }
        return;
    }
    // Chapter select: the three cards, top to bottom in story order.
    std::vector<glm::vec2> cards;
    for (const auto& [frame, at] : placed)
        if (frame == kCard) cards.push_back(at);
    std::sort(cards.begin(), cards.end(), [](glm::vec2 a, glm::vec2 b) { return a.y < b.y; });
    for (size_t slot = 0; slot < cards.size() && slot < 3; ++slot) {
        const int c = first_ + int(slot);
        if (c >= chapters()) break;
        const CampaignLevel& level = (*campaign)[size_t(c)];
        const bool sel = c == selected_, locked = c >= unlocked;
        const Part p = Part(int(Part::Card0) + int(slot));
        const glm::vec4 tint = locked ? glm::vec4(0.45f, 0.45f, 0.5f, 1) : kWhite;
        const Markers m = part(sel ? kCardSelected : kCard, cards[slot], p);
        if (level.image >= 0) hud.draw_frame(out, *s, level.image, px(cards[slot]), k, tint);
        if (locked) hud.draw_frame(out, *s, kCardLock, px(cards[slot]), k, kWhite);
        // The chapter's name on the card's tab; "Continue" (or "Play") across the chosen one.
        if (m.has_span)
            small.draw(out, say(level.title), px(cards[slot] + glm::vec2(m.left.x + 14, m.left.y + 8)), int(W),
                       sel ? kWhite : kDim, {0.0f, 0.5f});
        if (sel && !locked) {
            const std::string label = c == std::min(unlocked, chapters()) - 1 ? say("FPS_COTINUE") : say("FPS_PLAY");
            large.draw(out, label, px(cards[slot] + glm::vec2(247, 57)), int(W), pressed(p) ? kOrange : kWhite, {0.5f, 0.5f});
        }
    }
    // The selection's location, climate and alliance by the layout's text markers.
    std::vector<glm::vec2> texts;
    for (int i = lf.first; i < lf.first + lf.count; ++i) {
        const Sprite::FModule& fm = s->fmodules[size_t(i)];
        if ((fm.flags & 16) && fm.module == 3) texts.push_back({float(fm.x), float(fm.y)});
    }
    std::sort(texts.begin(), texts.end(), [](glm::vec2 a, glm::vec2 b) { return a.y < b.y; });
    if (selected_ < chapters() && texts.size() >= 6) {
        const CampaignLevel& level = (*campaign)[size_t(selected_)];
        const std::string lines[6] = {say("FPS_SP_LOCATION"), say(level.location), say("FPS_SP_CLIMATE"),
                                      say(level.climate), say("FPS_SP_ALLIANCE"), say(level.alliance)};
        for (int i = 0; i < 6; ++i)
            small.draw(out, lines[i], px(texts[size_t(i)] + glm::vec2(i % 2 ? 0.0f : 18.0f, 8)), int(W), i % 2 ? kDim : kWhite,
                       {0.0f, 0.5f});
    }
    for (const auto& [frame, at] : placed) {
        if (frame == kUp && first_ > 0) part(pressed(Part::Up) ? kUpPressed : kUp, at, Part::Up);
        else if (frame == kDown && first_ + 3 < chapters()) part(pressed(Part::Down) ? kDownPressed : kDown, at, Part::Down);
        else if (frame == kBack) part(pressed(Part::Back) ? kBackPressed : kBack, at, Part::Back);
    }
}

}  // namespace nova
