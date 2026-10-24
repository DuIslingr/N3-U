// The in-game menu, after the original's GS_InGameMenu: Esc (or the HUD's pause button on
// touch screens) pauses the game. Pages as the original has them, with its strings and its
// Chatboard.bsprite art (buttons 187/188 and 29/30, toggles 123/124, sliders 127-129):
//   PAUSE: Objectives, Restart Checkpoint, Restart Level, Options, Back to Menu
//   Options: Sound, Controls, Display (this port's), Advanced
//   Sound: Game Sound, SFX Volume, Music Volume
//   Controls: Sensitivity, Left-Handed (touch), Invert Y-Axis, Aim (mouse: hold or toggle)
//   Display: Resolution (16:9, up to 3840x2160), Display Mode, V-Sync, Frame Rate
//   Advanced: Aim Assist, Auto-Switch, Auto-Leveling, Metric
// Driven by the keys, the mouse or a finger; settings change as they are picked. Beside the
// PAUSE page's buttons, the Military Shop (the original's panel there): its ITEM and UPGRADE
// tabs, a drum of what they sell (the arrows or Left/Right turn it), the pick's name and
// description, BUY, and the credits held.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/hud.h"
#include "game/settings.h"
#include "render/renderer.h"

namespace nova {

class Combat;
class Shop;
class TextRenderer;

class PauseMenu {
public:
    bool open = false;
    void show();            // open at the PAUSE page
    void show_options();    // open at the Options page alone (from the main menu); Back closes it
    // String lookup (the game's string tables: "FPS_PAUSE" ...); the id itself if missing.
    std::function<std::string(const std::string&)> text = [](const std::string& id) { return id; };
    bool touch = false;   // the touch scheme: Left-Handed shows, the mouse aim option does not
    // The Military Shop on the pause page (without them, the objectives there instead).
    const Shop* shop = nullptr;
    Combat* combat = nullptr;
    void update(float dt);   // the shop's drum turning

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
        int slot = 0;                               // buttons: 0 in the page, 1/2 the bottom pair (Back, Resume)
    };
    std::vector<Row> rows(const Settings& s);
    Action back();
    Action activate(int row, int direction, Settings& s);

    std::vector<Page> stack_{Page::Pause};
    Page page() const { return stack_.back(); }
    Action confirm_action_ = Action::None;
    std::string confirm_text_;
    int selected_ = -1;                             // none until the keys pick one
    int dragging_ = -1;                             // a slider row held by the pointer
    struct Rect {
        glm::vec4 row{0}, control{0};               // x0, y0, x1, y1
    };
    std::vector<Rect> rects_;
    // The shop: the tab (0 ITEM, 1 UPGRADE, which it opens on), the drum's pick in it and
    // where the drum stands (easing to the pick, in items), the arrows' and side items' boxes.
    int shop_tab_ = 1, shop_pick_ = 0;
    float shop_drum_ = 0;
    glm::vec4 shop_arrows_[2]{}, shop_sides_[2]{};
    std::vector<int> shop_list() const;
    int shop_selected() const;   // the upgrade under the drum's pick; -1 if none
    void shop_turn(int d);
    // The shop in the pause page's panel: `origin` the original's 960x640 layout's corner on
    // screen, `px` its scale, `k` the art's.
    void draw_shop(ParticleList& out, Hud& hud, const Sprite& art, TextRenderer& title, TextRenderer& items,
                   const std::vector<Row>& list, const Settings& s, glm::vec2 origin, float px, float k, float W);
};

}  // namespace nova
