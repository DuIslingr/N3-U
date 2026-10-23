// The end of a level (GS_EndLevel): over a darkened view, CONGRATULATIONS! and the level's
// STATISTICS, a row every 300 ms sliding in from the left as it fades in, then Back to Menu
// and Continue (Chatboard's slanted buttons at (59, 280) and (252, 280) of 480x320).
//
// The original also counts the credits the level earned (its shop's currency); this port
// has no shop, so the rows show the statistics alone.
#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "game/hud.h"
#include "render/renderer.h"

namespace nova {

class TextRenderer;

class EndLevelScreen {
public:
    struct Stats {
        int headshots = 0, blast_kills = 0, paralyze_kills = 0, slow_kills = 0, kills = 0, retries = 0;
        float shield = 1, accuracy = 0, play_time = 0;   // 0..1, 0..1, seconds
    };
    std::function<std::string(const std::string&)> text = [](const std::string& id) { return id; };
    bool open = false;
    void show(const Stats& stats, bool game_finished);
    void update(float dt);

    enum class Action { None, Continue, Menu };
    enum class Key { Left, Right, Activate };
    Action key(Key k);
    enum class Pointer { Move, Press, Release };
    Action pointer(glm::vec2 at, Pointer phase);
    void draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& title, TextRenderer& rows, int w, int h);

private:
    Stats stats_;
    bool finished_ = false;
    float time_ = 0;
    int selected_ = 1, pressed_ = -1;   // 0 Back to Menu, 1 Continue
    glm::vec4 rects_[2]{};
    bool ready() const;
};

}  // namespace nova
