// The game: a level and everything that plays it (scripts, combat, the first-person view,
// HUD and menus), run in a window or headless for screenshots and tests. Built for one
// level from the options; later levels load into the same objects.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/camera.h"
#include "app/options.h"
#include "audio/audio.h"
#include "core/vfs.h"
#include "game/combat.h"
#include "game/death_screen.h"
#include "game/end_level_screen.h"
#include "game/hud.h"
#include "game/lens_flare.h"
#include "game/level_logic.h"
#include "game/loading_screen.h"
#include "game/main_menu.h"
#include "game/pause_menu.h"
#include "game/player.h"
#include "game/save_game.h"
#include "game/scene.h"
#include "game/settings.h"
#include "game/shop.h"
#include "game/touch_controls.h"
#include "game/viewmodel.h"
#include "render/renderer.h"
#include "render/text.h"

struct SDL_Window;
union SDL_Event;

namespace nova {

class Game {
public:
    // Loads the options' level (or shows the main menu first). `window` is null headless.
    Game(const Options& options, const Vfs& vfs, const Gol& gol, SDL_Window* window, const std::string& shader_dir);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // The headless runs (screenshot, walk test, audit) or the window's loop; the exit code.
    int run();

private:
    static constexpr float kPhysicsStep = 1.0f / 120.0f;

    // Screens over or instead of the level.
    const CampaignLevel* campaign_level(const std::string& level) const;
    FrameParams draw_loading(int w, int h);
    void show_loading(const std::string& level, bool section = false);
    FrameParams draw_main_menu(int w, int h);
    // The level's frame as seen by `cam`: fog, lights, particles, post effects.
    FrameParams frame(const Camera& cam, int w, int h);
    // The HUD, prompts, subtitles and menus over a frame.
    void draw_text(FrameParams& f, int w, int h);
    static void fullscreen(Overlay& o, int w, int h, const glm::vec4& c);

    // Campaign progress and the save file.
    std::string next_level(const std::string& level) const;
    int chapter_of(const std::string& level) const;
    bool saving(const std::string& level) const;
    EndLevelScreen::Stats end_stats();
    EndLevelScreen::Rules end_rules();
    void load_level(const std::string& level);
    void resume(const SaveGame& g);

    // A fixed step of level logic (and what it sets off: sections, respawns, level ends).
    void step_logic(float dt);
    void movement_states(bool aim);
    void spawn_at_option();
    // A combat step: the player's bullets leave from the view model's muzzle, which flashes
    // for each shot fired.
    void fire_step(float dt, const CombatInput& in, const glm::vec3& eye, const glm::vec3& aim, bool aiming);
    void arms_distort();   // the level logic's Test_distort_* requests, to the view model
    void apply_sound();
    void update_frost(float dt);

    // Headless (game_headless.cpp).
    int run_audit();
    void run_walk_test();
    int run_screenshot();
    FrameParams shot_frame(Camera base);

    // The window (game_window.cpp).
    void run_window();
    void handle_event(const SDL_Event& e);
    glm::vec2 to_pixels(float x, float y, bool normalized) const;
    // The use button (controls.bsprite 35, CLevelInterfaceNormal): its box on a w x h screen
    // (x0, y0, x1, y1), centred across, its top 100 (640-high) pixels below the middle.
    glm::vec4 use_button(int w, int h);
    float use_pressed_ = 0;   // seconds the button shows pressed (36)
    unsigned vm_shots_ = 0;   // the player's shots the view model has seen
    void apply_display();
    void set_menu(bool on);
    void menu_action(PauseMenu::Action a);
    void end_action(EndLevelScreen::Action a);
    void death_action(DeathScreen::Action a);
    void main_action(MainMenu::Action a);
    void title_music(const char* state);

    Options opt;
    const Vfs& vfs;
    const Gol& gol;
    SDL_Window* window;
    const bool headless;
    Renderer renderer;
    Scene scene;
    // The loading screen (GS_Loading): the level's picture and story while it loads.
    const std::vector<CampaignLevel> campaign;
    StringTable main_strings;
    LoadingScreen loading;
    Hud loading_hud;
    TextRenderer loading_title, loading_text;
    Overlay loading_overlay;
    ParticleList loading_sprites;
    int loading_fonts_for;
    MainMenu main_menu;
    float clock;
    const PostParams* post_params = nullptr;   // the level logic's, once it exists
    const float* zfar = nullptr;               // the level logic's far plane (SetZFar), once it exists
    ParticleList particle_list;
    Player player;
    LevelLogic logic;
    std::unique_ptr<Audio> audio;
    std::vector<float> rendered;               // --sound-render
    Combat combat;
    Overlay overlay;
    ViewModel viewmodel;
    // Subtitles: the latest script message, bottom centre.
    TextRenderer subtitles;
    ParticleList hud_sprites;
    Hud hud;
    TouchControls touch;                       // the Touch scheme's on-screen controls
    const std::string settings_path;
    Settings settings;
    PauseMenu menu;
    Shop shop;                                 // the Military Shop on the pause page
    const std::string save_path;
    LensFlares flares;
    DeathScreen death;
    EndLevelScreen end_screen;
    TextRenderer menu_title, objective_text, dialogue;
    uint32_t frost_serial = 0;
    int fonts_for;                             // the screen height the fonts are sized for
    float flare_clock = -1;
    bool end_screen_done = false;              // dismissed with Continue: the level's end goes on
    float ended_for = 0;                       // seconds since the level ended
    Camera cam;
    uint32_t view_spawns = 0, view_jumps = 0, distort_serial = 0;

    // Playing in the window.
    bool flying = false;
    float accumulator = 0;
    CombatInput combat_in;
    bool fire_held = false, aim_held = false, aim_toggled = false;
    bool touch_mode = false;
    bool running = true;
    std::chrono::steady_clock::time_point last;
};

}  // namespace nova
