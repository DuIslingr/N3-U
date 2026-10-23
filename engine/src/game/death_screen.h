// The death screen (GS_PlayerDead): the view darkens to a grey veil over a second, the
// mission-failed music starts, and two of Chatboard's slanted buttons offer Retry (back
// at the last checkpoint) and Back to Menu, one above the other at the screen's centre.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/hud.h"
#include "render/renderer.h"

namespace nova {

class TextRenderer;

class DeathScreen {
public:
    std::function<std::string(const std::string&)> text = [](const std::string& id) { return id; };
    bool open = false;
    void show();
    void update(float dt);
    bool ready() const { return time_ >= 1.0f; }   // the buttons answer after the fade
    bool music_due();                              // once, when the buttons come up

    enum class Action { None, Retry, Menu };
    enum class Key { Up, Down, Activate };
    Action key(Key k);
    enum class Pointer { Move, Press, Release };
    Action pointer(glm::vec2 at, Pointer phase);
    void draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& font, int w, int h);

private:
    float time_ = 0;
    bool music_ = false;
    int selected_ = 0, pressed_ = -1;
    glm::vec4 rects_[2]{};
};

}  // namespace nova
