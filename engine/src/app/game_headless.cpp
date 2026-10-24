// The windowless runs: screenshots after a stretch of level logic (with the tests that
// drive it), the walk test and the zone culling audit.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>

#include <SDL3/SDL.h>

#include "app/game.h"
#include "app/output.h"
#include "game/navmesh.h"

namespace nova {

int Game::run_audit() {
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

// Settle for a second, then walk forward; report the path.
void Game::run_walk_test() {
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

// The screenshot's frame from `base`: the cinematic or limited-control camera while one is
// on, with the overlay, HUD and text over it.
FrameParams Game::shot_frame(Camera base) {
    if (Scene::CameraView cv; opt.scripts && !opt.have_eye && logic.cinematic_view(cv)) {   // --eye wins
        base.position = cv.position, base.directed = true, base.direction = cv.forward, base.up = cv.up;
        base.fov = glm::degrees(cv.fov), base.game_fov = false;
    } else if (opt.scripts && !opt.have_eye && logic.limited_view(cv)) {   // the limited-control rig's camera
        base.position = cv.position, base.directed = true, base.direction = cv.forward, base.up = cv.up;
    } else if (opt.scripts && !opt.have_eye) {
        base.jolt(viewmodel.camera_offset());
    }
    FrameParams shot = frame(base, opt.width, opt.height);
    overlay.clear();
    if (opt.scripts && !logic.player_locked()) combat.draw(overlay, shot.camera, opt.width, opt.height);
    if (opt.scripts && logic.fade_alpha() > 0) fullscreen(overlay, opt.width, opt.height, glm::vec4(logic.fade_color, logic.fade_alpha()));
    shot.overlay = &overlay;
    draw_text(shot, opt.width, opt.height);
    scene.cull(shot.camera, shot.proj * shot.view, opt.scripts ? logic.cinematic_zone() : ZoneSystem::kNone);
    return shot;
}

int Game::run_screenshot() {
    float report = 0;
    int shots = 0;   // --every: frames saved along the run
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
        if (opt.respawn_at >= 0 && t <= opt.respawn_at && t + kPhysicsStep > opt.respawn_at && !opt.spawn_at.empty()) spawn_at_option();
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
                // Held, or pressed and let go for a semi-automatic weapon.
                ci.fire = combat.weapon().automatic || int(t / 0.1f) % 2 == 0;
            }
            fire_step(kPhysicsStep, ci, player.eye(), aim, false);
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
        if (opt.scripts) combat.place_attachments();
        if (opt.scripts) {
            ViewModel::Input vm;
            vm.weapon = combat.weapon_type();
            vm.visible = !combat.player_dead && !logic.player_locked() && !logic.limited_active() && (!combat.special_weapon() || logic.in_mech());
            vm.cockpit = logic.in_mech();
            if (!logic.weapon_state.empty()) viewmodel.set_state(logic.weapon_state), logic.weapon_state.clear();
            arms_distort();
            if (const std::string h = combat.take_view_hurt(); !h.empty()) viewmodel.camera_state(h);
            movement_states(false);
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
        if (opt.every > 0 && t + kPhysicsStep >= float(shots) * opt.every) {   // OUT_000.bmp, OUT_001.bmp, ...
            const FrameParams sf = shot_frame(eye);
            const Image img = renderer.render_to_image(scene.draws(), sf, opt.width, opt.height);
            std::string path = opt.screenshot;
            char n[16];
            std::snprintf(n, sizeof n, "_%03d", shots++);
            const size_t dot = path.rfind('.');
            path.insert(dot == std::string::npos ? path.size() : dot, n);
            if (!save_bmp(img, path)) throw std::runtime_error("cannot write " + path);
            std::printf("t=%.2f %s: eye (%.1f %.1f %.1f) yaw %.0f pitch %.0f\n", t, path.c_str(), sf.camera.x, sf.camera.y,
                        sf.camera.z, player.yaw, player.pitch);
        }
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
    if (opt.scripts) combat.place_attachments();
    if (opt.scripts) {
        ViewModel::Input vm;
        vm.weapon = combat.weapon_type();
        vm.visible = !combat.player_dead && !logic.player_locked() && !logic.limited_active() && (!combat.special_weapon() || logic.in_mech());
        vm.cockpit = logic.in_mech();
        if (!logic.weapon_state.empty()) viewmodel.set_state(logic.weapon_state), logic.weapon_state.clear();
        arms_distort();
        vm.firing = combat.since_shot() < 0.2f;
        vm.reloading = combat.reloading();
        viewmodel.update(1.0f / 60.0f, cam.position, cam.yaw, cam.pitch, vm);
    }
    bool scene_hidden = false;
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
    FrameParams shot = shot_frame(cam);
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
        main_menu.unlocked = std::max(1, int(campaign.size()) / 2), main_menu.credits = settings.credits;
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
    return 0;
}

}  // namespace nova
