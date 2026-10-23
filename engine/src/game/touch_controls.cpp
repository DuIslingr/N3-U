#include "game/touch_controls.h"

#include <algorithm>
#include <cmath>

#include "game/hud.h"

namespace nova {

namespace {

// controls.bsprite frames.
constexpr int kFire = 0, kFirePressed = 2;
constexpr int kPower = 3, kPowerPressed = 30, kPowerIcon = 4, kPowerIconPressed = 5;
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
    scale_ = float(std::min(w, h)) / 640.0f;   // the 960x640 sprites
    pad_ = {60 * sx_, float(h) - 70 * sy_};
    cluster_ = {float(w) - 75 * sx_, float(h) - 70 * sy_};
    // The HUD's panels (Hud::frame places them).
    weapons_ = {float(w) - 70 * sx_, 40 * sy_};
    grenades_ = {float(w) - 33 * sx_, 85 * sy_};
    pad_radius_ = 60 * scale_;
    pad_center_ = pad_;
}

bool TouchControls::in_frame(int frame, glm::vec2 anchor, glm::vec2 at, float grow) const {
    const Sprite* s = hud_.sprite(kSprite);
    if (!s) return false;
    const glm::vec4 b = Hud::frame_bounds(*s, frame) * scale_;
    return at.x >= anchor.x + b.x - grow && at.x <= anchor.x + b.z + grow && at.y >= anchor.y + b.y - grow &&
           at.y <= anchor.y + b.w + grow;
}

TouchControls::Part TouchControls::hit(glm::vec2 at) const {
    if (in_frame(kAim, cluster_, at, 0)) return Part::Aim;
    if (in_frame(kPower, cluster_, at, 0)) return Part::Power;
    if (in_frame(kJump, cluster_, at, 0)) return Part::Jump;
    if (glm::distance(at, cluster_) < 80 * scale_) return Part::Fire;
    if (in_frame(kSprint, pad_, at, 4 * scale_)) return Part::Sprint;
    if (std::abs(at.x - weapons_.x) < 50 * sx_ && std::abs(at.y - weapons_.y) < 22 * sy_) return Part::Weapons;
    if (std::abs(at.x - grenades_.x) < 28 * sx_ && std::abs(at.y - grenades_.y) < 20 * sy_) return Part::Grenades;
    // The walk pad: on it, or anywhere low on the left (it moves under the finger).
    if (glm::distance(at, pad_) < pad_radius_ * 1.6f || (at.x < float(w_) * 0.4f && at.y > float(h_) * 0.3f)) return Part::Pad;
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
    // Walk pad: the ring at rest; held, the base with the knob under the finger.
    if (const Touch* t = held(Part::Pad)) {
        glm::vec2 d = t->at - pad_center_;
        if (glm::length(d) > pad_radius_) d = glm::normalize(d) * pad_radius_;
        hud_.draw_frame(out, *s, kPadBase, pad_center_, scale_, white);
        hud_.draw_frame(out, *s, kPadKnob, pad_center_ + d, scale_, white);
    } else {
        hud_.draw_frame(out, *s, kPadRing, pad_, scale_, white);
    }
    hud_.draw_frame(out, *s, kSprint, pad_, scale_, run_ ? glm::vec4(0.6f, 1, 1, 1) : glm::vec4(1, 1, 1, 0.75f));
    hud_.draw_frame(out, *s, held(Part::Fire) ? kFirePressed : kFire, cluster_, scale_, white);
    const bool power = held(Part::Power) != nullptr;
    hud_.draw_frame(out, *s, power ? kPowerPressed : kPower, cluster_, scale_, white);
    hud_.draw_frame(out, *s, power ? kPowerIconPressed : kPowerIcon, cluster_, scale_, white);
    hud_.draw_frame(out, *s, aim_ ? kAimOn : kAim, cluster_, scale_, white);
    hud_.draw_frame(out, *s, held(Part::Jump) ? kJumpPressed : kJump, cluster_, scale_, white);
}

}  // namespace nova
