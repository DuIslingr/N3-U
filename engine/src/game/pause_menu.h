// The in-game menu, after the original's GS_InGameMenu: Esc (or the HUD's pause button on
// touch screens) pauses the game. Pages as the original has them, with its strings and its
// Chatboard.bsprite art (buttons 187/188 and 29/30, toggles 123/124, sliders 127-129):
//   PAUSE: Objectives, Restart Checkpoint, Restart Level, Options, Exit
//   Options: Sound, Controls, Display (this port's), Advanced
//   Sound: Game Sound, SFX Volume, Music Volume
//   Controls: Sensitivity, Left-Handed (touch), Invert Y-Axis, Aim (mouse: hold or toggle)
//   Display: Resolution (16:9, up to 3840x2160), Display Mode, V-Sync, Frame Rate
//   Advanced: Aim Assist, Auto-Switch, Auto-Leveling, Metric
// Driven by the keys, the mouse or a finger; settings change as they are picked.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/hud.h"
#include "game/settings.h"
#include "render/renderer.h"

namespace nova {

class TextRenderer;

class PauseMenu {
public:
    bool open = false;
    void show();    // open at the PAUSE page
    // String lookup (the game's string tables: "FPS_PAUSE" ...); the id itself if missing.
    std::function<std::string(const std::string&)> text = [](const std::string& id) { return id; };
    bool touch = false;   // the touch scheme: Left-Handed shows, the mouse aim option does not

    enum class Action { None, Resume, Changed, DisplayChanged, RestartCheckpoint, RestartLevel, Exit };
    enum class Key { Up, Down, Left, Right, Activate, Back };
    Action key(Key k, Settings& s);
    // The pointer (mouse or finger) at `at` (pixels): moved, pressed or released.
    enum class Pointer { Move, Press, Release };
    Action pointer(glm::vec2 at, Pointer phase, Settings& s);

    void draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& title, TextRenderer& items,
              const std::vector<Hud::ObjectiveLine>& objectives, const Settings& s, int w, int h);

private:
    enum class Page { Pause, Objectives, Options, Sound, Controls, Display, Advanced, Confirm };
    struct Row {
        enum class Kind { Button, Toggle, Slider, Choice } kind = Kind::Button;
        std::string label;
        std::string value;                          // toggles and choices: what it is set to
        bool on = false;                            // toggles
        float fraction = 0;                         // sliders, 0..1
        std::function<Action(Settings&, int)> change;   // -1/+1 left/right, 0 activate
        std::function<Action(Settings&, float)> set;    // sliders: to a fraction
    };
    std::vector<Row> rows(const Settings& s);
    Action back();
    Action activate(int row, int direction, Settings& s);

    std::vector<Page> stack_{Page::Pause};
    Page page() const { return stack_.back(); }
    Action confirm_action_ = Action::None;
    std::string confirm_text_;
    int selected_ = 0;
    int dragging_ = -1;                             // a slider row held by the pointer
    struct Rect {
        glm::vec4 row{0}, control{0};               // x0, y0, x1, y1
    };
    std::vector<Rect> rects_;
};

}  // namespace nova
