// nova3: level viewer for the N.O.V.A. 3 reimplementation.
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
// The data directory is the game's own files folder (the .gla archives).
#include <chrono>
#include <cstring>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glm/gtc/matrix_transform.hpp>

#include "audio/audio.h"
#include "core/vfs.h"
#include "game/combat.h"
#include "game/hud.h"
#include "game/loading_screen.h"
#include "game/main_menu.h"
#include "game/death_screen.h"
#include "game/end_level_screen.h"
#include "game/lens_flare.h"
#include "game/viewmodel.h"
#include "game/navmesh.h"
#include "game/player.h"
#include "game/level_logic.h"
#include "game/scene.h"
#include "game/pause_menu.h"
#include "game/settings.h"
#include "game/save_game.h"
#include "game/shop.h"
#include "game/touch_controls.h"
#include "render/renderer.h"
#include "render/text.h"

using namespace nova;

namespace {

struct Options {
    std::string data = "work/data";
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

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--data") o.data = next();
        else if (a == "--level") o.level = next(), o.level_given = true;
        else if (a == "--main-menu") o.main_menu_keys = next();
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--walk-test") o.walk_test = std::stof(next());
        else if (a == "--time") o.time = std::stof(next());
        else if (a == "--focus") o.focus = next();
        else if (a == "--builtin-shaders") o.builtin_shaders = true;
        else if (a == "--bench") o.bench = std::stoi(next());
        else if (a == "--skybox") o.skybox = std::stoi(next());
        else if (a == "--no-scripts") o.scripts = false;
        else if (a == "--trace-scripts") o.trace_scripts = true;
        else if (a == "--skip-intro") o.skip_intro = true;
        else if (a == "--run") o.run = std::stof(next());
        else if (a == "--spawn-at") o.spawn_at = next();
        else if (a == "--exec") o.exec.push_back(next());
        else if (a == "--effect") o.effects.push_back(next());
        else if (a == "--sound-check") o.sound_check = true;
        else if (a == "--touch") o.touch = true;
        else if (a == "--probe-model") o.probe_model = next();
        else if (a == "--probe") {
            float v[3];
            if (std::sscanf(next().c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) throw std::runtime_error("--probe needs X,Y,Z");
            o.probe = {v[0], v[1], v[2]}, o.have_probe = true;
        }
        else if (a == "--mute") o.mute = true;
        else if (a == "--sound-render") o.sound_render = next();
        else if (a == "--hide-models") o.hide_models.push_back(next());
        else if (a == "--no-zones") o.zones = false;
        else if (a == "--section") o.section = std::stoi(next());
        else if (a == "--trace-zones") o.trace_zones = true;
        else if (a == "--log-player") o.log_player = true;
        else if (a == "--combat-test") o.combat_test = std::stof(next());
        else if (a == "--grenade-test") o.grenade_test = std::stof(next());
        else if (a == "--mine-test") o.mine_test = std::stof(next());
        else if (a == "--power-test") o.power_test = std::stof(next());
        else if (a == "--sprite-sheet") o.sprite_sheet = next();
        else if (a == "--menu") o.menu_keys = next();
        else if (a == "--credits") o.credits = std::stoi(next());
        else if (a == "--difficulty") o.difficulty = std::stoi(next());
        else if (a == "--save") o.save_file = next();
        else if (a == "--continue") o.continue_file = next();
        else if (a == "--loading-screen") o.loading_screen = std::stoi(next());
        else if (a == "--death-screen") o.death_screen = std::stof(next());
        else if (a == "--end-screen") o.end_screen = std::stof(next());
        else if (a == "--touch-test") o.touch_test = std::stof(next()), o.touch = true;
        else if (a == "--god") o.god = true;
        else if (a == "--list-actors") o.list_actors = true;
        else if (a == "--audit") o.audit = std::stoi(next());
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &o.width, &o.height), o.size_given = true;
        else if (a == "--eye") {
            float v[5] = {0, 0, 0, 0, 0};
            const int n = std::sscanf(next().c_str(), "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
            if (n < 4) throw std::runtime_error("--eye needs X,Y,Z,YAW[,PITCH]");
            o.eye = {v[0], v[1], v[2], v[3]};
            o.pitch = v[4];
            o.have_eye = true;
        } else {
            throw std::runtime_error("unknown option " + a);
        }
    }
    return o;
}

// A viewpoint looking at the Nth placement whose object type contains `spec` (NAME[#N]),
// from whichever side has the most open space. Returns false if there is no such object.
bool focus_camera(const Scene& scene, const Gol& gol, const std::string& spec, glm::vec3& eye, float& yaw, float& pitch) {
    const size_t hash = spec.find('#');
    const std::string name = to_lower(spec.substr(0, hash));
    int index = hash == std::string::npos ? 0 : std::stoi(spec.substr(hash + 1));
    for (const Placement& p : model_placements(scene.level(), gol)) {
        if (to_lower(p.object).find(name) == std::string::npos || index-- > 0) continue;
        const glm::vec3 target = p.transform.position + glm::vec3(0, 0, 1.0f);
        constexpr float kWant = 4.0f;
        float best = -1;
        for (int i = 0; i < 16; ++i) {
            // Start from the object's facing (its local +Y) and go round.
            const float a = glm::radians(p.transform.rotation.z + i * 22.5f);
            const glm::vec3 dir(-std::sin(a), std::cos(a), 0.3f);
            float t = 1;
            scene.collision().raycast(target, target + dir * kWant, t);
            const float clear = t * kWant - 0.3f;
            if (clear > best + 0.25f) {
                best = clear;
                eye = target + dir * std::max(clear, 0.5f);
            }
        }
        const glm::vec3 d = target - eye;
        yaw = glm::degrees(std::atan2(-d.x, d.y));
        pitch = glm::degrees(std::asin(d.z / glm::length(d)));
        std::printf("focus: %s (%s, anim %s:%s) at (%.2f %.2f %.2f), eye %.2f,%.2f,%.2f,%.1f,%.1f\n", p.object.c_str(), p.model.c_str(), p.anim_file.c_str(), p.anim_clip.c_str(),
                    p.transform.position.x, p.transform.position.y, p.transform.position.z, eye.x, eye.y, eye.z, yaw,
                    pitch);
        return true;
    }
    return false;
}

// The game's field of view (CLevel::SetTargetFov: the camera gets fov * height / width
// degrees vertically; 70 by default). Aim mode narrows it.
constexpr float kFov = 70, kAimZoom = 0.65f;

struct Camera {
    glm::vec3 position{0};
    float yaw = 0, pitch = 0;   // degrees; yaw 0 looks along +Y, positive yaw turns left (game convention)
    float fov = kFov;
    bool game_fov = true;       // fov in the game's units (else vertical degrees: cinematic cameras)
    float zfar = 8000;
    // A camera that looks along a given direction (cinematics) instead of yaw/pitch.
    bool directed = false;
    glm::vec3 direction{0, 1, 0}, up{0, 0, 1};

    glm::vec3 forward() const {
        if (directed) return direction;
        const float y = glm::radians(yaw), p = glm::radians(pitch);
        return {-std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p)};
    }
    FrameParams frame(int w, int h) const {
        FrameParams f;
        f.view = glm::lookAt(position, position + forward(), directed ? up : glm::vec3(0, 0, 1));
        // Reversed depth (near -> 1, far -> 0) spreads the float depth buffer's precision
        // evenly; near and far are swapped on purpose.
        const float vertical = game_fov ? fov * float(h) / float(std::max(w, 1)) : fov;
        f.proj = glm::perspectiveRH_ZO(glm::radians(vertical), float(w) / float(std::max(h, 1)), zfar, 0.05f);
        f.camera = position;
        return f;
    }
};

// A screen-filling quad in the overlay (fades).
void fullscreen(Overlay& o, int w, int h, const glm::vec4& c) {
    const uint32_t rgba = pack_rgba(c);
    const ColorVertex v[4] = {{{0, 0, 0}, rgba}, {{float(w), 0, 0}, rgba}, {{float(w), float(h), 0}, rgba}, {{0, float(h), 0}, rgba}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

// 16-bit stereo WAV from interleaved float samples.
bool save_wav(const std::vector<float>& samples, int rate, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t data = uint32_t(samples.size() * 2);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f), u32(36 + data), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(2), u32(uint32_t(rate)), u32(uint32_t(rate) * 4), u16(4), u16(16);
    std::fwrite("data", 1, 4, f), u32(data);
    for (float x : samples) u16(uint16_t(int16_t(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f))));
    return std::fclose(f) == 0;
}

bool save_bmp(const Image& img, const std::string& path) {
    // Opaque, as the window shows it: the framebuffer's alpha (the HUD writes its own) is not
    // part of the picture.
    SDL_Surface* s = SDL_CreateSurfaceFrom(img.width, img.height, SDL_PIXELFORMAT_XBGR8888,
                                           const_cast<uint8_t*>(img.rgba.data()), img.width * 4);
    const bool ok = s && SDL_SaveBMP(s, path.c_str());
    SDL_DestroySurface(s);
    return ok;
}

// The level after `level` in the campaign's story order (gol "GameLevels"); empty after the last.
std::string next_campaign_level(const std::vector<CampaignLevel>& campaign, const std::string& level) {
    const size_t slash = level.find_last_of('/');
    std::string base = level.substr(slash == std::string::npos ? 0 : slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".lvc") == 0) base.resize(base.size() - 4);
    for (size_t i = 0; i + 1 < campaign.size(); ++i)
        if (base == campaign[i].file) return level.substr(0, slash == std::string::npos ? 0 : slash + 1) + campaign[i + 1].file + ".lvc";
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        opt = parse(argc, argv);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "%s\n", ex.what());
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const std::string shader_dir = std::string(SDL_GetBasePath()) + "shaders";
    int rc = 0;
    try {
        Vfs vfs;
        if (vfs.mount_directory(opt.data) == 0) throw std::runtime_error("no .gla archives in " + opt.data);
        const Gol gol = load_gol(vfs.read("gol.bin"));
        if (opt.sound_check) {
            Audio audio(vfs, false);
            const auto failed = audio.check_all();
            size_t n = 0;
            for (const SoundDef& sd : audio.table().sounds) n += sd.uid >= 0;
            std::printf("sound table: %zu sounds, %zu events; %zu failed to decode\n", n, audio.table().events.size(), failed.size());
            for (const std::string& f : failed) std::printf("  %s\n", f.c_str());
            return failed.empty() ? 0 : 1;
        }

        const bool headless = !opt.screenshot.empty();
        SDL_Window* window = nullptr;
        if (!headless) {
            window = SDL_CreateWindow(("N.O.V.A. 3 - " + opt.level).c_str(), opt.width, opt.height,
                                      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (!window) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        }
        {
            Renderer renderer(window, shader_dir);
            Scene scene(vfs, renderer, gol);
            // The loading screen (GS_Loading): the level's picture and story while it loads.
            const std::vector<CampaignLevel> campaign = campaign_levels(vfs, gol);
            auto campaign_level = [&campaign](const std::string& level) -> const CampaignLevel* {
                for (const CampaignLevel& c : campaign)
                    if (level.find("/" + c.file + ".lvc") != std::string::npos || level == c.file + ".lvc") return &c;
                return nullptr;
            };
            StringTable main_strings;
            main_strings.load(vfs, "MAIN");
            LoadingScreen loading;
            loading.touch = opt.touch;
            Hud loading_hud(scene);
            TextRenderer loading_title(vfs, renderer, "Days.ttf", std::max(14.0f, float(opt.height) * 0.03f));
            TextRenderer loading_text(vfs, renderer, "Days.ttf", std::max(12.0f, float(opt.height) * 0.022f));
            Overlay loading_overlay;
            ParticleList loading_sprites;
            int loading_fonts_for = opt.height;
            auto draw_loading = [&](int w, int h) {
                loading_hud.set_screen(w, h);
                if (h != loading_fonts_for) {
                    loading_fonts_for = h;
                    loading_title.set_size(std::max(14.0f, float(h) * 0.03f));
                    loading_text.set_size(std::max(12.0f, float(h) * 0.022f));
                }
                loading_overlay.clear();
                loading_sprites.vertices.clear();
                loading_sprites.batches.clear();
                loading.draw(loading_overlay, loading_sprites, loading_hud, loading_title, loading_text, main_strings, w, h);
                FrameParams f;
                f.overlay = &loading_overlay;
                f.sprites = &loading_sprites;
                return f;
            };
            // A frame of it before a level is read (the window shows nothing new until then).
            auto show_loading = [&](const std::string& level, bool section = false) {
                loading.begin(section ? nullptr : campaign_level(level));
                if (!window) return;
                SDL_PumpEvents();
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                renderer.render_to_window({}, draw_loading(w, h));
            };
            // The main menu first, unless a level was asked for.
            MainMenu main_menu;
            main_menu.campaign = &campaign;
            main_menu.text = [&main_strings](const std::string& id) {
                const std::string t = main_strings.find(id);
                return t.empty() ? id : t;
            };
            auto draw_main_menu = [&](int w, int h) {
                loading_hud.set_screen(w, h);
                if (h != loading_fonts_for) {
                    loading_fonts_for = h;
                    loading_title.set_size(std::max(14.0f, float(h) * 0.03f));
                    loading_text.set_size(std::max(12.0f, float(h) * 0.022f));
                }
                loading_overlay.clear();
                loading_sprites.vertices.clear();
                loading_sprites.batches.clear();
                main_menu.draw(loading_overlay, loading_sprites, loading_hud, loading_title, loading_text, w, h);
                FrameParams f;
                f.overlay = &loading_overlay;
                f.sprites = &loading_sprites;
                return f;
            };
            if (!headless && !opt.level_given) {
                main_menu.show();
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                SDL_PumpEvents();
                renderer.render_to_window({}, draw_main_menu(w, h));
            } else if (!headless) {
                show_loading(opt.level);
            }
            scene.load_level(opt.level, 1u << (std::max(opt.section, 1) - 1));
            loading.loaded = true;
            if (opt.builtin_shaders) scene.set_game_shaders(false);
            scene.zones().enabled = opt.zones;
            if (opt.skybox >= 0 && !scene.set_skybox(opt.skybox)) throw std::runtime_error("no skybox with that id");
            const SceneStats& st = scene.stats();
            const MaterialStats& ms = scene.material_stats();
            std::printf("materials: %zu with game shaders, %zu fallback; %zu programs (%zu failed), %zu effects missing; "
                        "%zu shader stages compiled, %zu from cache\n",
                        ms.materials, ms.fallback, ms.programs, ms.failed_programs, ms.missing_effects,
                        scene.shader_compiler().compiled, scene.shader_compiler().cache_hits);
            float clock = opt.time;
            const PostParams* post_params = nullptr;   // the level logic's, once it exists
            const float* zfar = nullptr;               // the level logic's far plane (SetZFar), once it exists
            ParticleList particle_list;
            auto frame = [&](const Camera& cam_in, int w, int h) {
                Camera cam = cam_in;
                if (zfar) cam.zfar = *zfar;
                FrameParams f = cam.frame(w, h);
                scene.build_particles(f.camera, particle_list);
                f.particles = &particle_list;
                f.fog_color = scene.fog_color;
                f.fog_start = scene.fog_start;
                f.fog_end = scene.fog_end;
                f.fog_density = scene.fog_density;
                f.time = clock;
                f.lights = scene.lights();
                f.post = post_params;
                return f;
            };
            std::printf("%s: %zu placements, %zu models (%zu missing), %zu textures (%zu missing), %zu draws, "
                        "%zu collision objects (%zu triangles, %zu missing), %zu animated (%zu animations missing), "
                        "%zu draws hidden by LOD, %zu irradiance volumes, %zu lights, %zu skyboxes, loaded in %.2fs\n",
                        opt.level.c_str(), st.placements, st.models, st.missing_models, st.textures,
                        st.missing_textures, st.draws, st.collision_objects, st.collision_triangles,
                        st.missing_collision, st.animated, st.missing_animations, st.lod_hidden, st.irradiance_volumes, st.lights, st.skyboxes, st.load_seconds);
            Player player;
            player.spawn(scene.start_position, scene.start_yaw);
            LevelLogic logic(scene, player, gol);
            if (opt.touch) logic.input = InputScheme::Touch;
            std::unique_ptr<Audio> audio;
            if ((!headless && !opt.mute) || !opt.sound_render.empty()) {
                audio = std::make_unique<Audio>(vfs, opt.sound_render.empty());
                if (audio->enabled()) logic.audio = audio.get();
                audio->trace = !opt.sound_render.empty();
                scene.particles().on_sound = [&logic](const std::vector<std::string>& names, const glm::vec3& at) {
                    if (!logic.audio) return;
                    Audio::Params p;
                    p.position = &at;
                    logic.audio->play_one_of(names, p);
                };
            }
            std::vector<float> rendered;   // --sound-render
            if (opt.scripts) post_params = &logic.post, zfar = &logic.zfar;
            logic.auto_qte = headless;   // nobody to press the QTE buttons
            logic.trace = opt.trace_scripts;
            logic.skip_intro = opt.skip_intro;
            logic.log = [&opt](const std::string& line) {
                if (line.rfind("print: ", 0) == 0 && !opt.trace_scripts) return;   // scripts print a lot
                std::printf("[script] %s\n", line.c_str());
            };
            std::printf("zones: %zu zones, %zu portals\n", scene.zones().zones().size(), scene.zones().portals().size());
            Combat combat(scene, player, logic, gol);
            combat.log = [](const std::string& line) { std::printf("[combat] %s\n", line.c_str()); };
            combat.player_invulnerable = opt.god;
            combat.death_screen = !headless;
            logic.combat = &combat;
            Overlay overlay;
            ViewModel viewmodel(scene, gol);
            // Subtitles: the latest script message, bottom centre.
            TextRenderer subtitles(vfs, renderer, "Days.ttf", std::max(16.0f, float(opt.height) * 0.032f));
            ParticleList hud_sprites;
            Hud hud(scene);
            TouchControls touch(hud);   // the Touch scheme's on-screen controls
            const std::string settings_path = headless ? std::string() : Settings::default_path();
            Settings settings = Settings::load(settings_path);
            if (opt.credits >= 0) settings.credits = opt.credits;
            if (opt.difficulty >= 0) settings.difficulty = opt.difficulty;
            PauseMenu menu;
            Shop shop;   // the Military Shop on the pause page
            shop.load(gol);
            combat.set_difficulty(settings.difficulty), shop.difficulty = settings.difficulty;
            // The save file (Application::Save): written while playing the furthest chapter
            // reached (replaying an earlier one saves nothing, as the original's NoSave).
            const std::string save_path = !opt.save_file.empty() ? opt.save_file : headless ? std::string() : SaveGame::default_path();
            auto chapter_of = [&](const std::string& level) {
                for (size_t c = 0; c < campaign.size(); ++c)
                    if (level.find("/" + campaign[c].file + ".lvc") != std::string::npos) return int(c);
                return -1;
            };
            auto saving = [&](const std::string& level) {
                const int c = chapter_of(level);
                return !save_path.empty() && c >= 0 && (c + 1 >= settings.chapters || !opt.save_file.empty());
            };
            logic.on_checkpoint = [&] {
                if (!saving(opt.level)) return;
                SaveGame g;
                g.level = opt.level, g.checkpoint = logic.checkpoint_data();
                if (!g.write(save_path)) std::fprintf(stderr, "could not write %s\n", save_path.c_str());
            };
            if (!shop.items().empty()) menu.shop = &shop, menu.combat = &combat;
            LensFlares flares(scene, logic);
            DeathScreen death;
            EndLevelScreen end_screen;
            end_screen.text = [&main_strings](const std::string& id) {
                const std::string t = main_strings.find(id);
                return t.empty() ? id : t;
            };
            auto end_stats = [&] {
                const Combat::Stats& cs = combat.stats();
                EndLevelScreen::Stats st;
                st.headshots = int(cs.headshot_kills), st.blast_kills = int(cs.repel_kills), st.paralyze_kills = int(cs.paralyze_kills);
                st.slow_kills = int(cs.slow_kills), st.kills = int(cs.kills), st.retries = int(cs.deaths);
                st.criticals = int(cs.criticals);
                st.accuracy = cs.shots ? std::min(float(cs.hits) / float(cs.shots), 1.0f) : 1.0f;
                st.play_time = cs.play_time;
                return st;
            };
            auto end_rules = [&] {
                return EndLevelScreen::read_rules(gol, logic.play_time_limit(0), logic.play_time_limit(1));
            };
            death.text = [&main_strings](const std::string& id) {
                const std::string t = main_strings.find(id);
                return t.empty() ? id : t;
            };
            menu.text = [&logic](const std::string& id) {
                const std::string t = logic.strings.find(id);
                return t.empty() ? id : t;
            };
            menu.touch = logic.input == InputScheme::Touch;
            // Settings that act on the game: sound levels (the menu's Sound page).
            auto apply_sound = [&] {
                if (!logic.audio) return;
                logic.audio->master = settings.sound ? 0.8f : 0.0f;
                logic.audio->sfx = logic.audio->voices = settings.sfx_volume;
                logic.audio->music = 0.6f * settings.music_volume;
            };
            TextRenderer menu_title(vfs, renderer, "Days.ttf", std::max(20.0f, float(opt.height) * 0.06f));
            TextRenderer objective_text(vfs, renderer, "Days.ttf", std::max(13.0f, float(opt.height) * 0.026f));
            TextRenderer dialogue(vfs, renderer, "Days.ttf", std::max(13.0f, float(opt.height) * 0.024f));
            // HUD_StartFrost / HUD_StopFrost: the frost spreads or melts over time.
            uint32_t frost_serial = 0;
            auto update_frost = [&](float dt) {
                if (logic.frost.serial != frost_serial) {
                    frost_serial = logic.frost.serial;
                    if (logic.frost.on > 0) hud.start_frost(logic.frost.ms);
                    else hud.stop_frost(logic.frost.ms);
                }
                hud.frost_max = logic.frost.max;
                hud.update_frost(dt);
            };
            int fonts_for = opt.height;   // the screen height the fonts are sized for
            auto draw_text = [&](FrameParams& f, int w, int h) {
                hud.set_screen(w, h);   // which sprite sheets (1x or 2x) suit this screen
                if (h != fonts_for) {   // resized: the text keeps its share of the screen
                    fonts_for = h;
                    subtitles.set_size(std::max(16.0f, float(h) * 0.032f));
                    menu_title.set_size(std::max(20.0f, float(h) * 0.06f));
                    objective_text.set_size(std::max(13.0f, float(h) * 0.026f));
                    dialogue.set_size(std::max(13.0f, float(h) * 0.024f));
                }
                hud_sprites.vertices.clear();
                hud_sprites.batches.clear();
                if (opt.scripts) {   // lens flares, under the HUD
                    static float flare_clock = -1;
                    const float flare_dt = flare_clock < 0 ? 10.0f : clock - flare_clock;   // the first frame: settled
                    flare_clock = clock;
                    flares.frame(flare_dt, f.camera, f.proj * f.view, w, h, hud_sprites);
                }
                if (opt.scripts) {
                    Hud::State hs;
                    hs.visible = !combat.player_dead && logic.hud_enabled && (!logic.player_locked() || logic.interface_in_cinematic) && !menu.open;
                    hs.clip = combat.ammo_in_clip(), hs.reserve = combat.ammo_reserve();
                    hs.grenades = (combat.grenades_enabled || combat.mines_picked()) && logic.controls.items;
                    hs.grenade_count = combat.mines_picked() ? combat.mines : combat.grenades;
                    hs.shield = combat.player_health / std::max(combat.player_max_health, 1.0f);
                    hs.core = combat.core;
                    if (combat.core >= 0) hs.core_left = (hs.shield - 1) / std::max(combat.core_percent / 100, 1e-3f);
                    hs.weapon_sprite = combat.weapon_sprite();
                    hs.clip_size = combat.clip_size();
                    hud.frame(f, w, h, hs, hud_sprites);
                    const std::string& qte = logic.qte_kind();
                    if (logic.input == InputScheme::Touch && (hs.visible || !qte.empty())) {
                        const LevelLogic::Controls& lc = logic.controls;
                        touch.shown = {lc.move, lc.move && lc.rush, lc.shoot && combat.shoot_enabled, lc.aim, lc.jump, lc.powers};
                        if (!hs.visible) touch.shown = {false, false, qte == "fire", false, false, qte == "repel"};   // a QTE in a cinematic
                        touch.layout(w, h);
                        touch.power_ready = combat.power_ready();
                        touch.power = combat.power();
                        touch.draw(hud_sprites);
                    } else {
                        touch.release_all();
                    }
                }
                if (opt.scripts && !logic.qte_kind().empty()) {
                    // CQTEFire: tutorial.bsprite's animation 1 over the button to press.
                    if (const Sprite* ts = hud.sprite("tutorial.bsprite"); ts) {
                        const float scale = hud.sprite_scale("tutorial.bsprite");
                        const bool touch_qte = logic.input == InputScheme::Touch;
                        const glm::vec2 at = touch_qte ? touch.fire_anchor() : glm::vec2(float(w) * 0.5f, float(h) * 0.72f);
                        hud.draw_anim(hud_sprites, *ts, 1, at, scale, logic.qte_time(), glm::vec4(1));
                        if (!touch_qte) {
                            const std::string key = logic.qte_kind() == "fire" ? "[Left mouse]" : logic.qte_kind() == "repel" ? "[Q]" : "[W][A][S][D]";
                            objective_text.draw(hud_sprites, key, at + glm::vec2(0, 70 * scale), w, glm::vec4(1));
                        }
                    }
                }
                if (opt.scripts && !logic.player_locked() && logic.hud_enabled)
                    for (const LevelLogic::ObjectiveMarker& m : logic.objective_markers) {
                        const LevelLogic::Object* o = logic.object(m.id);
                        if (!o || !o->enabled) continue;
                        glm::vec3 at = o->position;
                        if (!m.node.empty()) scene.node_position(o->index, m.node, at);
                        hud.draw_marker(hud_sprites, objective_text, f.proj * f.view, at, glm::distance(f.camera, at), w, h, settings.metric);
                    }
                if (opt.scripts && logic.path_marker.on && !logic.player_locked() && logic.hud_enabled)
                    hud.draw_marker(hud_sprites, objective_text, f.proj * f.view, logic.path_marker.position,
                                    glm::distance(f.camera, logic.path_marker.position), w, h, settings.metric);
                if (opt.scripts && !logic.player_locked() && logic.hud_enabled)
                    for (const glm::vec3& g : combat.grenade_threats()) hud.draw_grenade_warning(hud_sprites, f.view, g, w, h);
                if (opt.scripts && logic.timer >= 0 && !logic.player_locked() && logic.hud_enabled)
                    hud.draw_timer(hud_sprites, objective_text, logic.timer, w, h);
                if (const Combat::PickupInfo& pi = combat.pickup_info(); pi.time > 0 && opt.scripts && !logic.player_locked() && logic.hud_enabled)
                    hud.draw_pickup(hud_sprites, objective_text, pi.sprite, pi.amount, w, h);
                if (opt.scripts && logic.objectives.alpha > 0 && !logic.player_locked() && !menu.open) {
                    std::vector<Hud::ObjectiveLine> lines;
                    if (!logic.objectives.primary.empty()) lines.push_back({logic.objectives.primary, true, logic.objectives.primary_done});
                    for (const auto& s : logic.objectives.secondary) lines.push_back({s.text, false, s.done});
                    hud.draw_objectives(hud_sprites, objective_text, lines, logic.objectives.alpha, w, h);
                }
                if (opt.scripts && !logic.messages().empty() && !menu.open) {
                    const LevelLogic::Message& m = logic.messages().back();
                    const float a = std::clamp(m.seconds_left / 0.4f, 0.0f, 1.0f);
                    if (m.text.find('^') != std::string::npos)   // touch prompts: button glyphs inline
                        hud.draw_prompt(hud_sprites, subtitles, m.text, {float(w) * 0.5f, float(h) * 0.8f}, {1, 1, 1, a});
                    else if (!m.speaker.empty()) {   // dialogue: "Name: line", left-aligned low on the screen
                        const std::string name = m.speaker + ": ";
                        const float x = float(w) * 0.082f, nw = dialogue.measure(name, w).x;
                        const int wrap = int(float(w) * 0.84f - nw);
                        const float y = std::min(float(h) * 0.8f, float(h) * 0.97f - dialogue.measure(m.text, wrap).y);
                        dialogue.draw(hud_sprites, name, {x, y}, w, {1.0f, 0.62f, 0.15f, a}, {0.0f, 0.0f});
                        dialogue.draw(hud_sprites, m.text, {x + nw, y}, wrap, {1, 1, 1, a}, {0.0f, 0.0f});
                    } else {   // from 80% down, raised as needed to keep every line on screen
                        const int wrap = int(float(w) * 0.7f);
                        const float y = std::min(float(h) * 0.8f, float(h) * 0.97f - subtitles.measure(m.text, wrap).y);
                        subtitles.draw(hud_sprites, m.text, {float(w) * 0.5f, y}, wrap, {1, 1, 1, a});
                    }
                }
                if (death.open) death.draw(overlay, hud_sprites, hud, subtitles, w, h);
                if (end_screen.open) end_screen.draw(overlay, hud_sprites, hud, menu_title, subtitles, w, h);
                if (menu.open) {
                    std::vector<Hud::ObjectiveLine> lines;
                    if (!logic.objectives.primary.empty()) lines.push_back({logic.objectives.primary, true, logic.objectives.primary_done});
                    for (const auto& o : logic.objectives.secondary) lines.push_back({o.text, false, o.done});
                    menu.draw(overlay, hud_sprites, hud, subtitles, objective_text, lines, settings, w, h);
                }
                if (!opt.sprite_sheet.empty()) {   // NAME[:first:count]
                    std::string name = opt.sprite_sheet;
                    int first = 0, count = -1;
                    if (const size_t c = name.find(':'); c != std::string::npos) {
                        std::sscanf(name.c_str() + c + 1, "%d:%d", &first, &count);
                        name.resize(c);
                    }
                    if (const Sprite* sp = hud.sprite(name)) {
                        fullscreen(overlay, w, h, glm::vec4(0.15f, 0.15f, 0.15f, 1));
                        const int total = int(sp->frames.size());
                        if (count < 0) count = total - first;
                        count = std::max(0, std::min(count, total - first));
                        const int cols = std::clamp(count, 1, 6);
                        const float cell_w = float(w) / cols, cell_h = float(h) / float(std::max((count + cols - 1) / cols, 1));
                        for (int j = 0; j < count; ++j) {
                            const int i = first + j;
                            const glm::vec4 b = Hud::frame_bounds(*sp, i);
                            const float fw = std::max(b.z - b.x, 1.0f), fh = std::max(b.w - b.y, 1.0f);
                            const float k = std::min((cell_w - 4) / fw, (cell_h - 16) / fh);
                            const glm::vec2 cell(float(j % cols) * cell_w, float(j / cols) * cell_h);
                            hud.draw_frame(hud_sprites, *sp, i, cell + glm::vec2(2 - b.x * k, 14 - b.y * k), k, glm::vec4(1));
                            objective_text.draw(hud_sprites, std::to_string(i), cell, w, glm::vec4(1, 1, 0, 1), {0.0f, 0.0f});
                        }
                    }
                }
                f.sprites = &hud_sprites;
            };
            if (opt.scripts) {
                logic.start();
                const auto& ls = logic.stats();
                std::printf("scripts: %zu chunks (%zu with errors), %zu objects, %zu triggers, %zu doors, %zu usable\n",
                            ls.chunks, ls.chunk_errors, ls.objects, ls.triggers, ls.doors, ls.interactive);
                combat.start();
                combat.campaign_loadout(opt.level);
                shop.apply_owned(combat, settings);
                viewmodel.start();
                hud.start();
                flares.start();
                if (opt.list_actors)
                    for (const std::string& line : combat.describe()) std::printf("  %s\n", line.c_str());
            }
            if (!opt.spawn_at.empty()) {
                float v[4] = {0, 0, 0, 0};
                if (std::sscanf(opt.spawn_at.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) >= 3) {
                    player.spawn({v[0], v[1], v[2]}, v[3]);
                } else {
                    const LevelLogic::Object* o = logic.object(std::stoi(opt.spawn_at));
                    if (!o) throw std::runtime_error("no object with id " + opt.spawn_at);
                    player.spawn(o->position, o->yaw);
                }
            }
            for (const std::string& code : opt.exec) logic.exec(code);
            for (const std::string& h : opt.hide_models) std::printf("hid %zu placements of *%s*\n", scene.hide_models(h), h.c_str());
            // A level from its start (the next campaign level, Restart Level); the arsenal carries over.
            auto load_level = [&](const std::string& level) {
                opt.level = level;
                show_loading(level);
                if (window) SDL_SetWindowTitle(window, ("N.O.V.A. 3 - " + opt.level).c_str());
                scene.load_level(opt.level, 1);
                loading.loaded = true;
                player.spawn(scene.start_position, scene.start_yaw);
                logic.start();
                combat.start();
                combat.campaign_loadout(opt.level);
                shop.apply_owned(combat, settings);
                viewmodel.start();
                hud.start();
                flares.start();
            };
            // Continue (Application::Load): the save's level from its checkpoint (as a respawn
            // there), or from its start with the arsenal carried in.
            auto resume = [&](const SaveGame& g) {
                if (!g.checkpoint.empty() && logic.load_checkpoint_data(g.checkpoint)) {
                    opt.level = g.level;
                    show_loading(g.level);
                    if (window) SDL_SetWindowTitle(window, ("N.O.V.A. 3 - " + opt.level).c_str());
                    scene.load_level(opt.level, logic.checkpoint_section());
                    loading.loaded = true;
                    player.spawn(scene.start_position, scene.start_yaw);
                    logic.respawn_at_checkpoint();
                    shop.apply_owned(combat, settings);
                    viewmodel.start();
                    hud.start();
                    flares.start();
                } else {
                    load_level(g.level);
                    if (g.has_arsenal) combat.restore_arsenal(g.arsenal), shop.apply_owned(combat, settings);
                }
                std::printf("continued %s%s\n", g.level.c_str(), g.checkpoint.empty() ? "" : " from its checkpoint");
            };
            if (!opt.continue_file.empty() && opt.scripts) {
                SaveGame g;
                if (!SaveGame::read(opt.continue_file, g)) throw std::runtime_error("cannot read the save " + opt.continue_file);
                resume(g);
            }
            // Level logic at a fixed step, so triggers and doors see every position.
            bool end_screen_done = false;   // dismissed with Continue: the level's end goes on
            auto step_logic = [&](float dt) {
                if (!opt.scripts) return;
                logic.update(dt);
                if (logic.pending_section) {   // LoadLevelSections: reload, keep the player
                    const uint32_t section = logic.pending_section;
                    const glm::vec3 feet = player.center - glm::vec3(0, 0, player.radius.z);
                    const float yaw = player.yaw;
                    std::printf("[script] loading level section mask %u\n", section);
                    show_loading(opt.level, true);   // a black screen with the rings; no press awaited
                    scene.load_level(opt.level, section);
                    loading.active = false;
                    player.spawn(feet, yaw);
                    logic.start(true);
                    combat.start(true);
                    viewmodel.start();
                    hud.start();
                    flares.start();
                }
                if (logic.respawn_pending) {   // the player died: the level again, at the checkpoint
                    std::printf("[script] respawning at checkpoint %d\n", logic.checkpoint());
                    show_loading(opt.level, true);
                    scene.load_level(opt.level, logic.checkpoint_section());
                    loading.active = false;
                    logic.respawn_at_checkpoint();
                    shop.apply_owned(combat, settings);   // what was bought for good stays
                    viewmodel.start();
                    hud.start();
                    flares.start();
                }
                // EndLevel: a moment on the faded screen, then the next campaign level (the
                // same one again after a failure). The arsenal carries over.
                static float ended_for = 0;
                if (!logic.level_ended) {
                    ended_for = 0;
                } else if ((ended_for += dt) > 3.0f) {
                    // Complete: the end-of-level screen first; its Continue comes back here.
                    if (logic.level_success && !headless && !end_screen.open && !end_screen_done) {
                        const bool finished = next_campaign_level(campaign, opt.level).empty();
                        end_screen.show(end_stats(), end_rules(), finished);
                        if (finished) settings.hard_unlocked = true;   // Hard is offered from now on
                        // CUpgradesManager::GiveUpgradePoints, kept with the settings.
                        settings.credits = std::max(settings.credits + end_screen.earned().total, 0);
                        settings.save(settings_path);
                        std::printf("end of level: %d credits (%d in all)\n", end_screen.earned().total, settings.credits);
                    }
                    if (end_screen.open) return;
                    end_screen_done = false;
                    const std::string next = logic.level_success ? next_campaign_level(campaign, opt.level) : opt.level;
                    if (next.empty()) {
                        SaveGame::remove(save_path);   // Application::DeleteCampaign
                        logic.message("The campaign is complete.", 10);
                        logic.level_ended = false;
                        return;
                    }
                    std::printf("[script] level over (%s): loading %s\n", logic.level_success ? "complete" : "failed", next.c_str());
                    for (size_t c = 0; c < campaign.size(); ++c)   // the next chapter is reached
                        if (next.find("/" + campaign[c].file + ".lvc") != std::string::npos && int(c) + 1 > settings.chapters)
                            settings.chapters = int(c) + 1, settings.save(settings_path);
                    if (logic.level_success && saving(next)) {   // GS_EndLevel: Application::Save(next level)
                        SaveGame g;
                        g.level = next, g.has_arsenal = true, g.arsenal = combat.save();
                        g.write(save_path);
                    }
                    load_level(next);
                }
            };
            constexpr float kPhysicsStep = 1.0f / 120.0f;

            Camera cam;
            uint32_t view_spawns = player.spawns;
            cam.position = opt.have_eye ? glm::vec3(opt.eye) : player.eye();
            cam.yaw = opt.have_eye ? opt.eye.w : scene.start_yaw;
            cam.pitch = opt.pitch;
            if (!opt.focus.empty() && !focus_camera(scene, gol, opt.focus, cam.position, cam.yaw, cam.pitch))
                throw std::runtime_error("no placement matching " + opt.focus);
            for (const std::string& name : opt.effects) {
                const glm::vec3 at = cam.position + cam.forward() * 6.0f;
                if (scene.particles().start(name, glm::translate(glm::mat4(1), at)) < 0)
                    std::fprintf(stderr, "no effect named %s\n", name.c_str());
            }

            if (opt.audit > 0) {
                // Walk random paths on the nav mesh like a player would (zone tracking follows
                // portal crossings) and, every 2 m, render with zone culling on and off. Views
                // where culling hides what is really in view are saved for inspection.
                Navigation nav;
                nav.build(scene.level());
                std::mt19937 rng(7);
                const std::string dir = "work/engine_test/audit";
                SDL_CreateDirectory(dir.c_str());
                int bad = 0, samples = 0, flips = 0, steps_since_change = 0;
                const int w = 320, h = 180;
                // Holes: with no sky and a magenta clear colour, pixels that show nothing with
                // culling on but geometry with it off.
                scene.set_skybox(-999999);
                auto magenta = [](const Image& img, size_t i) {
                    return img.rgba[i] > 240 && img.rgba[i + 1] < 15 && img.rgba[i + 2] > 240;
                };
                auto diff = [&](const Image& a, const Image& b) {
                    size_t n = 0;
                    for (size_t i = 0; i < a.rgba.size(); i += 4) n += magenta(a, i) && !magenta(b, i);
                    return float(n) / float(a.rgba.size() / 4);
                };
                for (int pi = 0; pi < opt.audit && !nav.empty(); ++pi) {
                    std::vector<glm::vec3> path;
                    glm::vec3 a, b;
                    int ta = -1, tb = -1;
                    for (int tries = 0; tries < 50; ++tries) {
                        ta = int(std::uniform_int_distribution<size_t>(0, nav.triangle_count() - 1)(rng));
                        tb = int(std::uniform_int_distribution<size_t>(0, nav.triangle_count() - 1)(rng));
                        a = nav.center(ta), b = nav.center(tb);
                        if (glm::distance(a, b) > 15 && nav.find_path(ta, a, tb, b, path)) break;
                        path.clear();
                    }
                    if (path.empty()) continue;
                    path.insert(path.begin(), a);
                    float since = 2;
                    Camera eye;
                    for (size_t k = 0; k + 1 < path.size(); ++k) {
                        const glm::vec3 p0 = path[k], p1 = path[k + 1];
                        const float len = glm::distance(p0, p1);
                        const glm::vec3 dir3 = len > 0 ? (p1 - p0) / len : glm::vec3(0, 1, 0);
                        for (float t = 0; t < len; t += 0.25f) {
                            eye.position = p0 + dir3 * t + glm::vec3(0, 0, 1.65f);
                            eye.yaw = glm::degrees(std::atan2(-dir3.x, dir3.y));
                            FrameParams f = frame(eye, w, h);
                            f.clear = {1, 0, 1, 1};
                            scene.cull(f.camera, f.proj * f.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
                            {   // zone flips: A -> B -> A within a few steps
                                static std::vector<int> recent;
                                const int z = scene.zones().current();
                                if (recent.empty() || recent.back() != z) {
                                    if (recent.size() >= 2 && recent[recent.size() - 2] == z && steps_since_change < 8) ++flips;
                                    recent.push_back(z);
                                    steps_since_change = 0;
                                }
                                ++steps_since_change;
                            }
                            if ((since += 0.25f) < 2) continue;
                            since = 0;
                            ++samples;
                            const Image on = renderer.render_to_image(scene.draws(), f, w, h);
                            scene.zones().enabled = false;
                            scene.cull(f.camera, f.proj * f.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
                            const Image off = renderer.render_to_image(scene.draws(), f, w, h);
                            scene.zones().enabled = true;
                            scene.cull(f.camera, f.proj * f.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
                            const float d = diff(on, off);
                            if (d < 0.01f) continue;
                            ++bad;
                            const int z = scene.zones().current();
                            char name[256];
                            std::snprintf(name, sizeof name, "%s/%03d_%.0f_%.0f_%.0f_y%.0f_zone%d_%.0fpct.bmp", dir.c_str(), bad,
                                          eye.position.x, eye.position.y, eye.position.z, eye.yaw,
                                          z >= 0 ? scene.zones().zones()[size_t(z)].id : -1, d * 100);
                            Image both{w * 2, h, std::vector<uint8_t>(size_t(w) * 2 * h * 4)};
                            for (int y = 0; y < h; ++y) {
                                std::memcpy(&both.rgba[(size_t(y) * w * 2) * 4], &on.rgba[size_t(y) * w * 4], size_t(w) * 4);
                                std::memcpy(&both.rgba[(size_t(y) * w * 2 + w) * 4], &off.rgba[size_t(y) * w * 4], size_t(w) * 4);
                            }
                            save_bmp(both, name);
                            std::printf("audit: %s\n", name);
                        }
                    }
                }
                std::printf("audit: %d of %d views have holes from zone culling; %d quick zone flips\n", bad, samples, flips);
                return 0;
            }
            if (opt.walk_test > 0) {
                // Settle for a second, then walk forward; report the path.
                const glm::vec3 start = player.center;
                const int steps = int(opt.walk_test / kPhysicsStep);
                float travelled = 0;
                glm::vec3 last = player.center;
                for (int i = 0; i <= steps; ++i) {
                    PlayerInput in;
                    in.forward = i * kPhysicsStep >= 1.0f ? 1.0f : 0.0f;
                    player.update(scene.collision(), in, kPhysicsStep);
                    step_logic(kPhysicsStep);
                    {   // zone tracking follows the walker
                        Camera eye = cam;
                        eye.position = player.eye();
                        eye.yaw = player.yaw;
                        const FrameParams wf = frame(eye, opt.width, opt.height);
                        scene.cull(wf.camera, wf.proj * wf.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
                    }
                    travelled += glm::length(glm::vec2(player.center - last));
                    last = player.center;
                    if (i % 60 == 0) {
                        std::printf("t=%5.2fs  pos=(%8.2f %8.2f %7.2f)  %s\n", i * kPhysicsStep, player.center.x,
                                    player.center.y, player.center.z, player.on_ground ? "ground" : "air");
                    }
                }
                if (travelled < 0.5f) {   // stuck: what is around
                    float floor_z = 0;
                    if (scene.collision().ray_down(player.center + glm::vec3(0, 0, 2), 10, floor_z))
                        std::printf("  centre z %.3f, feet %.3f, floor below at %.3f\n", player.center.z, player.center.z - player.radius.z, floor_z);
                    for (float h : {-0.8f, -0.4f, 0.0f, 0.6f, 1.2f}) {
                        std::printf("  rays at %+.1f m:", h);
                        for (int k = 0; k < 8; ++k) {
                            const float a = glm::radians(45.0f * k);
                            const glm::vec3 from = player.center + glm::vec3(0, 0, h);
                            float t = 1;
                            const bool hit = scene.collision().raycast(from, from + 5.0f * glm::vec3(std::cos(a), std::sin(a), 0), t);
                            std::printf(" %.2f", hit ? t * 5 : 9.0f);
                        }
                        std::printf("\n");
                    }
                    for (uint32_t owner : scene.collision().owners_near(player.center, 1.5f)) {
                        const LevelObject& o = scene.level().objects.at(owner);
                        std::printf("  stuck near object %u: %s %s id %d\n", owner, o.kind.c_str(), o.name.c_str(), o.base ? o.base->id : -1);
                    }
                }
                std::printf("walk test: start z %.2f, end z %.2f, horizontal distance %.2f m, %s\n", start.z,
                            player.center.z, travelled, player.on_ground ? "on ground" : "in the air");
                cam.position = player.eye();
                cam.yaw = player.yaw;
            }
            if (headless || opt.walk_test > 0) {
                if (opt.screenshot.empty()) return 0;
                float report = 0;
                const float run_time = std::max(opt.run, opt.combat_test);
                glm::vec3 touch_from{0};
                float touch_yaw = 0;
                bool touching = false;
                for (float t = 0; t < run_time; t += kPhysicsStep) {   // stand still and let the scripts run
                    PlayerInput pin;
                    if (opt.touch_test > 0 && t >= run_time - opt.touch_test) {
                        // Fingers: 1 holds the walk pad forward, 2 swipes right to turn.
                        touch.layout(opt.width, opt.height);
                        const glm::vec2 swipe(float(opt.width) * 0.6f, float(opt.height) * 0.4f);
                        if (!touching) {
                            touching = true;
                            touch.down(1, touch.pad_anchor());
                            touch.down(2, swipe);
                            touch_from = player.center, touch_yaw = player.yaw;
                        }
                        const float k = (t - (run_time - opt.touch_test)) / opt.touch_test;
                        touch.move(1, touch.pad_anchor() - glm::vec2(0, float(opt.height)));
                        touch.move(2, swipe + glm::vec2(k * float(opt.width) * 0.2f, 0));
                        const TouchControls::Output o = touch.take();
                        pin.forward = o.forward, pin.right = o.right;
                        player.yaw += o.look.x;
                    }
                    if (opt.scripts) logic.advance_real_time(kPhysicsStep);   // QTE bookkeeping (headless keeps full speed)
                    if (float ly, lp; opt.scripts && logic.look_target(ly, lp)) player.yaw = ly, player.pitch = lp;   // PlayerLookTo
                    if (opt.scripts) player.eye_height = logic.in_mech() ? 7.4f : 0.75f;
                    glm::vec3 seat_eye;
                    float seat_min = 0, seat_max = 0;
                    const bool seated = opt.scripts && logic.seat(seat_eye, seat_min, seat_max);
                    if (seated) player.center = seat_eye - glm::vec3(0, 0, player.eye_height);
                    else if (!(opt.scripts && logic.cinematic_active()) && !(opt.scripts && logic.limited_step(pin.right, kPhysicsStep)))
                        player.update(scene.collision(), pin, kPhysicsStep);
                    if (opt.scripts) logic.limited_view_input(player.yaw, player.pitch, kPhysicsStep);
                    step_logic(kPhysicsStep);
                    if (opt.scripts) {
                        CombatInput ci;
                        glm::vec3 aim = player.view_forward(), target;
                        if (opt.grenade_test >= 0 && t <= opt.grenade_test && t + kPhysicsStep > opt.grenade_test) {
                            if (combat.nearest_enemy(player.eye(), target)) {
                                const glm::vec3 d = target - player.eye();
                                aim = glm::normalize(d + glm::vec3(0, 0, glm::length(d) * 0.25f));   // lob it
                            }
                            combat.give_ammo("Grenade", 1);
                            ci.grenade = true;
                            std::printf("t=%.2f grenade thrown, %d enemies alive\n", t, combat.enemies_alive());
                        }
                        if (opt.mine_test >= 0 && t <= opt.mine_test && t + kPhysicsStep > opt.mine_test) {
                            if (combat.nearest_enemy(player.eye(), target)) {
                                const glm::vec3 d = target - player.eye();
                                aim = glm::normalize(d + glm::vec3(0, 0, glm::length(d) * 0.25f));
                            }
                            ++combat.mines;
                            player.pitch = -35;   // onto the ground ahead
                            if (!combat.mines_picked()) ci.switch_throwable = true;
                            ci.grenade = true;
                            std::printf("t=%.2f mine thrown, %d enemies alive\n", t, combat.enemies_alive());
                        }
                        if (opt.mine_test >= 0 && t <= opt.mine_test + 2 && t + kPhysicsStep > opt.mine_test + 2) {
                            ci.detonate = true;
                            std::printf("t=%.2f detonate (%s), %d enemies alive\n", t, combat.mines_placed() ? "placed" : "none placed",
                                        combat.enemies_alive());
                        }
                        if (opt.power_test >= 0 && t <= opt.power_test && t + kPhysicsStep > opt.power_test) {
                            if (combat.nearest_enemy(player.eye(), target)) aim = glm::normalize(target - player.eye());
                            ci.power = true;
                            std::printf("t=%.2f repel\n", t);
                        }
                        if (opt.combat_test > 0 && combat.nearest_enemy(player.eye(), target)) {
                            aim = glm::normalize(target - player.eye());
                            player.yaw = glm::degrees(std::atan2(-aim.x, aim.y));
                            player.pitch = glm::degrees(std::asin(glm::clamp(aim.z, -1.0f, 1.0f)));
                            ci.fire = true;
                        }
                        combat.update(kPhysicsStep, ci, player.eye(), aim);
                        combat.push_player_out();
                        if (opt.combat_test > 0 && (report += kPhysicsStep) >= 1.0f) {
                            report = 0;
                            const auto& cs = combat.stats();
                            std::printf("t=%5.1fs health %5.1f%s, enemies alive %d, shots %zu hits %zu kills %zu, enemy shots %zu, "
                                        "damage taken %.0f, ammo %d/%d\n", t, combat.player_health,
                                        combat.player_dead ? " (dead)" : "", combat.enemies_alive(), cs.shots, cs.hits,
                                        cs.kills, cs.enemy_shots, cs.damage_taken, combat.ammo_in_clip(), combat.ammo_reserve());
                        }
                    }
                    scene.update(kPhysicsStep, cam.position);
                    if (opt.scripts) {
                        ViewModel::Input vm;
                        vm.weapon = combat.weapon_type();
                        vm.visible = !combat.player_dead && !logic.player_locked() && !logic.limited_active() && (!combat.special_weapon() || logic.in_mech());
                        vm.cockpit = logic.in_mech();
                        if (!logic.weapon_state.empty()) viewmodel.set_state(logic.weapon_state), logic.weapon_state.clear();
                        vm.firing = combat.since_shot() < 0.2f;
                        vm.reloading = combat.reloading();
                        viewmodel.update(kPhysicsStep, player.eye(), player.yaw, player.pitch, vm);
                        update_frost(kPhysicsStep);
                    }
                    if (logic.audio && !opt.sound_render.empty()) {
                        logic.audio->set_listener(player.eye(), player.view_forward(), glm::vec3(0, 0, 1));
                        const int frames = int(std::lround(double(t + kPhysicsStep) * logic.audio->rate())) -
                                           int(std::lround(double(t) * logic.audio->rate()));
                        const size_t at = rendered.size();
                        rendered.resize(at + size_t(frames) * 2);
                        logic.audio->render(rendered.data() + at, frames);
                    }
                    Camera eye = cam;
                    if (!opt.have_eye && opt.focus.empty()) eye.position = player.eye(), eye.yaw = player.yaw, eye.pitch = player.pitch;
                    const FrameParams rf = frame(eye, opt.width, opt.height);
                    scene.cull(rf.camera, rf.proj * rf.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
                }
                if (!opt.sound_render.empty() && logic.audio) {
                    if (!save_wav(rendered, logic.audio->rate(), opt.sound_render)) throw std::runtime_error("cannot write " + opt.sound_render);
                    std::printf("wrote %s (%.1f s of sound)\n", opt.sound_render.c_str(), double(rendered.size() / 2) / logic.audio->rate());
                }
                if (opt.list_actors && opt.scripts)
                    for (const std::string& line : combat.describe())
                        if (line.find(" enabled") != std::string::npos) std::printf("  end: %s\n", line.c_str());
                if (run_time > 0 && !opt.have_eye && opt.focus.empty()) {
                    cam.position = player.eye();
                    cam.yaw = player.yaw;
                    cam.pitch = player.pitch;
                }
                scene.update(opt.time, cam.position);
                if (opt.scripts) {
                    ViewModel::Input vm;
                    vm.weapon = combat.weapon_type();
                    vm.visible = !combat.player_dead && !logic.player_locked() && !logic.limited_active() && (!combat.special_weapon() || logic.in_mech());
                    vm.cockpit = logic.in_mech();
                    if (!logic.weapon_state.empty()) viewmodel.set_state(logic.weapon_state), logic.weapon_state.clear();
                    vm.firing = combat.since_shot() < 0.2f;
                    vm.reloading = combat.reloading();
                    viewmodel.update(1.0f / 60.0f, cam.position, cam.yaw, cam.pitch, vm);
                }
                Camera shot_cam = cam;
                if (Scene::CameraView cv; opt.scripts && !opt.have_eye && logic.cinematic_view(cv)) {   // --eye wins
                    shot_cam.position = cv.position, shot_cam.directed = true, shot_cam.direction = cv.forward, shot_cam.up = cv.up;
                    shot_cam.fov = glm::degrees(cv.fov), shot_cam.game_fov = false;
                } else if (opt.scripts && !opt.have_eye && logic.limited_view(cv)) {   // the limited-control rig's camera
                    shot_cam.position = cv.position, shot_cam.directed = true, shot_cam.direction = cv.forward, shot_cam.up = cv.up;
                }
                FrameParams shot = frame(shot_cam, opt.width, opt.height);
                bool scene_hidden = false;
                overlay.clear();
                if (!opt.menu_keys.empty()) {
                    menu.show();
                    for (char c : opt.menu_keys) {
                        if (c == '.') continue;
                        using K = PauseMenu::Key;
                        const K k = c == 'u' ? K::Up : c == 'd' ? K::Down : c == 'l' ? K::Left : c == 'r' ? K::Right : c == 'b' ? K::Back : K::Activate;
                        if (menu.key(k, settings) == PauseMenu::Action::Resume) menu.open = false;
                    }
                    menu.update(1.0f);
                }
                if (opt.scripts && !logic.player_locked()) combat.draw(overlay, shot.camera, opt.width, opt.height);
                if (opt.scripts && logic.fade_alpha() > 0) fullscreen(overlay, opt.width, opt.height, glm::vec4(logic.fade_color, logic.fade_alpha()));
                shot.overlay = &overlay;
                draw_text(shot, opt.width, opt.height);
                scene.cull(shot.camera, shot.proj * shot.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
                if (opt.trace_zones) {
                    const ZoneSystem& z = scene.zones();
                    std::printf("camera %.1f %.1f %.1f zone %d (id %d); visible:", shot.camera.x, shot.camera.y, shot.camera.z,
                                z.current(), z.current() >= 0 ? z.zones()[size_t(z.current())].id : -1);
                    for (size_t i = 0; i < z.zones().size(); ++i)
                        if (z.visible(int(i))) std::printf(" %d", z.zones()[i].id);
                    std::printf("\n");
                    for (const auto& pt : z.portals())
                        std::printf("  portal %d zones %d-%d %s center (%.1f %.1f %.1f)%s\n", pt.id, z.zones()[size_t(pt.zone[0])].id,
                                    z.zones()[size_t(pt.zone[1])].id, pt.enabled ? "open" : "closed", pt.center.x, pt.center.y,
                                    pt.center.z, pt.door != std::numeric_limits<size_t>::max() ? " (door)" : "");
                    for (size_t i = 0; i < z.zones().size(); ++i) {
                        const auto& zz = z.zones()[i];
                        std::printf("  zone %zu id %d bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f) portals %zu children %zu\n", i, zz.id,
                                    zz.min.x, zz.min.y, zz.min.z, zz.max.x, zz.max.y, zz.max.z, zz.portals.size(), zz.children.size());
                    }
                }
                if (!opt.main_menu_keys.empty()) {   // the main menu instead of the view
                    main_menu.unlocked = std::max(1, int(campaign.size()) / 2);
                    main_menu.show();
                    for (char c : opt.main_menu_keys) {
                        using K = MainMenu::Key;
                        if (c == '.') continue;
                        main_menu.key(c == 'u' ? K::Up : c == 'd' ? K::Down : c == 'b' ? K::Back : K::Activate);
                    }
                    shot = draw_main_menu(opt.width, opt.height);
                    scene_hidden = true;
                }
                if (opt.end_screen >= 0) {   // the end-of-level screen over the view
                    EndLevelScreen::Stats st = end_stats();
                    st.headshots = 7, st.blast_kills = 3, st.kills = 41, st.accuracy = 0.46f, st.play_time = 734, st.retries = 2;
                    end_screen.show(st, end_rules(), false);
                    std::printf("end of level: %d credits\n", end_screen.earned().total);
                    end_screen.update(opt.end_screen);
                    draw_text(shot, opt.width, opt.height);
                }
                if (opt.death_screen >= 0) {   // the death screen over the view
                    death.show();
                    death.update(opt.death_screen);
                    draw_text(shot, opt.width, opt.height);
                }
                if (opt.loading_screen >= 0) {   // the level's loading screen instead of the view
                    loading.begin(campaign_level(opt.level));
                    for (int k = 0; k < 10; ++k) loading.update(0.1f);
                    loading.loaded = opt.loading_screen > 0;
                    for (int k = 0; k < 10 && loading.loaded; ++k) loading.update(0.1f);
                    shot = draw_loading(opt.width, opt.height);
                    scene_hidden = true;
                }
                const Image img = renderer.render_to_image(scene_hidden ? std::vector<DrawItem>{} : scene.draws(), shot, opt.width, opt.height);
                if (!save_bmp(img, opt.screenshot)) throw std::runtime_error("cannot write " + opt.screenshot);
                if (opt.bench > 0) {
                    const auto t0 = std::chrono::steady_clock::now();
                    for (int i = 0; i < opt.bench; ++i) {
                        scene.update(1.0f / 60.0f, cam.position);
                        const FrameParams bf = frame(cam, opt.width, opt.height);
                        scene.cull(bf.camera, bf.proj * bf.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
                        renderer.render_to_image(scene.draws(), bf, opt.width, opt.height);
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    std::printf("bench: %.2f ms/frame (update + render + readback)\n", ms / opt.bench);
                }
                if (opt.have_probe || !opt.probe_model.empty())
                    for (size_t i = 0; i < scene.draws().size(); ++i) {
                        const Scene::DrawInfo& di = scene.draw_info()[i];
                        if (!opt.probe_model.empty() ? di.name.find(opt.probe_model) == std::string::npos
                                                     : (glm::any(glm::lessThan(opt.probe, di.min - 0.5f)) ||
                                                        glm::any(glm::greaterThan(opt.probe, di.max + 0.5f))))
                            continue;
                        const DrawItem& d = scene.draws()[i];
                        std::printf("probe: draw %zu %s%s%s%s%s%s bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", i, di.name.c_str(),
                                    d.blend ? " blend" : "", d.visible ? "" : " lod-hidden", d.hidden ? " hidden" : "",
                                    d.anim_hidden ? " anim-hidden" : "", d.zone_hidden ? " zone-hidden" : "", di.min.x, di.min.y,
                                    di.min.z, di.max.x, di.max.y, di.max.z);
                    }
                if (touching)
                    std::printf("touch test: walked %.2f m, turned %.1f degrees\n", glm::length(glm::vec2(player.center - touch_from)),
                                player.yaw - touch_yaw);
                if (opt.log_player)
                    std::printf("player at (%.2f %.2f %.2f) yaw %.1f pitch %.1f %s\n", player.center.x, player.center.y, player.center.z, player.yaw, player.pitch,
                                player.on_ground ? "on the ground" : "in the air");
                std::printf("coronas: %zu of %zu shown\n", scene.stats().coronas_shown, scene.stats().corona_controllers);
                std::printf("particles: %zu in %zu batches\n", scene.particles().particle_count(), particle_list.batches.size());
                std::printf("wrote %s\n", opt.screenshot.c_str());
            } else {
                bool flying = false;
                float accumulator = 0;
                CombatInput combat_in;
                bool fire_held = false, aim_held = false, aim_toggled = false;
                // Touch: fingers drive the on-screen controls, and the mouse stands in for a
                // finger (a free pointer); else the mouse looks around.
                const bool touch_mode = opt.scripts && logic.input == InputScheme::Touch;
                SDL_SetWindowRelativeMouseMode(window, !touch_mode);
                auto to_pixels = [&](float x, float y, bool normalized) {
                    int ww = 1, wh = 1, pw = 1, ph = 1;
                    SDL_GetWindowSize(window, &ww, &wh);
                    SDL_GetWindowSizeInPixels(window, &pw, &ph);
                    return normalized ? glm::vec2(x * float(pw), y * float(ph))
                                      : glm::vec2(x * float(pw) / float(std::max(ww, 1)), y * float(ph) / float(std::max(wh, 1)));
                };
                bool running = true;
                // The pause menu: the game stands still, the pointer is free, the sound stops.
                // Display settings: the window's size or fullscreen mode, V-Sync.
                auto apply_display = [&] {
                    if (settings.display == Settings::DisplayMode::Windowed) {
                        SDL_SetWindowFullscreen(window, false);
                        SDL_SetWindowBordered(window, true);
                        // A window larger than the desktop's room becomes the largest 16:9 that fits.
                        int ww = settings.width, wh = settings.height;
                        if (SDL_Rect room; SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &room) && (ww > room.w || wh > room.h)) {
                            const float k = std::min(float(room.w) / float(ww), float(room.h - 40) / float(wh));
                            ww = int(float(ww) * k), wh = int(float(wh) * k);
                        }
                        SDL_SetWindowSize(window, ww, wh);
                        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    } else {
                        SDL_DisplayMode mode{};
                        const bool exclusive = settings.display == Settings::DisplayMode::Fullscreen &&
                                               SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window), settings.width,
                                                                                   settings.height, 0.0f, true, &mode);
                        SDL_SetWindowFullscreenMode(window, exclusive ? &mode : nullptr);   // null: borderless desktop
                        SDL_SetWindowFullscreen(window, true);
                    }
                    renderer.set_vsync(settings.vsync);
                };
                touch.mirror = settings.left_handed;
                if (!opt.size_given) apply_display();
                else renderer.set_vsync(settings.vsync);
                apply_sound();
                auto set_menu = [&](bool on) {
                    if (on) menu.show();
                    else menu.open = false;
                    SDL_SetWindowRelativeMouseMode(window, !on && !touch_mode && !main_menu.open);
                    fire_held = aim_held = false;
                    touch.release_all();
                    if (logic.audio) logic.audio->set_paused(on);
                };
                auto menu_action = [&](PauseMenu::Action a) {
                    using A = PauseMenu::Action;
                    switch (a) {
                        case A::Resume: set_menu(false); break;
                        case A::Exit:   // back to the main menu
                            set_menu(false), main_menu.show();
                            SDL_SetWindowRelativeMouseMode(window, false);
                            if (logic.audio) logic.audio->set_paused(true);
                            break;
                        case A::Changed:
                            settings.save(settings_path), aim_toggled = false, apply_sound();
                            touch.mirror = settings.left_handed, touch.relayout();
                            break;
                        case A::DisplayChanged: settings.save(settings_path), apply_display(); break;
                        case A::RestartCheckpoint: set_menu(false), logic.respawn_player(); break;
                        case A::RestartLevel: set_menu(false), load_level(opt.level); break;
                        case A::None: break;
                    }
                };
                auto end_action = [&](EndLevelScreen::Action a) {
                    if (a == EndLevelScreen::Action::None) return;
                    end_screen.open = false;
                    end_screen_done = true;   // the level's end goes on: the next level
                    const std::string next = next_campaign_level(campaign, opt.level);
                    for (size_t c = 0; c < campaign.size(); ++c)   // the next chapter is reached
                        if (!next.empty() && next.find("/" + campaign[c].file + ".lvc") != std::string::npos && int(c) + 1 > settings.chapters)
                            settings.chapters = int(c) + 1, settings.save(settings_path);
                    if (a == EndLevelScreen::Action::Menu) {
                        logic.level_ended = false, end_screen_done = false;
                        main_menu.show();
                    } else {
                        SDL_SetWindowRelativeMouseMode(window, !touch_mode);
                    }
                };
                auto death_action = [&](DeathScreen::Action a) {
                    if (a == DeathScreen::Action::None) return;
                    death.open = false;
                    combat.respawn();
                    if (a == DeathScreen::Action::Menu) {
                        main_menu.show();
                        SDL_SetWindowRelativeMouseMode(window, false);
                    } else {
                        SDL_SetWindowRelativeMouseMode(window, !touch_mode);
                    }
                };
                auto main_action = [&](MainMenu::Action a) {
                    using A = MainMenu::Action;
                    switch (a) {
                        case A::Play: {
                            main_menu.open = false;
                            settings.difficulty = main_menu.difficulty, settings.save(settings_path);   // Menu_SetDifficulty
                            combat.set_difficulty(settings.difficulty), shop.difficulty = settings.difficulty;
                            const std::string level = "levels/" + campaign[size_t(main_menu.chosen)].file + ".lvc";
                            SaveGame g;
                            if (saving(level) && SaveGame::read(save_path, g) && chapter_of(g.level) == main_menu.chosen) {
                                resume(g);
                            } else {
                                load_level(level);
                            }
                            SDL_SetWindowRelativeMouseMode(window, !touch_mode);
                            break;
                        }
                        case A::Options: menu.show_options(); break;
                        case A::Quit: running = false; break;
                        case A::None: break;
                    }
                };
                if (main_menu.open) SDL_SetWindowRelativeMouseMode(window, false);
                auto last = std::chrono::steady_clock::now();
                while (running) {
                    if (touch_mode) {
                        int pw = 0, ph = 0;
                        SDL_GetWindowSizeInPixels(window, &pw, &ph);
                        touch.layout(pw, ph);
                    }
                    SDL_Event e;
                    while (SDL_PollEvent(&e)) {
                        if (e.type == SDL_EVENT_QUIT) running = false;
                        if (loading.active) {   // the loading screen takes every input; a press dismisses it
                            const bool pressed = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                                 e.type == SDL_EVENT_FINGER_DOWN;
                            if (pressed && loading.press() && logic.audio) logic.audio->set_paused(menu.open);
                            continue;
                        }
                        if (main_menu.open && !menu.open) {   // the main menu takes every input
                            using K = MainMenu::Key;
                            using P = MainMenu::Pointer;
                            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                                switch (e.key.key) {
                                    case SDLK_ESCAPE: case SDLK_BACKSPACE: main_action(main_menu.key(K::Back)); break;
                                    case SDLK_UP: case SDLK_W: main_action(main_menu.key(K::Up)); break;
                                    case SDLK_DOWN: case SDLK_S: main_action(main_menu.key(K::Down)); break;
                                    case SDLK_LEFT: case SDLK_A: main_action(main_menu.key(K::Left)); break;
                                    case SDLK_RIGHT: case SDLK_D: main_action(main_menu.key(K::Right)); break;
                                    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: main_action(main_menu.key(K::Activate)); break;
                                    default: break;
                                }
                            } else if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
                                main_action(main_menu.pointer(to_pixels(e.motion.x, e.motion.y, false), P::Move));
                            } else if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                                       e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) {
                                main_action(main_menu.pointer(to_pixels(e.button.x, e.button.y, false),
                                                              e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? P::Press : P::Release));
                            } else if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP) {
                                main_action(main_menu.pointer(to_pixels(e.tfinger.x, e.tfinger.y, true),
                                                              e.type == SDL_EVENT_FINGER_DOWN ? P::Press : P::Release));
                            }
                            continue;
                        }
                        if (end_screen.open && !menu.open) {   // Back to Menu or Continue
                            using K = EndLevelScreen::Key;
                            using P = EndLevelScreen::Pointer;
                            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                                switch (e.key.key) {
                                    case SDLK_LEFT: case SDLK_A: end_action(end_screen.key(K::Left)); break;
                                    case SDLK_RIGHT: case SDLK_D: end_action(end_screen.key(K::Right)); break;
                                    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: end_action(end_screen.key(K::Activate)); break;
                                    default: break;
                                }
                            } else if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
                                end_action(end_screen.pointer(to_pixels(e.motion.x, e.motion.y, false), P::Move));
                            } else if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                                       e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) {
                                end_action(end_screen.pointer(to_pixels(e.button.x, e.button.y, false),
                                                              e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? P::Press : P::Release));
                            } else if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP) {
                                end_action(end_screen.pointer(to_pixels(e.tfinger.x, e.tfinger.y, true),
                                                              e.type == SDL_EVENT_FINGER_DOWN ? P::Press : P::Release));
                            }
                            continue;
                        }
                        if (death.open && !menu.open) {   // Retry or Back to Menu
                            using K = DeathScreen::Key;
                            using P = DeathScreen::Pointer;
                            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                                switch (e.key.key) {
                                    case SDLK_UP: case SDLK_W: death_action(death.key(K::Up)); break;
                                    case SDLK_DOWN: case SDLK_S: death_action(death.key(K::Down)); break;
                                    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: death_action(death.key(K::Activate)); break;
                                    default: break;
                                }
                            } else if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
                                death_action(death.pointer(to_pixels(e.motion.x, e.motion.y, false), P::Move));
                            } else if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                                       e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) {
                                death_action(death.pointer(to_pixels(e.button.x, e.button.y, false),
                                                           e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? P::Press : P::Release));
                            } else if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP) {
                                death_action(death.pointer(to_pixels(e.tfinger.x, e.tfinger.y, true),
                                                           e.type == SDL_EVENT_FINGER_DOWN ? P::Press : P::Release));
                            }
                            continue;
                        }
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !e.key.repeat) {
                            if (menu.open) menu_action(menu.key(PauseMenu::Key::Back, settings));   // back a page, or resume
                            else set_menu(true);
                            continue;
                        }
                        if (menu.open) {   // everything goes to the menu
                            using K = PauseMenu::Key;
                            using P = PauseMenu::Pointer;
                            if (e.type == SDL_EVENT_KEY_DOWN) {
                                switch (e.key.key) {
                                    case SDLK_BACKSPACE: menu_action(menu.key(K::Back, settings)); break;
                                    case SDLK_UP: case SDLK_W: menu_action(menu.key(K::Up, settings)); break;
                                    case SDLK_DOWN: case SDLK_S: menu_action(menu.key(K::Down, settings)); break;
                                    case SDLK_LEFT: case SDLK_A: menu_action(menu.key(K::Left, settings)); break;
                                    case SDLK_RIGHT: case SDLK_D: menu_action(menu.key(K::Right, settings)); break;
                                    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: menu_action(menu.key(K::Activate, settings)); break;
                                    default: break;
                                }
                            } else if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
                                menu_action(menu.pointer(to_pixels(e.motion.x, e.motion.y, false), P::Move, settings));
                            } else if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                                       e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) {
                                menu_action(menu.pointer(to_pixels(e.button.x, e.button.y, false),
                                                         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? P::Press : P::Release, settings));
                            } else if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_FINGER_UP) {
                                menu_action(menu.pointer(to_pixels(e.tfinger.x, e.tfinger.y, true),
                                                         e.type == SDL_EVENT_FINGER_DOWN ? P::Press : e.type == SDL_EVENT_FINGER_UP ? P::Release : P::Move,
                                                         settings));
                            }
                            continue;
                        }
                        if (touch_mode) {
                            constexpr int64_t kMouseFinger = -1;
                            switch (e.type) {
                                case SDL_EVENT_FINGER_DOWN:
                                    touch.down(int64_t(e.tfinger.fingerID), to_pixels(e.tfinger.x, e.tfinger.y, true));
                                    break;
                                case SDL_EVENT_FINGER_MOTION:
                                    touch.move(int64_t(e.tfinger.fingerID), to_pixels(e.tfinger.x, e.tfinger.y, true));
                                    break;
                                case SDL_EVENT_FINGER_UP:
                                case SDL_EVENT_FINGER_CANCELED:
                                    touch.up(int64_t(e.tfinger.fingerID), to_pixels(e.tfinger.x, e.tfinger.y, true));
                                    break;
                                // Mouse events made from touches are already handled above.
                                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                                    if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT)
                                        touch.down(kMouseFinger, to_pixels(e.button.x, e.button.y, false));
                                    break;
                                case SDL_EVENT_MOUSE_MOTION:
                                    if (e.motion.which != SDL_TOUCH_MOUSEID) touch.move(kMouseFinger, to_pixels(e.motion.x, e.motion.y, false));
                                    break;
                                case SDL_EVENT_MOUSE_BUTTON_UP:
                                    if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT)
                                        touch.up(kMouseFinger, to_pixels(e.button.x, e.button.y, false));
                                    break;
                                case SDL_EVENT_WINDOW_FOCUS_LOST: touch.release_all(); break;
                                default: break;
                            }
                            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION) continue;
                        }
                        if (e.type == SDL_EVENT_QUIT) running = false;
                        // Debug: F1 free flight, F2 zone culling, F3 built-in shaders.
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F1 && !e.key.repeat) {
                            flying = !flying;
                            if (!flying) player.spawn(cam.position - glm::vec3(0, 0, player.radius.z + player.eye_height), cam.yaw);
                        }
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_E && !e.key.repeat && opt.scripts && !logic.limited_tap()) logic.use();
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F2 && !e.key.repeat) scene.zones().enabled = !scene.zones().enabled;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_R && !e.key.repeat) combat_in.reload = true;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_G && !e.key.repeat && logic.controls.items) combat_in.grenade = true;
                        // Grenades or mines on the grenade button (the original's swipe), and the
                        // detonation of the mines placed.
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_H && !e.key.repeat && logic.controls.items) combat_in.switch_throwable = true;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_X && !e.key.repeat && logic.controls.items) combat_in.detonate = true;
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
                            fire_held = true, logic.qte_press("fire"), logic.limited_tap();
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_TAB && !e.key.repeat && logic.controls.powers) combat.cycle_power();
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_Q && !e.key.repeat) {
                            if (!logic.qte_press("repel") && logic.controls.powers) combat_in.power = true;
                        }
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) fire_held = false;
                        // Aim: hold the right button, or (settings) click it to switch aim mode on and off.
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT) {
                            if (settings.aim_toggle) aim_toggled = !aim_toggled;
                            else aim_held = true;
                        }
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) aim_held = false;
                        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) fire_held = false, aim_held = false;
                        // Weapons: number keys pick the n-th weapon the player has; the wheel cycles.
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key >= SDLK_1 && e.key.key <= SDLK_9 && logic.controls.weapons)
                            combat_in.select = combat.owned_weapon(int(e.key.key - SDLK_1));
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F3 && !e.key.repeat) {
                            scene.set_game_shaders(!scene.game_shaders());
                        }
                        if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0 && logic.controls.weapons)
                            combat_in.select = combat.cycle_weapon(e.wheel.y < 0 ? 1 : -1);
                        if (e.type == SDL_EVENT_MOUSE_MOTION && !(opt.scripts && (logic.player_locked() || !logic.controls.rotate))) {
                            // Slower while aiming (zoomed in); the Sensitivity and Invert Y-Axis settings.
                            const float k = 0.12f * cam.fov / kFov * settings.sensitivity * logic.look_sensitivity;
                            cam.yaw -= e.motion.xrel * k;
                            cam.pitch = glm::clamp(cam.pitch - e.motion.yrel * k * (settings.invert_y ? -1.0f : 1.0f), -89.0f, 89.0f);
                        }
                    }
                    const auto now = std::chrono::steady_clock::now();
                    // Frame times arrive unevenly even at a steady refresh rate (swapchain images
                    // are handed back in bursts); step the game by the average of the last few so
                    // motion stays smooth, which keeps the total time the same.
                    const float raw_dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
                    last = now;
                    static std::array<float, 8> recent{};
                    static size_t recent_n = 0;
                    recent[recent_n++ % recent.size()] = raw_dt;
                    float dt = 0;
                    for (size_t k = 0; k < std::min(recent_n, recent.size()); ++k) dt += recent[k];
                    dt /= float(std::min(recent_n, recent.size()));
                    main_menu.unlocked = settings.chapters, main_menu.hard_unlocked = settings.hard_unlocked;
                    if (!main_menu.open) main_menu.difficulty = settings.difficulty;
                    if (main_menu.open) {   // the main menu (with Options over it), the game paused behind
                        if (logic.audio) logic.audio->set_paused(true);
                        int mw = 0, mh = 0;
                        SDL_GetWindowSizeInPixels(window, &mw, &mh);
                        FrameParams mf = draw_main_menu(mw, mh);
                        if (menu.open) menu.draw(loading_overlay, loading_sprites, hud, subtitles, objective_text, {}, settings, mw, mh);
                        renderer.render_to_window({}, mf);
                        continue;
                    }
                    if (loading.active) {   // the level waits behind the loading screen
                        loading.update(raw_dt);
                        if (logic.audio) logic.audio->set_paused(true);
                        int lw = 0, lh = 0;
                        SDL_GetWindowSizeInPixels(window, &lw, &lh);
                        renderer.render_to_window({}, draw_loading(lw, lh));
                        continue;
                    }
                    // Dead: after a moment, the death screen (the game goes on behind it).
                    if (combat.player_dead && !death.open && combat.dead_time() > 1.0f) {
                        death.show();
                        SDL_SetWindowRelativeMouseMode(window, false);
                        fire_held = aim_held = false;
                    }
                    static bool end_was_open = false;   // just opened: free the mouse
                    if (end_screen.open && !end_was_open) SDL_SetWindowRelativeMouseMode(window, false), fire_held = aim_held = false;
                    end_was_open = end_screen.open;
                    if (end_screen.open && !menu.open) end_screen.update(raw_dt);
                    if (menu.open) menu.update(raw_dt);
                    if (death.open && !menu.open) {
                        death.update(raw_dt);
                        if (death.music_due() && logic.audio) logic.audio->play("m_mission_failed");
                    }
                    if (menu.open) dt = 0;   // paused
                    if (opt.scripts) {   // bullet time: the game runs slower for a while of real time
                        logic.advance_real_time(dt);
                        dt *= logic.time_scale();
                    }
                    static int frames_counted = 0;
                    static float frames_time = 0, worst_frame = 0, fps_shown = 0, worst_shown = 0;
                    ++frames_counted;
                    worst_frame = std::max(worst_frame, raw_dt);
                    if ((frames_time += raw_dt) >= 1.0f) {
                        fps_shown = float(frames_counted) / frames_time;
                        worst_shown = worst_frame * 1000.0f;
                        frames_counted = 0, frames_time = 0, worst_frame = 0;
                    }
                    const bool* keys = SDL_GetKeyboardState(nullptr);
                    const TouchControls::Output touch_in = touch_mode ? touch.take() : TouchControls::Output{};
                    // Piloting the mech: its cockpit is high up and it walks slowly.
                    const bool mech = opt.scripts && logic.in_mech();
                    player.eye_height = mech ? 7.4f : 0.75f;
                    player.walk_speed = mech ? 3.5f : 5.0f, player.run_speed = mech ? 3.5f : 9.0f;
                    // Aim mode: the view narrows (right mouse button held, or the aim button).
                    if (touch_in.pause) set_menu(true);
                    const bool can_aim = opt.scripts && !flying && !logic.player_locked() && !combat.player_dead && logic.controls.aim;
                    if (!can_aim) aim_toggled = false;   // a toggled aim ends with cinematics and deaths
                    const bool aiming = can_aim && (touch_mode ? touch_in.aim : settings.aim_toggle ? aim_toggled : aim_held);
                    {
                        const float base = opt.scripts ? logic.game_fov() : kFov;
                        const float target = aiming ? base * kAimZoom : base;
                        cam.fov += (target - cam.fov) * std::min(1.0f, dt * 12.0f);
                    }
                    if (opt.scripts && (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_D] ||
                                        touch_in.forward != 0 || touch_in.right != 0))
                        logic.qte_press("move");
                    if (flying) {
                        const float speed = (keys[SDL_SCANCODE_LSHIFT] ? 25.0f : 6.0f) * dt;
                        const glm::vec3 fwd = cam.forward();
                        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
                        if (keys[SDL_SCANCODE_W]) cam.position += fwd * speed;
                        if (keys[SDL_SCANCODE_S]) cam.position -= fwd * speed;
                        if (keys[SDL_SCANCODE_D]) cam.position += right * speed;
                        if (keys[SDL_SCANCODE_A]) cam.position -= right * speed;
                        if (keys[SDL_SCANCODE_SPACE]) cam.position.z += speed;
                        if (keys[SDL_SCANCODE_LCTRL]) cam.position.z -= speed;
                        step_logic(dt);
                    } else {
                        PlayerInput in;
                        const bool locked = opt.scripts && logic.player_locked();   // cinematics
                        if (!locked) {
                            const LevelLogic::Controls& lc = logic.controls;   // all on without scripts
                            if (lc.move) {   // SetMoveSensitivity scales walking
                                in.forward = glm::clamp(float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S]) + touch_in.forward, -1.0f, 1.0f);
                                in.right = glm::clamp(float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]) + touch_in.right, -1.0f, 1.0f);
                                in.forward *= logic.move_sensitivity, in.right *= logic.move_sensitivity;
                            }
                            in.jump = lc.jump && !mech && (keys[SDL_SCANCODE_SPACE] || touch_in.jump);
                            in.run = lc.rush && logic.player_rush && !aiming && (keys[SDL_SCANCODE_LSHIFT] || touch_in.run);
                            // Auto-leveling (touch): walking without turning, the view comes back level.
                            if (touch_mode && settings.auto_leveling && touch_in.look == glm::vec2(0) && (in.forward != 0 || in.right != 0))
                                cam.pitch -= std::clamp(cam.pitch, -40.0f * dt, 40.0f * dt);
                            if (lc.rotate) {
                                const float k = cam.fov / kFov * settings.sensitivity * logic.look_sensitivity;
                                cam.yaw += touch_in.look.x * k;
                                cam.pitch = glm::clamp(cam.pitch + touch_in.look.y * k * (settings.invert_y ? -1.0f : 1.0f), -89.0f, 89.0f);
                            }
                        }
                        if (float ly, lp; opt.scripts && logic.look_target(ly, lp)) cam.yaw = ly, cam.pitch = lp;   // PlayerLookTo
                        // Riding a vehicle or manning a gun: no walking, the view within its limits.
                        glm::vec3 seat_eye;
                        float yaw_min = 0, yaw_max = 0;
                        const bool seated = opt.scripts && logic.seat(seat_eye, yaw_min, yaw_max);
                        if (seated) {
                            in = PlayerInput{};
                            const float rel = std::fmod(std::fmod(cam.yaw - yaw_min, 360.0f) + 360.0f, 360.0f);
                            if (rel > yaw_max - yaw_min)   // outside: to the nearer limit
                                cam.yaw = rel - (yaw_max - yaw_min) < 360.0f - rel ? yaw_max : yaw_min;
                            cam.pitch = glm::clamp(cam.pitch, -30.0f, 35.0f);
                        }
                        if (opt.scripts) logic.limited_view_input(cam.yaw, cam.pitch, dt);   // limited control: within its limits
                        player.yaw = cam.yaw;
                        player.pitch = cam.pitch;
                        for (accumulator += dt; accumulator >= kPhysicsStep; accumulator -= kPhysicsStep) {
                            // During cinematics the player is out of play (the intro's spawn
                            // point may float in the void).
                            if (!(opt.scripts && logic.cinematic_active()) && !seated && !(opt.scripts && logic.limited_step(in.right, kPhysicsStep)))
                                player.update(scene.collision(), in, kPhysicsStep);
                            step_logic(kPhysicsStep);
                        }
                        if (seated && logic.seat(seat_eye, yaw_min, yaw_max)) player.center = seat_eye - glm::vec3(0, 0, player.eye_height);
                        if (player.center.z < -2000) player.spawn(scene.start_position, scene.start_yaw);   // fell out
                        if (opt.scripts) {
                            if (touch_mode) {
                                combat_in.fire = !locked && !logic.limited_active() && touch_in.fire;
                                if (touch_in.fire) logic.limited_tap();
                                combat_in.reload |= touch_in.reload;
                                if (touch_in.fire) logic.qte_press("fire");
                                if (touch_in.power && !logic.qte_press("repel") && logic.controls.powers) combat_in.power = true;
                                if (touch_in.power_switch && logic.controls.powers) combat.cycle_power();
                                combat_in.grenade |= touch_in.grenade && logic.controls.items;
                                if (touch_in.weapon_step != 0 && logic.controls.weapons) combat_in.select = combat.cycle_weapon(touch_in.weapon_step);
                            } else {
                                combat_in.fire = !locked && !logic.limited_active() && (fire_held || (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0);
                            }
                            combat.aim_assist = settings.aim_assist, combat.auto_switch = settings.auto_switch;
                            combat.update(dt, combat_in, player.eye(), cam.forward());
                            combat.push_player_out();
                            combat_in = {};
                        }
                        cam.position = player.eye();
                        if (player.spawns != view_spawns) {   // spawned or teleported: face its way
                            view_spawns = player.spawns;
                            cam.yaw = player.yaw, cam.pitch = player.pitch;
                        }
                        static float log_timer = 0;
                        if (opt.log_player && (log_timer += dt) >= 1.0f) {
                            log_timer = 0;
                            std::printf("player (%.2f %.2f %.2f) yaw %.0f %s, input fwd %.0f right %.0f fire %d, zone %d, "
                                        "health %.0f%s, shooting %s, ammo %d/%d, enemies %d, %.0f fps (worst %.1f ms), %zu sounds\n",
                                        player.center.x, player.center.y, player.center.z, player.yaw,
                                        player.on_ground ? "ground" : "air", in.forward, in.right, int(fire_held),
                                        scene.zones().current(), combat.player_health, combat.player_dead ? " (dead)" : "",
                                        combat.shoot_enabled ? "on" : "off", combat.ammo_in_clip(), combat.ammo_reserve(),
                                        combat.enemies_alive(), fps_shown, worst_shown, logic.audio ? logic.audio->voice_count() : size_t(0));
                            std::fflush(stdout);
                        }
                    }
                    scene.update(dt, cam.position);
                    if (opt.scripts) {
                        ViewModel::Input vm;
                        vm.weapon = combat.weapon_type();
                        vm.visible = !flying && !combat.player_dead && !logic.player_locked() && !logic.limited_active() && (!combat.special_weapon() || mech);
                        vm.cockpit = mech;
                        if (!logic.weapon_state.empty()) viewmodel.set_state(logic.weapon_state), logic.weapon_state.clear();
                        if (combat.take_grenade_throw()) viewmodel.set_state("ThrowGrenade");
                        if (combat.take_power_used()) viewmodel.set_state("Repel");
                        update_frost(dt);
                        vm.aiming = aiming;
                        vm.running = logic.controls.rush && (keys[SDL_SCANCODE_LSHIFT] || touch_in.run);
                        const bool* k = SDL_GetKeyboardState(nullptr);
                        vm.moving = player.on_ground && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_A] || k[SDL_SCANCODE_D] ||
                                                         touch_in.forward != 0 || touch_in.right != 0);
                        vm.firing = combat.since_shot() < 0.2f;
                        vm.reloading = combat.reloading();
                        Camera v = cam;
                        v.yaw += logic.shake_offset().x, v.pitch += logic.shake_offset().y;
                        viewmodel.update(dt, v.position, v.yaw, v.pitch, vm);
                    }
                    if (logic.audio) logic.audio->set_listener(cam.position, cam.forward(), glm::vec3(0, 0, 1));
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    clock += dt;
                    Camera view = cam;
                    if (opt.scripts) view.yaw += logic.shake_offset().x, view.pitch += logic.shake_offset().y;
                    Scene::CameraView cv;
                    const bool cinematic = opt.scripts && logic.cinematic_view(cv);
                    if (cinematic) {
                        view.position = cv.position, view.directed = true, view.direction = cv.forward, view.up = cv.up;
                        view.fov = glm::degrees(cv.fov), view.game_fov = false;
                    } else if (opt.scripts && !flying && logic.limited_view(cv)) {   // the limited-control rig's camera
                        view.position = cv.position, view.directed = true, view.direction = cv.forward, view.up = cv.up;
                    }
                    FrameParams f = frame(view, w, h);
                    scene.cull(f.camera, f.proj * f.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
                    overlay.clear();
                    if (opt.scripts && !flying && !logic.player_locked()) combat.draw(overlay, f.camera, w, h);
                    if (opt.scripts && logic.fade_alpha() > 0) fullscreen(overlay, w, h, glm::vec4(logic.fade_color, logic.fade_alpha()));
                    f.overlay = &overlay;
                    draw_text(f, w, h);
                    renderer.render_to_window(scene.draws(), f);
                    // The frame-rate limit (Display settings): wait out the rest of the frame.
                    if (settings.fps_limit > 0) {
                        const auto frame_end = last + std::chrono::nanoseconds(1000000000LL / settings.fps_limit);
                        const auto left = frame_end - std::chrono::steady_clock::now();
                        if (left.count() > 0) SDL_DelayPrecise(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count()));
                    }
                }
            }
        }
        if (window) SDL_DestroyWindow(window);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        rc = 1;
    }
    SDL_Quit();
    return rc;
}
