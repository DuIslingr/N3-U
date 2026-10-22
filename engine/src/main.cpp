// nova3: level viewer for the N.O.V.A. 3 reimplementation.
//
//   nova3 [--data DIR] [--level NAME.lvc]                       fly through a level
//   nova3 ... --screenshot OUT.bmp [--eye X,Y,Z,YAW[,PITCH]] [--size WxH]   render one frame, no window
//
// Controls: mouse to look, WASD to move, Space/Ctrl up/down, Shift faster, Esc to quit.
// The data directory is the game's own files folder (the .gla archives).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glm/gtc/matrix_transform.hpp>

#include "core/vfs.h"
#include "game/scene.h"
#include "render/renderer.h"

using namespace nova;

namespace {

struct Options {
    std::string data = "work/data";
    std::string level = "Ship_01.lvc";
    std::string screenshot;
    bool have_eye = false;
    glm::vec4 eye{0};        // x, y, z, yaw (degrees)
    float pitch = 0;
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
            std::printf("%s: %zu placements, %zu models (%zu missing), %zu textures (%zu missing), %zu draws, loaded in %.2fs\n",
                        opt.level.c_str(), st.placements, st.models, st.missing_models, st.textures,
                        st.missing_textures, st.draws, st.load_seconds);

            Camera cam;
            cam.position = opt.have_eye ? glm::vec3(opt.eye) : scene.start_position;
            cam.yaw = opt.have_eye ? opt.eye.w : scene.start_yaw;
            cam.pitch = opt.pitch;

            if (headless) {
                const Image img = renderer.render_to_image(scene.draws(), cam.frame(opt.width, opt.height), opt.width, opt.height);
                if (!save_bmp(img, opt.screenshot)) throw std::runtime_error("cannot write " + opt.screenshot);
                std::printf("wrote %s\n", opt.screenshot.c_str());
            } else {
                SDL_SetWindowRelativeMouseMode(window, true);
                bool running = true;
                auto last = std::chrono::steady_clock::now();
                while (running) {
                    SDL_Event e;
                    while (SDL_PollEvent(&e)) {
                        if (e.type == SDL_EVENT_QUIT) running = false;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = false;
                        if (e.type == SDL_EVENT_MOUSE_MOTION) {
                            cam.yaw -= e.motion.xrel * 0.12f;
                            cam.pitch = glm::clamp(cam.pitch - e.motion.yrel * 0.12f, -89.0f, 89.0f);
                        }
                    }
                    const auto now = std::chrono::steady_clock::now();
                    const float dt = std::chrono::duration<float>(now - last).count();
                    last = now;
                    const bool* keys = SDL_GetKeyboardState(nullptr);
                    const float speed = (keys[SDL_SCANCODE_LSHIFT] ? 25.0f : 6.0f) * dt;
                    const glm::vec3 fwd = cam.forward();
                    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
                    if (keys[SDL_SCANCODE_W]) cam.position += fwd * speed;
                    if (keys[SDL_SCANCODE_S]) cam.position -= fwd * speed;
                    if (keys[SDL_SCANCODE_D]) cam.position += right * speed;
                    if (keys[SDL_SCANCODE_A]) cam.position -= right * speed;
                    if (keys[SDL_SCANCODE_SPACE]) cam.position.z += speed;
                    if (keys[SDL_SCANCODE_LCTRL]) cam.position.z -= speed;
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
