#include "game/controls.h"

#include <unordered_map>

#include <SDL3/SDL_platform.h>

namespace nova {

InputScheme default_input_scheme() {
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    return InputScheme::Touch;
#else
    return InputScheme::Desktop;
#endif
}

std::string desktop_text(const std::string& id) {
    // Mouse and keyboard: WASD, mouse look, left button fire, right button aim, Space
    // jump, Shift sprint, R reload, 1/2 or wheel weapons, G grenade, H switch grenade,
    // Q ability, Tab switch ability, E use, X detonate.
    static const std::unordered_map<std::string, std::string> text = {
        {"FPS_TUTORIAL_SHOOT", "Click [Left mouse] to shoot!"},
        {"FPS_TUTORIAL_REPEL", "Press [Q] to use the selected ability!"},
        {"FPS_TUTORIAL_CAMERA", "Move the mouse to look around!"},
        {"FPS_TUTORIAL_JUMP", "Press [Space] to jump!"},
        {"FPS_TUTORIAL_RUSH", "Hold [Shift] to sprint!"},
        {"FPS_TUTORIAL_GRENADE", "Press [G] to throw a grenade!"},
        {"FPS_TUTORIAL_WEAPON", "Press [1]/[2] or roll the mouse wheel to switch weapons!"},
        {"FPS_TUTORIAL_RELOAD", "Press [R] to reload!"},
        {"FPS_TUTORIAL_MOVE", "Use [W][A][S][D] to move!"},
        {"FPS_TUTORIAL_AIM", "Hold [Right mouse] to aim!"},
        {"FPS_TUTORIAL_POWERS", "Press [Tab] to switch abilities!"},
        {"FPS_TUTORIAL_PARALYZE", "Press [Q] to use the paralyze ability!"},
        {"FPS_TUTORIAL_SLOWTIME", "Press [Q] to use the slow-time ability!"},
        {"FPS_TUTORIAL_GRENADESWITCH", "Press [H] to switch grenades!"},
        {"FPS_TUTORIAL_MINE", "Press [G] to place a sticky grenade!"},
        {"FPS_TUTORIAL_DETONATE", "Press [X] to detonate mines!"},
        {"FPS_THERRIUS01_020", "Press [E] to activate the jetpack."},
        {"FPS_TOUCH", "Click to continue"},
        {"FPS_MINI_GAME_HINT_2", "Click the generator to create a beam of light."},
        {"FPS_MINI_GAME_HINT_5", "Click and drag a mirror or a splitter to place it on the circuit board."},
        {"FPS_MINI_GAME_HINT_6", "Click a mirror to rotate it."},
        {"FPS_MINIGAME_TUTORIAL1_04", "Click and drag a mirror on the circuit board."},
        {"FPS_MINIGAME_TUTORIAL1_05", "Click the mirror to rotate it."},
        {"FPS_MINIGAME_TUTORIAL1_06", "Now click the generator to activate it."},
        {"FPS_MP_QUICK_PLAY_PRIORITIES_INSTRUCTIONS",
         "Set priorities by clicking the game mode buttons in order of preference.\n"
         "Clicking one button twice will reset its priority so that it can be reused."},
        {"FPS_SIGNATURE_CONT", "CLICK AND DRAG TO CHANGE BACKGROUND"},
    };
    std::string key = id.rfind("STR_", 0) == 0 ? id.substr(4) : id;
    auto it = text.find(key);
    return it == text.end() ? std::string() : it->second;
}

std::string desktop_key(int glyph) {
    switch (glyph) {
        case 28: return "[Space]";
        case 29: return "[Left mouse]";
        case 30: return "[Q]";
        case 31: return "[Shift]";
        case 32: return "[Q]";
        case 33: return "[Right mouse]";
        case 34: return "[X]";
        case 35: return "[1]/[2]";
        case 36: return "[G]";
        case 37: return "[G]";
        case 38: return "[W][A][S][D]";
        default: return {};
    }
}

}  // namespace nova
