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

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/game.h"
#include "app/options.h"
#include "audio/audio.h"
#include "core/vfs.h"

#ifdef _WIN32   // last: it defines near, far, min, max...
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace nova;

int main(int argc, char** argv) {
#ifdef _WIN32
    // A windowed program: started from a console, it prints there all the same.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    Options opt;
    try {
        opt = parse_options(argc, argv);
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
        // A packaged game: the data folder beside the executable.
        if (!opt.data_given && !std::filesystem::is_directory(opt.data)) {
            const std::filesystem::path beside = std::filesystem::path(SDL_GetBasePath()) / "data";
            if (std::filesystem::is_directory(beside)) opt.data = beside.string();
        }
        // Without the Android data, the iOS build's on its own (work/ios/ipa/Payload/nova3.app).
        if (!std::filesystem::is_directory(opt.data)) {
            const std::filesystem::path ios = std::filesystem::path(opt.data).lexically_normal().parent_path() / "ios" / "ipa" / "Payload" / "nova3.app";
            if (std::filesystem::is_directory(ios)) opt.data = ios.string();
        }
        if (!std::filesystem::is_directory(opt.data) || vfs.mount_directory(opt.data) == 0)
            throw std::runtime_error("The game's data (its .gla archives) was not found in " + opt.data +
                                     ".\nPut the files of your copy of N.O.V.A. 3 in a folder named data beside the game, or "
                                     "start it with --data FOLDER.");
        std::vector<std::string> extra = opt.extra_data;
        if (extra.empty()) {
            const std::filesystem::path ios = std::filesystem::path(opt.data) / ".." / "ios" / "ipa" / "Payload" / "nova3.app";
            if (std::filesystem::is_directory(ios)) extra.push_back(ios.string());
        }
        for (const std::string& dir : extra)
            if (dir == "none") break;   // --extra-data none: the main data alone
            else if (const size_t n = vfs.mount_directory(dir)) std::printf("mounted %zu archives from %s\n", n, dir.c_str());
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
            Game game(opt, vfs, gol, window, shader_dir);
            rc = game.run();
        }
        if (window) SDL_DestroyWindow(window);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        // Started from a desktop there may be no console to read it in.
        if (opt.screenshot.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "N.O.V.A. 3", ex.what(), nullptr);
        rc = 1;
    }
    SDL_Quit();
    return rc;
}
