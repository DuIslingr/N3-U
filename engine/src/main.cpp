// nova3: level viewer for the N.O.V.A. 3 reimplementation.
//
//   nova3 [--data DIR] [--level NAME.lvc]                       walk through a level
//   nova3 ... --screenshot OUT.bmp [--eye X,Y,Z,YAW[,PITCH]] [--size WxH]   render one frame, no window
//   nova3 ... --walk-test SECONDS [--screenshot OUT.bmp]          simulate walking forward from the start
//   nova3 ... --focus OBJECT[#N] [--time T]                        start looking at the Nth placement of a type
//
// Controls: mouse to look, WASD to move, Shift to run, Space to jump, F to toggle flying
// (Space/Ctrl then move up/down), Esc to quit.
// The data directory is the game's own files folder (the .gla archives).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glm/gtc/matrix_transform.hpp>

#include "core/vfs.h"
#include "game/player.h"
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
            const SceneStats& st = scene.stats();
            std::printf("%s: %zu placements, %zu models (%zu missing), %zu textures (%zu missing), %zu draws, "
                        "%zu collision objects (%zu triangles, %zu missing), %zu animated (%zu animations missing), "
                        "%zu draws hidden by LOD, loaded in %.2fs\n",
                        opt.level.c_str(), st.placements, st.models, st.missing_models, st.textures,
                        st.missing_textures, st.draws, st.collision_objects, st.collision_triangles,
                        st.missing_collision, st.animated, st.missing_animations, st.lod_hidden, st.load_seconds);
            Player player;
            player.spawn(scene.start_position, scene.start_yaw);
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
                scene.update(opt.time, cam.position);
                const Image img = renderer.render_to_image(scene.draws(), cam.frame(opt.width, opt.height), opt.width, opt.height);
                if (!save_bmp(img, opt.screenshot)) throw std::runtime_error("cannot write " + opt.screenshot);
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
                    } else {
                        PlayerInput in;
                        in.forward = float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S]);
                        in.right = float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
                        in.jump = keys[SDL_SCANCODE_SPACE];
                        in.run = keys[SDL_SCANCODE_LSHIFT];
                        player.yaw = cam.yaw;
                        for (accumulator += dt; accumulator >= kPhysicsStep; accumulator -= kPhysicsStep) {
                            player.update(scene.collision(), in, kPhysicsStep);
                        }
                        if (player.center.z < -2000) player.spawn(scene.start_position, scene.start_yaw);   // fell out
                        cam.position = player.eye();
                    }
                    scene.update(dt, cam.position);
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    renderer.render_to_window(scene.draws(), cam.frame(w, h));
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
