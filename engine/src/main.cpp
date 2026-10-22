// nova3: level viewer for the N.O.V.A. 3 reimplementation.
//
//   nova3 [--data DIR] [--level NAME.lvc]                       walk through a level
//   nova3 ... --screenshot OUT.bmp [--eye X,Y,Z,YAW[,PITCH]] [--size WxH]   render one frame, no window
//   nova3 ... --walk-test SECONDS [--screenshot OUT.bmp]          simulate walking forward from the start
//   nova3 ... --focus OBJECT[#N] [--time T]                        start looking at the Nth placement of a type
//
//   nova3 ... --builtin-shaders                                  draw with the fallback shader, not the game's
//   nova3 ... --screenshot OUT.bmp --bench N                      also time N offscreen frames
//   nova3 ... --skybox ID                                          start with another of the level's skyboxes
//   nova3 ... --no-scripts                                         do not run the level scripts
//   nova3 ... --trace-scripts                                      log every script function that starts
//   nova3 ... --spawn-at ID|X,Y,Z,YAW                              start at a level object, spawn point, waypoint or position
//   nova3 ... --exec "LUA"                                         run a script snippet once the level has started
//   nova3 ... --hide-models SUBSTR                                 (debugging) hide placements whose model matches
//   nova3 ... --screenshot OUT.bmp --run SECONDS                    run the level logic (standing still) first
//
// Controls: mouse to look, WASD to move, Shift to run, Space to jump, F to toggle flying
// (Space/Ctrl then move up/down), E to use, G to switch game/built-in shaders, Esc to quit.
// The data directory is the game's own files folder (the .gla archives).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glm/gtc/matrix_transform.hpp>

#include "core/vfs.h"
#include "game/player.h"
#include "game/level_logic.h"
#include "game/scene.h"
#include "render/renderer.h"

using namespace nova;

namespace {

struct Options {
    std::string data = "work/data";
    std::string level = "Ship_01.lvc";
    std::string screenshot;
    float walk_test = 0;
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
    int width = 1600, height = 900;
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
        else if (a == "--level") o.level = next();
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--walk-test") o.walk_test = std::stof(next());
        else if (a == "--time") o.time = std::stof(next());
        else if (a == "--focus") o.focus = next();
        else if (a == "--builtin-shaders") o.builtin_shaders = true;
        else if (a == "--bench") o.bench = std::stoi(next());
        else if (a == "--skybox") o.skybox = std::stoi(next());
        else if (a == "--no-scripts") o.scripts = false;
        else if (a == "--trace-scripts") o.trace_scripts = true;
        else if (a == "--run") o.run = std::stof(next());
        else if (a == "--spawn-at") o.spawn_at = next();
        else if (a == "--exec") o.exec.push_back(next());
        else if (a == "--hide-models") o.hide_models.push_back(next());
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &o.width, &o.height);
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

struct Camera {
    glm::vec3 position{0};
    float yaw = 0, pitch = 0;   // degrees; yaw 0 looks along +Y, positive yaw turns left (game convention)

    glm::vec3 forward() const {
        const float y = glm::radians(yaw), p = glm::radians(pitch);
        return {-std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p)};
    }
    FrameParams frame(int w, int h) const {
        FrameParams f;
        f.view = glm::lookAt(position, position + forward(), {0, 0, 1});
        f.proj = glm::perspectiveRH_ZO(glm::radians(65.0f), float(w) / float(std::max(h, 1)), 0.05f, 8000.0f);
        f.camera = position;
        return f;
    }
};

bool save_bmp(const Image& img, const std::string& path) {
    SDL_Surface* s = SDL_CreateSurfaceFrom(img.width, img.height, SDL_PIXELFORMAT_ABGR8888,
                                           const_cast<uint8_t*>(img.rgba.data()), img.width * 4);
    const bool ok = s && SDL_SaveBMP(s, path.c_str());
    SDL_DestroySurface(s);
    return ok;
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
            scene.load_level(opt.level);
            if (opt.builtin_shaders) scene.set_game_shaders(false);
            if (opt.skybox >= 0 && !scene.set_skybox(opt.skybox)) throw std::runtime_error("no skybox with that id");
            const SceneStats& st = scene.stats();
            const MaterialStats& ms = scene.material_stats();
            std::printf("materials: %zu with game shaders, %zu fallback; %zu programs (%zu failed), %zu effects missing; "
                        "%zu shader stages compiled, %zu from cache\n",
                        ms.materials, ms.fallback, ms.programs, ms.failed_programs, ms.missing_effects,
                        scene.shader_compiler().compiled, scene.shader_compiler().cache_hits);
            float clock = opt.time;
            auto frame = [&](const Camera& cam, int w, int h) {
                FrameParams f = cam.frame(w, h);
                f.fog_color = scene.fog_color;
                f.fog_start = scene.fog_start;
                f.fog_end = scene.fog_end;
                f.fog_density = scene.fog_density;
                f.time = clock;
                f.lights = scene.lights();
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
            logic.trace = opt.trace_scripts;
            logic.log = [&opt](const std::string& line) {
                if (line.rfind("print: ", 0) == 0 && !opt.trace_scripts) return;   // scripts print a lot
                std::printf("[script] %s\n", line.c_str());
            };
            if (opt.scripts) {
                logic.start();
                const auto& ls = logic.stats();
                std::printf("scripts: %zu chunks (%zu with errors), %zu objects, %zu triggers, %zu doors, %zu usable\n",
                            ls.chunks, ls.chunk_errors, ls.objects, ls.triggers, ls.doors, ls.interactive);
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
            // Level logic at a fixed step, so triggers and doors see every position.
            auto step_logic = [&](float dt) {
                if (opt.scripts) logic.update(dt);
            };
            constexpr float kPhysicsStep = 1.0f / 120.0f;

            Camera cam;
            cam.position = opt.have_eye ? glm::vec3(opt.eye) : player.eye();
            cam.yaw = opt.have_eye ? opt.eye.w : scene.start_yaw;
            cam.pitch = opt.pitch;
            if (!opt.focus.empty() && !focus_camera(scene, gol, opt.focus, cam.position, cam.yaw, cam.pitch))
                throw std::runtime_error("no placement matching " + opt.focus);

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
                    travelled += glm::length(glm::vec2(player.center - last));
                    last = player.center;
                    if (i % 60 == 0) {
                        std::printf("t=%5.2fs  pos=(%8.2f %8.2f %7.2f)  %s\n", i * kPhysicsStep, player.center.x,
                                    player.center.y, player.center.z, player.on_ground ? "ground" : "air");
                    }
                }
                std::printf("walk test: start z %.2f, end z %.2f, horizontal distance %.2f m, %s\n", start.z,
                            player.center.z, travelled, player.on_ground ? "on ground" : "in the air");
                cam.position = player.eye();
                cam.yaw = player.yaw;
            }
            if (headless || opt.walk_test > 0) {
                if (opt.screenshot.empty()) return 0;
                for (float t = 0; t < opt.run; t += kPhysicsStep) {   // stand still and let the scripts run
                    player.update(scene.collision(), PlayerInput{}, kPhysicsStep);
                    step_logic(kPhysicsStep);
                    scene.update(kPhysicsStep, cam.position);
                }
                if (opt.run > 0 && !opt.have_eye && opt.focus.empty()) {
                    cam.position = player.eye();
                    cam.yaw = player.yaw;
                }
                scene.update(opt.time, cam.position);
                const Image img = renderer.render_to_image(scene.draws(), frame(cam, opt.width, opt.height), opt.width, opt.height);
                if (!save_bmp(img, opt.screenshot)) throw std::runtime_error("cannot write " + opt.screenshot);
                if (opt.bench > 0) {
                    const auto t0 = std::chrono::steady_clock::now();
                    for (int i = 0; i < opt.bench; ++i) {
                        scene.update(1.0f / 60.0f, cam.position);
                        renderer.render_to_image(scene.draws(), frame(cam, opt.width, opt.height), opt.width, opt.height);
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    std::printf("bench: %.2f ms/frame (update + render + readback)\n", ms / opt.bench);
                }
                std::printf("wrote %s\n", opt.screenshot.c_str());
            } else {
                bool flying = false;
                float accumulator = 0;
                SDL_SetWindowRelativeMouseMode(window, true);
                bool running = true;
                auto last = std::chrono::steady_clock::now();
                while (running) {
                    SDL_Event e;
                    while (SDL_PollEvent(&e)) {
                        if (e.type == SDL_EVENT_QUIT) running = false;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = false;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F && !e.key.repeat) {
                            flying = !flying;
                            if (!flying) player.spawn(cam.position - glm::vec3(0, 0, player.radius.z + player.eye_height), cam.yaw);
                        }
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_E && !e.key.repeat && opt.scripts) logic.use();
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_G && !e.key.repeat) {
                            scene.set_game_shaders(!scene.game_shaders());
                        }
                        if (e.type == SDL_EVENT_MOUSE_MOTION) {
                            cam.yaw -= e.motion.xrel * 0.12f;
                            cam.pitch = glm::clamp(cam.pitch - e.motion.yrel * 0.12f, -89.0f, 89.0f);
                        }
                    }
                    const auto now = std::chrono::steady_clock::now();
                    const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
                    last = now;
                    const bool* keys = SDL_GetKeyboardState(nullptr);
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
                        in.forward = float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S]);
                        in.right = float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
                        in.jump = keys[SDL_SCANCODE_SPACE];
                        in.run = keys[SDL_SCANCODE_LSHIFT];
                        player.yaw = cam.yaw;
                        for (accumulator += dt; accumulator >= kPhysicsStep; accumulator -= kPhysicsStep) {
                            player.update(scene.collision(), in, kPhysicsStep);
                            step_logic(kPhysicsStep);
                        }
                        if (player.center.z < -2000) player.spawn(scene.start_position, scene.start_yaw);   // fell out
                        cam.position = player.eye();
                    }
                    scene.update(dt, cam.position);
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    clock += dt;
                    renderer.render_to_window(scene.draws(), frame(cam, w, h));
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
