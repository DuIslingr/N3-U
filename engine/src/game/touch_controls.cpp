#include "game/touch_controls.h"

#include <algorithm>
#include <cmath>

#include "game/hud.h"

namespace nova {

namespace {

// controls.bsprite frames.
constexpr int kFire = 0, kFirePressed = 2;
constexpr int kPower = 3, kPowerPressed = 30, kPowerIcon = 4, kPowerIconPressed = 5, kPowerIconRecharging = 13;
constexpr int kAim = 6, kAimOn = 7;
constexpr int kJump = 8, kJumpPressed = 9;
constexpr int kPadBase = 10, kPadRing = 11, kPadKnob = 12;
constexpr int kSprint = 57;

const char* kSprite = "controls.bsprite";

}  // namespace

void TouchControls::layout(int w, int h) {
    if (w == w_ && h == h_) return;
    w_ = w, h_ = h;
    sx_ = float(std::max(w, h)) / 480.0f, sy_ = float(std::min(w, h)) / 320.0f;
    hud_.set_screen(w, h);
    scale_ = hud_.sprite_scale(kSprite);
    // Left-handed (the original's option): the pad on the right, the buttons on the left.
    pad_ = {mirror ? float(w) - 60 * sx_ : 60 * sx_, float(h) - 70 * sy_};
    cluster_ = {mirror ? 75 * sx_ : float(w) - 75 * sx_, float(h) - 70 * sy_};
    // The HUD's panels (Hud::frame places them).
    weapons_ = {float(w) - 70 * sx_, 40 * sy_};
    grenades_ = {float(w) - 33 * sx_, 85 * sy_};
    pause_ = {63 * sx_, 40 * sy_};
    pad_radius_ = 60 * scale_;
    pad_center_ = pad_;
}

bool TouchControls::in_frame(int frame, glm::vec2 anchor, glm::vec2 at, float grow) const {
    const Sprite* s = hud_.sprite(kSprite);
    if (!s) return false;
    glm::vec4 b = Hud::frame_bounds(*s, frame) * scale_;
    if (mirror) b = glm::vec4(-b.z, b.y, -b.x, b.w);
    return at.x >= anchor.x + b.x - grow && at.x <= anchor.x + b.z + grow && at.y >= anchor.y + b.y - grow &&
           at.y <= anchor.y + b.w + grow;
}

TouchControls::Part TouchControls::hit(glm::vec2 at) const {
    if (shown.aim && in_frame(kAim, cluster_, at, 0)) return Part::Aim;
    if (shown.power && in_frame(kPower, cluster_, at, 0)) return Part::Power;
    if (shown.jump && in_frame(kJump, cluster_, at, 0)) return Part::Jump;
    if (shown.fire && glm::distance(at, cluster_) < 80 * scale_) return Part::Fire;
    if (shown.sprint && in_frame(kSprint, pad_, at, 4 * scale_)) return Part::Sprint;
    if (std::abs(at.x - weapons_.x) < 50 * sx_ && std::abs(at.y - weapons_.y) < 22 * sy_) return Part::Weapons;
    if (std::abs(at.x - grenades_.x) < 28 * sx_ && std::abs(at.y - grenades_.y) < 20 * sy_) return Part::Grenades;
    if (std::abs(at.x - pause_.x) < 40 * sx_ && std::abs(at.y - pause_.y) < 22 * sy_) return Part::Pause;
    // The walk pad: on it, or anywhere low on the left (it moves under the finger).
    if (shown.pad && (glm::distance(at, pad_) < pad_radius_ * 1.6f || ((mirror ? at.x > float(w_) * 0.6f : at.x < float(w_) * 0.4f) && at.y > float(h_) * 0.3f)))
        return Part::Pad;
    return Part::Look;
}

TouchControls::Touch* TouchControls::find(int64_t id) {
    for (Touch& t : touches_)
        if (t.id == id) return &t;
    return nullptr;
}

void TouchControls::down(int64_t id, glm::vec2 at) {
    if (find(id)) return;
    Touch t;
    t.id = id, t.part = hit(at), t.start = t.last = t.at = at;
    if (t.part == Part::Pad) {
        // One finger on the pad at a time.
        for (const Touch& o : touches_)
            if (o.part == Part::Pad) t.part = Part::Look;
        if (t.part == Part::Pad) pad_center_ = glm::distance(at, pad_) < pad_radius_ * 1.6f ? pad_ : at;
    }
    switch (t.part) {
        case Part::Aim: aim_ = !aim_; break;
        case Part::Sprint: run_ = !run_; break;
        default: break;
    }
    touches_.push_back(t);
}

void TouchControls::move(int64_t id, glm::vec2 at) {
    Touch* t = find(id);
    if (!t) return;
    if (t->part == Part::Look || t->part == Part::Fire) {
        // Swiping the full screen height turns about 100 degrees.
        const float k = 100.0f / float(std::max(h_, 1));
        pending_.look += glm::vec2(-(at.x - t->last.x), -(at.y - t->last.y)) * k;
    }
    t->last = at;
    t->at = at;
}

void TouchControls::up(int64_t id, glm::vec2 at) {
    Touch* t = find(id);
    if (!t) return;
    move(id, at);
    if (t->part == Part::Weapons) {
        const float dx = at.x - t->start.x;
        if (std::abs(dx) > 20 * sx_) pending_.weapon_step = dx < 0 ? 1 : -1;
        else pending_.reload = true;
    }
    if (t->part == Part::Grenades && glm::distance(at, t->start) < 20 * sx_) pending_.grenade = true;
    if (t->part == Part::Pause) pending_.pause = true;
    if (t->part == Part::Power) {   // a tap uses the power, a swipe switches it
        if (glm::distance(at, t->start) > 20 * sx_) pending_.power_switch = true;
        else pending_.power = true;
    }
    if (t->part == Part::Pad) run_ = false;   // sprinting stops with the walk
    touches_.erase(touches_.begin() + (t - touches_.data()));
}

void TouchControls::release_all() {
    touches_.clear();
    run_ = false;
}

TouchControls::Output TouchControls::take() {
    Output o = pending_;
    pending_ = {};
    for (const Touch& t : touches_) {
        if (t.part == Part::Pad) {
            glm::vec2 d = (t.at - pad_center_) / pad_radius_;
            if (glm::length(d) > 1) d = glm::normalize(d);
            o.right = d.x, o.forward = -d.y;
        }
        if (t.part == Part::Fire) o.fire = true;
        if (t.part == Part::Jump) o.jump = true;
    }
    o.run = run_;
    o.aim = aim_;
    return o;
}

void TouchControls::draw(ParticleList& out) {
    const Sprite* s = hud_.sprite(kSprite);
    if (!s || w_ <= 0) return;
    auto held = [&](Part p) {
        for (const Touch& t : touches_)
            if (t.part == p) return &t;
        return static_cast<const Touch*>(nullptr);
    };
    const glm::vec4 white(1);
    const glm::vec2 fs(mirror ? -scale_ : scale_, scale_);   // left-handed: mirrored
    // Walk pad: the ring at rest; held, the base with the knob under the finger.
    if (shown.pad) {
        if (const Touch* t = held(Part::Pad)) {
            glm::vec2 d = t->at - pad_center_;
            if (glm::length(d) > pad_radius_) d = glm::normalize(d) * pad_radius_;
            hud_.draw_frame(out, *s, kPadBase, pad_center_, fs, white);
            hud_.draw_frame(out, *s, kPadKnob, pad_center_ + d, fs, white);
        } else {
            hud_.draw_frame(out, *s, kPadRing, pad_, fs, white);
        }
    }
    if (shown.sprint) hud_.draw_frame(out, *s, kSprint, pad_, fs, run_ ? glm::vec4(0.6f, 1, 1, 1) : glm::vec4(1, 1, 1, 0.75f));
    if (shown.fire) hud_.draw_frame(out, *s, held(Part::Fire) ? kFirePressed : kFire, cluster_, fs, white);
    if (shown.power) {
        const bool power = held(Part::Power) != nullptr;
        hud_.draw_frame(out, *s, power ? kPowerPressed : kPower, cluster_, fs, white);
        // The selected power's icon: idle, pressed, recharging (Repel 4/5/13, Paralyze
        // 21/22/23, Slow 24/25/26).
        const int base = this->power == "Paralyze" ? 21 : this->power == "Slow" ? 24 : -1;
        const int icon = base < 0 ? (!power_ready ? kPowerIconRecharging : power ? kPowerIconPressed : kPowerIcon)
                                  : (!power_ready ? base + 2 : power ? base + 1 : base);
        hud_.draw_frame(out, *s, icon, cluster_, fs, white);
    }
    if (shown.aim) hud_.draw_frame(out, *s, aim_ ? kAimOn : kAim, cluster_, fs, white);
    if (shown.jump) hud_.draw_frame(out, *s, held(Part::Jump) ? kJumpPressed : kJump, cluster_, fs, white);
}

}  // namespace nova
