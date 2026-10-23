// The in-game HUD (CHud): NV_hud.bdae, a small 3D scene seen through its own camera
// (hud_camera) and drawn over the finished frame: the pause button, the weapon panel with
// its ammo counter and switch arrows, the grenade button. Multiplayer parts, touch zones
// and pressed ("active") states are hidden.
#pragma once

#include <string>
#include <unordered_map>

#include "assets/bsprite.h"

#include <glm/glm.hpp>

#include "render/renderer.h"

namespace nova {

class Scene;
class TextRenderer;

class Hud {
public:
    explicit Hud(Scene& scene) : scene_(scene) {}
    // After the level is loaded.
    void start();
    struct State {
        bool visible = true;
        int clip = 0, reserve = 0;   // ammo
        int clip_size = 1;
        bool grenades = false;
        std::string weapon_sprite;   // the weapon's .bsprite: frame 0 is its icon
    };
    // Set the frame's HUD camera and draw the HUD's numbers into `sprites`.
    void frame(FrameParams& f, int w, int h, const State& s, TextRenderer* numbers, ParticleList& sprites);

private:
    // CHud::SetNodePosition: put a part where the HUD camera sees it at pixel (x, y).
    void place(const std::string& node, glm::vec2 px, const glm::mat4& view_proj, int w, int h);

    const Sprite* sprite(const std::string& name);
    // Draw a sprite frame with its anchor at `at` (pixels), scaled.
    void draw_frame(ParticleList& out, const Sprite& s, int frame, glm::vec2 at, float scale, const glm::vec4& color);

    Scene& scene_;
    std::unordered_map<std::string, Sprite> sprites_;
    size_t object_ = size_t(-1), ammo_ = size_t(-1);
    glm::ivec2 placed_for_{0};
    bool visible_ = false, grenades_ = true;
};

}  // namespace nova
