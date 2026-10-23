// The campaign's levels and the loading screen shown while one loads (GS_Loading).
//
// The levels come from gol type "GameLevels" (CComponentLevels, 92): per level its index
// into levels/locations.bin's level list, the chapter-select picture, the loading picture
// (LOADING_<name>.bsprite), and the ids of its title, loading text, location, climate and
// alliance. Its order is the story order.
//
// The loading screen: the level's picture over the whole screen, its title at the top and
// its text at the bottom, "LOADING" with loading.bsprite's turning rings in the lower right
// corner; once loaded, "touch the screen to continue" until the player does.
#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"
#include "game/hud.h"
#include "render/renderer.h"

namespace nova {

class StringTable;
class TextRenderer;
class Vfs;

struct CampaignLevel {
    std::string file;                 // "San_Francisco_1_FINAL" (levels/<file>.lvc)
    std::string picture;              // "jungle4": LOADING_jungle4.bsprite
    std::string title, text;          // string ids: "FPS_SAN_FRANCISCO_1", "..._LOADING"
    std::string location, climate, alliance;
};
// In story order; empty if the game's data lacks them.
std::vector<CampaignLevel> campaign_levels(const Vfs& vfs, const Gol& gol);

class LoadingScreen {
public:
    bool active = false;
    bool loaded = false;      // set by the caller once the level is in
    bool touch = false;       // the touch scheme's wording

    // Show the screen for `level` (null: no picture or text, just the rings).
    void begin(const CampaignLevel* level);
    void update(float dt);
    // A key, click or tap: true when it dismisses the (loaded) screen.
    bool press();
    void draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& title, TextRenderer& text, const StringTable& strings,
              int w, int h);

private:
    CampaignLevel level_;
    bool has_level_ = false;
    float time_ = 0, loaded_for_ = 0;
    // loading.bsprite frames 1-6 turn at their own speeds, each picked anew every 1-3 s
    // (Application::DrawLoadingAnim).
    struct Ring {
        float angle = 0, speed = 0, left = 0;   // degrees, degrees per second, seconds
    };
    Ring rings_[6];
};

}  // namespace nova
