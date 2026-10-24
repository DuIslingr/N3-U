// The interactive game: events, the fixed-step simulation, the view and the menus over it.
#include <array>
#include <chrono>
#include <cstdio>

#include <SDL3/SDL.h>

#include "app/game.h"

namespace nova {

glm::vec2 Game::to_pixels(float x, float y, bool normalized) const {
    int ww = 1, wh = 1, pw = 1, ph = 1;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    return normalized ? glm::vec2(x * float(pw), y * float(ph))
                      : glm::vec2(x * float(pw) / float(std::max(ww, 1)), y * float(ph) / float(std::max(wh, 1)));
}

// Display settings: the window's size or fullscreen mode, V-Sync.
void Game::apply_display() {
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
}

// The pause menu: the game stands still, the pointer is free, the sound stops.
void Game::set_menu(bool on) {
    if (on) menu.show();
    else menu.open = false;
    SDL_SetWindowRelativeMouseMode(window, !on && !touch_mode && !main_menu.open);
    fire_held = aim_held = false;
    touch.release_all();
    // GS_InGameMenu::PlayMusic / StopMusic: its own music (m_about), faded
    // in and out over 500 ms, while the game's sounds hold.
    if (logic.audio) {
        logic.audio->set_paused(on, "m_about");
        if (on && !logic.audio->playing("m_about")) {
            Audio::Params p;
            p.fade_in_ms = 500;
            logic.audio->play("m_about", p);
        }
        if (!on) logic.audio->stop("m_about", 500);
    }
}

void Game::menu_action(PauseMenu::Action a) {
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
}

void Game::end_action(EndLevelScreen::Action a) {
    if (a == EndLevelScreen::Action::None) return;
    end_screen.open = false;
    end_screen_done = true;   // the level's end goes on: the next level
    const std::string next = next_level(opt.level);
    for (size_t c = 0; c < campaign.size(); ++c)   // the next chapter is reached
        if (!next.empty() && next.find("/" + campaign[c].file + ".lvc") != std::string::npos && int(c) + 1 > settings.chapters)
            settings.chapters = int(c) + 1, settings.save(settings_path);
    if (a == EndLevelScreen::Action::Menu) {
        logic.level_ended = false, end_screen_done = false;
        main_menu.show();
    } else {
        SDL_SetWindowRelativeMouseMode(window, !touch_mode);
    }
}

void Game::death_action(DeathScreen::Action a) {
    if (a == DeathScreen::Action::None) return;
    death.open = false;
    combat.respawn();
    if (a == DeathScreen::Action::Menu) {
        main_menu.show();
        SDL_SetWindowRelativeMouseMode(window, false);
    } else {
        SDL_SetWindowRelativeMouseMode(window, !touch_mode);
    }
}

void Game::main_action(MainMenu::Action a) {
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
}

// The title music (Application::PlayTitleMusic, m_title.vxn): its "mmenu" state in the
// menus, "loading" on the loading screen (GS_Loading); the game's own sounds hold
// meanwhile. It fades once the level is under way.
void Game::title_music(const char* state) {
    if (!logic.audio) return;
    logic.audio->set_paused(true, "m_title");
    if (!logic.audio->playing("m_title")) logic.audio->play("m_title");
    logic.audio->set_state("m_title", state);
}

void Game::handle_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_QUIT) running = false;
    if (loading.active) {   // the loading screen takes every input; a press dismisses it
        const bool pressed = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                             e.type == SDL_EVENT_FINGER_DOWN;
        if (pressed && loading.press() && logic.audio) logic.audio->set_paused(menu.open);
        return;
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
        return;
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
        return;
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
        return;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !e.key.repeat) {
        if (menu.open) menu_action(menu.key(PauseMenu::Key::Back, settings));   // back a page, or resume
        else set_menu(true);
        return;
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
        return;
    }
    if (touch_mode) {
        constexpr int64_t kMouseFinger = -1;
        // A tap on the use button uses (CLevelInterfaceNormal's interact button).
        auto use_tap = [&](glm::vec2 at) {
            int pw = 1, ph = 1;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            const glm::vec4 r = use_button(pw, ph);
            if (!opt.scripts || !logic.use_available() || at.x < r.x || at.x > r.z || at.y < r.y || at.y > r.w) return false;
            use_pressed_ = 0.15f;
            logic.use();
            return true;
        };
        switch (e.type) {
            case SDL_EVENT_FINGER_DOWN:
                if (const glm::vec2 at = to_pixels(e.tfinger.x, e.tfinger.y, true); !use_tap(at)) touch.down(int64_t(e.tfinger.fingerID), at);
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
                    if (const glm::vec2 at = to_pixels(e.button.x, e.button.y, false); !use_tap(at)) touch.down(kMouseFinger, at);
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
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION) return;
    }
    if (e.type == SDL_EVENT_QUIT) running = false;
    // Debug: F1 free flight, F2 zone culling, F3 built-in shaders.
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F1 && !e.key.repeat) {
        flying = !flying;
        if (!flying) player.spawn(cam.position - glm::vec3(0, 0, player.radius.z + player.eye_height), cam.yaw);
    }
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_E && !e.key.repeat && opt.scripts && !logic.limited_tap()) {
        if (logic.use_available()) use_pressed_ = 0.15f;
        logic.use();
    }
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
    float widest = 0, narrowest = 0;
    if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0 && aiming_ && viewmodel.zoom_range(widest, narrowest))   // the zoom slider
        zoom_fov_ = std::clamp(zoom_fov_ - e.wheel.y * 2.5f, narrowest, widest);
    else if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0 && logic.controls.weapons)
        combat_in.select = combat.cycle_weapon(e.wheel.y < 0 ? 1 : -1);
    if (e.type == SDL_EVENT_MOUSE_MOTION && !(opt.scripts && (logic.player_locked() || logic.interacting() || !logic.controls.rotate))) {
        // Slower while aiming (zoomed in); the Sensitivity and Invert Y-Axis settings.
        const float k = 0.12f * cam.fov / kFov * settings.sensitivity * logic.look_sensitivity;
        cam.yaw -= e.motion.xrel * k;
        cam.pitch = glm::clamp(cam.pitch - e.motion.yrel * k * (settings.invert_y ? -1.0f : 1.0f), -89.0f, 89.0f);
    }
}

void Game::run_window() {
    // Touch: fingers drive the on-screen controls, and the mouse stands in for a
    // finger (a free pointer); else the mouse looks around.
    touch_mode = opt.scripts && logic.input == InputScheme::Touch;
    SDL_SetWindowRelativeMouseMode(window, !touch_mode);
    touch.mirror = settings.left_handed;
    if (!opt.size_given) apply_display();
    else renderer.set_vsync(settings.vsync);
    apply_sound();
    if (main_menu.open) SDL_SetWindowRelativeMouseMode(window, false);
    last = std::chrono::steady_clock::now();
    while (running) {
        if (touch_mode) {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            touch.layout(pw, ph);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(e);
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
        main_menu.unlocked = settings.chapters, main_menu.hard_unlocked = settings.hard_unlocked, main_menu.credits = settings.credits;
        if (!main_menu.open) main_menu.difficulty = settings.difficulty;
        if (main_menu.open) {   // the main menu (with Options over it), the game paused behind
            title_music("mmenu");
            int mw = 0, mh = 0;
            SDL_GetWindowSizeInPixels(window, &mw, &mh);
            FrameParams mf = draw_main_menu(mw, mh);
            if (menu.open) menu.draw(loading_overlay, loading_sprites, hud, subtitles, objective_text, {}, settings, mw, mh);
            renderer.render_to_window({}, mf);
            continue;
        }
        if (loading.active) {   // the level waits behind the loading screen
            loading.update(raw_dt);
            title_music("loading");
            int lw = 0, lh = 0;
            SDL_GetWindowSizeInPixels(window, &lw, &lh);
            renderer.render_to_window({}, draw_loading(lw, lh));
            continue;
        }
        if (logic.audio && logic.audio->playing("m_title")) logic.audio->stop("m_title", 1500);
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
        if (opt.scripts && !menu.open && !main_menu.open) logic.update_motion_blur(raw_dt);
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
            // CWeaponComponent::Aim: the weapon's own; one with a zoom slider starts there (within
            // its range) and the wheel moves it.
            float widest = 0, narrowest = 0;
            const bool zoom = opt.scripts && viewmodel.zoom_range(widest, narrowest);
            if (aiming && !aiming_) zoom_fov_ = zoom ? std::clamp(viewmodel.aim_fov(), narrowest, widest) : viewmodel.aim_fov();
            aiming_ = aiming;
            const float target = aiming && opt.scripts ? (zoom ? zoom_fov_ : viewmodel.aim_fov()) : base;
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
            const bool locked = opt.scripts && (logic.player_locked() || logic.interacting());   // cinematics, stepping up to use
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
                fire_step(dt, combat_in, player.eye(), cam.forward(), aiming);
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
        // Standing on a moving collision part (the lava surf's rocks, a cart): carried along.
        if (float turn = 0; !flying && !(opt.scripts && logic.seated()) && scene.collision().carry(player.center - glm::vec3(0, 0, player.radius.z - 0.3f), 0.8f, player.center, turn))
            cam.yaw += turn;
        if (opt.scripts) combat.place_attachments();
        if (opt.scripts) {
            ViewModel::Input vm;
            vm.weapon = combat.weapon_type();
            vm.visible = !flying && !combat.player_dead && !logic.player_locked() && !logic.limited_active() && !logic.weapon_hidden() &&
                         (!combat.special_weapon() || mech);
            vm.cockpit = mech;
            if (!logic.weapon_state.empty()) viewmodel.set_state(logic.weapon_state), logic.weapon_state.clear();
            arms_distort();
            if (combat.take_grenade_throw()) viewmodel.set_state("ThrowGrenade");
            if (combat.take_power_used()) viewmodel.set_state("Repel");
            if (const std::string h = combat.take_view_hurt(); !h.empty()) viewmodel.camera_state(aiming ? "IronSight_" + h : h);
            movement_states(aiming);
            player_sounds(dt, !flying && !mech && !logic.seated());
            update_frost(dt);
            vm.aiming = aiming;
            vm.running = player.rushing;   // (not once the rush is spent)
            const bool* k = SDL_GetKeyboardState(nullptr);
            vm.moving = player.on_ground && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_A] || k[SDL_SCANCODE_D] ||
                                             touch_in.forward != 0 || touch_in.right != 0);
            vm.firing = combat.firing();
            vm.shot = combat.shots() != vm_shots_, vm_shots_ = combat.shots();
            vm.reloading = combat.reloading();
            Camera v = cam;
            v.yaw += logic.shake_offset().x, v.pitch += logic.shake_offset().y;
            viewmodel.update(dt, v.position, v.yaw, v.pitch, vm);
            logic.weapon_playing = viewmodel.state();
        }
        if (logic.audio) logic.audio->set_listener(cam.position, cam.forward(), glm::vec3(0, 0, 1));
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        clock += dt;
        use_pressed_ = std::max(use_pressed_ - dt, 0.0f);
        Camera view = cam;
        if (opt.scripts) view.yaw += logic.shake_offset().x, view.pitch += logic.shake_offset().y;
        if (opt.scripts) view.viewmodel_fov = viewmodel.model_fov_add();
        Scene::CameraView cv;
        const bool cinematic = opt.scripts && logic.cinematic_view(cv);
        if (cinematic) {
            view.position = cv.position, view.directed = true, view.direction = cv.forward, view.up = cv.up;
            view.fov = glm::degrees(cv.fov), view.game_fov = false;
        } else if (opt.scripts && !flying && logic.limited_view(cv)) {   // the limited-control rig's camera
            view.position = cv.position, view.directed = true, view.direction = cv.forward, view.up = cv.up;
        } else if (opt.scripts) {
            view.jolt(viewmodel.camera_offset());
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

}  // namespace nova
