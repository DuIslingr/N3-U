// The original on-screen touch controls (the Touch input scheme): ControlScheme4's default
// layout (ResetCustomization), drawn from controls.bsprite. In 480x320 base pixels scaled
// like the HUD's: the walk pad and the sprint button anchored at (60, H-70); the fire
// button at (W-75, H-70), with the power, aim and jump buttons around it (their frames are
// drawn offset from that same anchor). Sliding a finger anywhere else turns the camera,
// and so does dragging the fire button. The HUD's weapon panel reloads on a tap and
// switches weapons on a swipe.
#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "render/renderer.h"

namespace nova {

class Hud;

class TouchControls {
public:
    explicit TouchControls(Hud& hud) : hud_(hud) {}

    // The screen size in pixels; call before feeding the frame's touches.
    void layout(int w, int h);
    // Touches in pixels. `id` tells fingers apart (the mouse can stand in for one).
    void down(int64_t id, glm::vec2 at);
    void move(int64_t id, glm::vec2 at);
    void up(int64_t id, glm::vec2 at);
    // Drop every touch (controls hidden, focus lost).
    void release_all();

    struct Output {
        float forward = 0, right = 0;   // the walk pad, -1..1
        glm::vec2 look{0};              // degrees to turn: yaw (left +), pitch (up +)
        bool fire = false, jump = false, run = false, aim = false;
        bool reload = false, grenade = false;
        int weapon_step = 0;            // +1 next weapon, -1 previous
    };
    // This frame's input; turning and taps are consumed.
    Output take();

    void draw(ParticleList& out);
    // Controls the scripts have switched off are hidden and ignore touches.
    struct Shown {
        bool pad = true, sprint = true, fire = true, aim = true, jump = true, power = true;
    } shown;
    glm::vec2 pad_anchor() const { return pad_; }

private:
    enum class Part { None, Pad, Look, Fire, Aim, Jump, Power, Sprint, Weapons, Grenades };
    struct Touch {
        int64_t id = 0;
        Part part = Part::None;
        glm::vec2 start{0}, last{0}, at{0};
    };
    Part hit(glm::vec2 at) const;
    bool in_frame(int frame, glm::vec2 anchor, glm::vec2 at, float grow) const;
    Touch* find(int64_t id);

    Hud& hud_;
    int w_ = 0, h_ = 0;
    float sx_ = 1, sy_ = 1, scale_ = 1;      // base pixels -> screen; sprite pixels -> screen
    glm::vec2 pad_{0}, cluster_{0}, weapons_{0}, grenades_{0};
    float pad_radius_ = 1;
    glm::vec2 pad_center_{0};                // where the pad is while held (it follows a far touch)
    std::vector<Touch> touches_;
    Output pending_;
    bool aim_ = false, run_ = false;
};

}  // namespace nova
