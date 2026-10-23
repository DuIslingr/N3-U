// The main menu and chapter select, drawn from the original's own screen layouts in
// menu_elements_nova3.bsprite: frame 16 is the main menu (the Campaign and Multiplayer
// cards around the N.O.V.A. emblem, Options), frame 17 chapter select (three chapter cards
// down a slope, the chapter's location, climate and alliance, arrows to scroll, Back).
//
// A layout frame places its parts as other frames; each part carries invisible markers
// (modules 3 and 4: its touch rectangle's corners, 5: its text's centre, 17 and 18: its
// text's left and right). The layouts' own marker frames (3, 4, 5, 7) are not drawn.
//
// A chapter picked, the difficulty comes next (menus.lvc's page 31042: Easy, Normal, Hard,
// Hard only once unlocked), as the sheet's own buttons (695, pressed 696) in its dialog
// window (layout 14).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/hud.h"
#include "game/loading_screen.h"
#include "render/renderer.h"

namespace nova {

class TextRenderer;

class MainMenu {
public:
    bool open = false;
    const std::vector<CampaignLevel>* campaign = nullptr;
    int unlocked = 1;   // chapters the player has reached
    int difficulty = 1;           // 0 Easy, 1 Normal, 2 Hard: the last chosen, and the choice
    bool hard_unlocked = false;   // Menu_IsHardDiffUnlocked
    // String lookup (MAIN table ids such as "FPS_CAMPAIGN").
    std::function<std::string(const std::string&)> text = [](const std::string& id) { return id; };

    void show();        // at the main page
    enum class Action { None, Play, Options, Quit };
    int chosen = -1;    // with Play: the chapter (index into the campaign)
    enum class Key { Up, Down, Left, Right, Activate, Back };
    Action key(Key k);
    enum class Pointer { Move, Press, Release };
    Action pointer(glm::vec2 at, Pointer phase);

    void draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& large, TextRenderer& small, int w, int h);

private:
    enum class Page { Main, Chapters, Difficulty } page_ = Page::Main;
    int selected_ = 0, first_ = 0;   // chapters: the selection, the top card's chapter
    // What the pointer is over, and the touch rectangles as last drawn.
    enum class Part { None, Campaign, Multiplayer, Options, Back, Up, Down, Card0, Card1, Card2, Easy, Normal, Hard };
    Part hover_ = Part::None, pressed_ = Part::None;
    struct Hit {
        Part part;
        glm::vec4 rect;   // x0, y0, x1, y1 pixels
    };
    std::vector<Hit> hits_;
    Action act(Part p);
    void select(int chapter);
    int chapters() const { return campaign ? int(campaign->size()) : 0; }
};

}  // namespace nova
