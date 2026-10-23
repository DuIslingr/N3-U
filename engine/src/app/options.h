// Command-line options of nova3, the N.O.V.A. 3 reimplementation:
//
//   nova3 [--data DIR] [--level NAME.lvc]                       the main menu, or straight into a level
//   nova3 ... --screenshot OUT.bmp [--eye X,Y,Z,YAW[,PITCH]] [--size WxH]   render one frame, no window
//   nova3 ... --walk-test SECONDS [--screenshot OUT.bmp]          simulate walking forward from the start
//   nova3 ... --focus OBJECT[#N] [--time T]                        start looking at the Nth placement of a type
//
//   nova3 ... --builtin-shaders                                  draw with the fallback shader, not the game's
//   nova3 ... --screenshot OUT.bmp --bench N                      also time N offscreen frames
//   nova3 ... --skybox ID                                          start with another of the level's skyboxes
//   nova3 ... --no-scripts                                         do not run the level scripts
//   nova3 ... --trace-scripts                                      log every script function that starts
//   nova3 ... --skip-intro                                         (testing) the start spawn point's script does not run
//   nova3 ... --spawn-at ID|X,Y,Z,YAW                              start at a level object, spawn point, waypoint or position
//   nova3 ... --exec "LUA"                                         run a script snippet once the level has started
//   nova3 ... --hide-models SUBSTR                                 (debugging) hide placements whose model matches
//   nova3 ... --no-zones                                           draw every zone (no portal culling)
//   nova3 ... --section N                                          load level section N (as LoadLevelSections)
//   nova3 ... --screenshot OUT.bmp --trace-zones                   print the zones, portals and what is visible
//   nova3 ... --screenshot OUT.bmp --run SECONDS                    run the level logic (standing still) first
//
// Controls: mouse to look, WASD to move, Shift to run, Space to jump, F to toggle flying
// (Space/Ctrl then move up/down), E to use, G to switch game/built-in shaders, Z to toggle
// zone culling, Esc to quit.
#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace nova {

struct Options {
    std::string data = "work/data";
    // Other versions' archives, mounted after the main data to fill what it lacks; by
    // default the iOS build's app folder beside it (<data>/../ios/ipa/Payload/nova3.app).
    std::vector<std::string> extra_data;
    std::string level = "levels/San_Francisco_1_FINAL.lvc";   // the campaign's first level
    std::vector<std::string> effects;   // --effect: started 6 m in front of the camera
    bool sound_check = false;           // decode every sound in the sound table and exit
    bool mute = false;
    bool touch = false;                 // the touch scheme (mobile prompts and controls) on desktop
    glm::vec3 probe{0};                 // --probe: list the draws whose bounds contain this point
    bool have_probe = false;
    std::string probe_model;            // --probe-model: list the draws of models whose name contains this
    std::string sound_render;           // headless: mix the run's sounds into this WAV file
    std::string screenshot;
    float walk_test = 0;
    bool log_player = false;    // print the player's state every second (interactive)
    float combat_test = 0;      // headless: aim at the nearest enemy and fire for this many seconds
    float grenade_test = -1;    // headless: at this time, throw a grenade at the nearest enemy
    float mine_test = -1;       // headless: at this time, throw a mine at the nearest enemy; detonate 2 s later
    float power_test = -1;      // headless: at this time, use Repel at the nearest enemy
    float touch_test = 0;       // headless, touch scheme: for the run's last seconds, hold the pad up and swipe to turn
    std::string sprite_sheet;   // debugging: draw frames of this .bsprite (numbered) over the view
    std::string menu_keys;      // headless: open the in-game menu and press these (u d l r e=enter b=back .=none)
    int credits = -1;           // the credits to hold (the Military Shop), instead of the saved ones
    int difficulty = -1;        // 0 Easy, 1 Normal, 2 Hard, instead of the saved one
    std::string save_file;      // the save file to write (also headless), instead of the preferences folder's
    std::string continue_file;  // headless: resume from this save file after the level loads
    int loading_screen = -1;
    float death_screen = -1;    // headless: draw the death screen this long after it opened
    float end_screen = -1;      // headless: draw the end-of-level screen this long after it opened    // headless: draw the level's loading screen, 0 loading, 1 loaded
    bool god = false;           // the player takes no damage
    bool list_actors = false;   // print combat targets after the level starts
    bool skip_intro = false;    // the start spawn point's script (the intro) does not run
    int audit = 0;              // walk this many random nav-mesh paths comparing zone culling on/off
    float time = 0;           // advance animations by this many seconds before a screenshot
    bool have_eye = false;
    glm::vec4 eye{0};        // x, y, z, yaw (degrees)
    float pitch = 0;
    std::string focus;        // object type name (substring), optionally #index
    bool builtin_shaders = false;
    int bench = 0;
    int skybox = -1;          // show this skybox id (as the level script's SetSkybox would)
    bool scripts = true, trace_scripts = false;
    float run = 0;            // headless: seconds of level logic before the screenshot
    std::string spawn_at;
    std::vector<std::string> exec;
    std::vector<std::string> hide_models;
    bool zones = true;
    int section = 1;
    bool trace_zones = false;
    int width = 1600, height = 900;
    bool size_given = false;
    bool level_given = false;   // --level: straight into it, no main menu
    std::string main_menu_keys; // headless: draw the main menu after pressing these (u d e=enter b=back .=none)    // --size: the window as asked, not the saved display settings
};

// Throws std::runtime_error on an unknown option or a missing value.
Options parse_options(int argc, char** argv);

}  // namespace nova
