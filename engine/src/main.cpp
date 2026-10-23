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

#include "core/vfs.h"
#include "game/combat.h"
#include "game/navmesh.h"
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
    bool log_player = false;    // print the player's state every second (interactive)
    float combat_test = 0;      // headless: aim at the nearest enemy and fire for this many seconds
    bool god = false;           // the player takes no damage
    bool list_actors = false;   // print combat targets after the level starts
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
        else if (a == "--no-zones") o.zones = false;
        else if (a == "--section") o.section = std::stoi(next());
        else if (a == "--trace-zones") o.trace_zones = true;
        else if (a == "--log-player") o.log_player = true;
        else if (a == "--combat-test") o.combat_test = std::stof(next());
        else if (a == "--god") o.god = true;
        else if (a == "--list-actors") o.list_actors = true;
        else if (a == "--audit") o.audit = std::stoi(next());
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
        // Reversed depth (near -> 1, far -> 0) spreads the float depth buffer's precision
        // evenly; near and far are swapped on purpose.
        f.proj = glm::perspectiveRH_ZO(glm::radians(65.0f), float(w) / float(std::max(h, 1)), 8000.0f, 0.05f);
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
            scene.load_level(opt.level, 1u << (std::max(opt.section, 1) - 1));
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
            std::printf("zones: %zu zones, %zu portals\n", scene.zones().zones().size(), scene.zones().portals().size());
            Combat combat(scene, player, logic, gol);
            combat.log = [](const std::string& line) { std::printf("[combat] %s\n", line.c_str()); };
            combat.player_invulnerable = opt.god;
            logic.combat = &combat;
            Overlay overlay;
            if (opt.scripts) {
                logic.start();
                const auto& ls = logic.stats();
                std::printf("scripts: %zu chunks (%zu with errors), %zu objects, %zu triggers, %zu doors, %zu usable\n",
                            ls.chunks, ls.chunk_errors, ls.objects, ls.triggers, ls.doors, ls.interactive);
                combat.start();
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
            // Level logic at a fixed step, so triggers and doors see every position.
            auto step_logic = [&](float dt) {
                if (!opt.scripts) return;
                logic.update(dt);
                if (logic.pending_section) {   // LoadLevelSections: reload, keep the player
                    const uint32_t section = logic.pending_section;
                    const glm::vec3 feet = player.center - glm::vec3(0, 0, player.radius.z);
                    const float yaw = player.yaw;
                    std::printf("[script] loading level section mask %u\n", section);
                    scene.load_level(opt.level, section);
                    player.spawn(feet, yaw);
                    logic.start(true);
                    combat.start();
                }
            };
            constexpr float kPhysicsStep = 1.0f / 120.0f;

            Camera cam;
            cam.position = opt.have_eye ? glm::vec3(opt.eye) : player.eye();
            cam.yaw = opt.have_eye ? opt.eye.w : scene.start_yaw;
            cam.pitch = opt.pitch;
            if (!opt.focus.empty() && !focus_camera(scene, gol, opt.focus, cam.position, cam.yaw, cam.pitch))
                throw std::runtime_error("no placement matching " + opt.focus);

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
                            scene.cull(f.camera, f.proj * f.view);
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
                            scene.cull(f.camera, f.proj * f.view);
                            const Image off = renderer.render_to_image(scene.draws(), f, w, h);
                            scene.zones().enabled = true;
                            scene.cull(f.camera, f.proj * f.view);
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
                        scene.cull(wf.camera, wf.proj * wf.view);
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
                for (float t = 0; t < run_time; t += kPhysicsStep) {   // stand still and let the scripts run
                    player.update(scene.collision(), PlayerInput{}, kPhysicsStep);
                    step_logic(kPhysicsStep);
                    if (opt.scripts) {
                        CombatInput ci;
                        glm::vec3 aim = player.view_forward(), target;
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
                    Camera eye = cam;
                    if (!opt.have_eye && opt.focus.empty()) eye.position = player.eye(), eye.yaw = player.yaw;
                    const FrameParams rf = frame(eye, opt.width, opt.height);
                    scene.cull(rf.camera, rf.proj * rf.view);
                }
                if (opt.list_actors && opt.scripts)
                    for (const std::string& line : combat.describe())
                        if (line.find(" enabled") != std::string::npos) std::printf("  end: %s\n", line.c_str());
                if (run_time > 0 && !opt.have_eye && opt.focus.empty()) {
                    cam.position = player.eye();
                    cam.yaw = player.yaw;
                    if (opt.combat_test > 0) cam.pitch = player.pitch;
                }
                scene.update(opt.time, cam.position);
                FrameParams shot = frame(cam, opt.width, opt.height);
                overlay.clear();
                if (opt.scripts) combat.draw(overlay, shot.camera, opt.width, opt.height);
                shot.overlay = &overlay;
                scene.cull(shot.camera, shot.proj * shot.view);
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
                const Image img = renderer.render_to_image(scene.draws(), shot, opt.width, opt.height);
                if (!save_bmp(img, opt.screenshot)) throw std::runtime_error("cannot write " + opt.screenshot);
                if (opt.bench > 0) {
                    const auto t0 = std::chrono::steady_clock::now();
                    for (int i = 0; i < opt.bench; ++i) {
                        scene.update(1.0f / 60.0f, cam.position);
                        const FrameParams bf = frame(cam, opt.width, opt.height);
                        scene.cull(bf.camera, bf.proj * bf.view);
                        renderer.render_to_image(scene.draws(), bf, opt.width, opt.height);
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    std::printf("bench: %.2f ms/frame (update + render + readback)\n", ms / opt.bench);
                }
                std::printf("wrote %s\n", opt.screenshot.c_str());
            } else {
                bool flying = false;
                float accumulator = 0;
                CombatInput combat_in;
                bool fire_held = false;
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
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_Z && !e.key.repeat) scene.zones().enabled = !scene.zones().enabled;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_R && !e.key.repeat) combat_in.reload = true;
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) fire_held = true;
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) fire_held = false;
                        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) fire_held = false;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_1) combat_in.select = 0;
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_2) combat_in.select = 1;
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
                        if (opt.scripts) {
                            combat_in.fire = fire_held || (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
                            combat.update(dt, combat_in, player.eye(), cam.forward());
                            combat.push_player_out();
                            combat_in = {};
                        }
                        cam.position = player.eye();
                        static float log_timer = 0;
                        if (opt.log_player && (log_timer += dt) >= 1.0f) {
                            log_timer = 0;
                            std::printf("player (%.2f %.2f %.2f) yaw %.0f %s, input fwd %.0f right %.0f fire %d, zone %d, "
                                        "health %.0f%s, shooting %s, ammo %d/%d, enemies %d, %.0f fps\n",
                                        player.center.x, player.center.y, player.center.z, player.yaw,
                                        player.on_ground ? "ground" : "air", in.forward, in.right, int(fire_held),
                                        scene.zones().current(), combat.player_health, combat.player_dead ? " (dead)" : "",
                                        combat.shoot_enabled ? "on" : "off", combat.ammo_in_clip(), combat.ammo_reserve(),
                                        combat.enemies_alive(), dt > 0 ? 1.0f / dt : 0.0f);
                            std::fflush(stdout);
                        }
                    }
                    scene.update(dt, cam.position);
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    clock += dt;
                    FrameParams f = frame(cam, w, h);
                    scene.cull(f.camera, f.proj * f.view);
                    overlay.clear();
                    if (opt.scripts && !flying) combat.draw(overlay, f.camera, w, h);
                    f.overlay = &overlay;
                    renderer.render_to_window(scene.draws(), f);
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
