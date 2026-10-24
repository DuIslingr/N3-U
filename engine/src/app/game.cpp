#include "app/game.h"

#include <cstdio>
#include <stdexcept>

#include <SDL3/SDL.h>

#include "audio/audio.h"
#include "game/anim_sounds.h"

namespace nova {

namespace {

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

}  // namespace

Game::Game(const Options& options, const Vfs& vfs_, const Gol& gol_, SDL_Window* window_, const std::string& shader_dir)
    : opt(options),
      vfs(vfs_),
      gol(gol_),
      window(window_),
      headless(!options.screenshot.empty()),
      renderer(window_, shader_dir),
      scene(vfs_, renderer, gol_),
      campaign(campaign_levels(vfs_, gol_)),
      loading_hud(scene),
      loading_title(vfs_, renderer, "Days.ttf", std::max(14.0f, float(options.height) * 0.03f)),
      loading_text(vfs_, renderer, "Days.ttf", std::max(12.0f, float(options.height) * 0.022f)),
      loading_fonts_for(options.height),
      clock(options.time),
      logic(scene, player, gol_),
      combat(scene, player, logic, gol_),
      viewmodel(scene, gol_),
      subtitles(vfs_, renderer, "Days.ttf", std::max(16.0f, float(options.height) * 0.032f)),
      hud(scene),
      touch(hud),
      settings_path(headless ? std::string() : Settings::default_path()),
      settings(Settings::load(settings_path)),
      // The save file (Application::Save): written while playing the furthest chapter
      // reached (replaying an earlier one saves nothing, as the original's NoSave).
      save_path(!options.save_file.empty() ? options.save_file : headless ? std::string() : SaveGame::default_path()),
      flares(scene, logic),
      menu_title(vfs_, renderer, "Days.ttf", std::max(20.0f, float(options.height) * 0.06f)),
      objective_text(vfs_, renderer, "Days.ttf", std::max(13.0f, float(options.height) * 0.026f)),
      dialogue(vfs_, renderer, "Days.ttf", std::max(13.0f, float(options.height) * 0.024f)),
      fonts_for(options.height) {
    main_strings.load(vfs, "MAIN");
    loading.touch = opt.touch;
    // The main menu first, unless a level was asked for.
    main_menu.campaign = &campaign;
    main_menu.text = [this](const std::string& id) {
        const std::string t = main_strings.find(id);
        return t.empty() ? id : t;
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
    std::printf("%s: %zu placements, %zu models (%zu missing), %zu textures (%zu missing), %zu draws, "
                "%zu collision objects (%zu triangles, %zu missing), %zu animated (%zu animations missing), "
                "%zu draws hidden by LOD, %zu irradiance volumes, %zu lights, %zu skyboxes, loaded in %.2fs\n",
                opt.level.c_str(), st.placements, st.models, st.missing_models, st.textures,
                st.missing_textures, st.draws, st.collision_objects, st.collision_triangles,
                st.missing_collision, st.animated, st.missing_animations, st.lod_hidden, st.irradiance_volumes, st.lights, st.skyboxes, st.load_seconds);
    player.spawn(scene.start_position, scene.start_yaw);
    if (opt.touch) logic.input = InputScheme::Touch;
    if ((!headless && !opt.mute) || !opt.sound_render.empty()) {
        audio = std::make_unique<Audio>(vfs, opt.sound_render.empty());
        if (audio->enabled()) logic.audio = audio.get();
        audio->trace = !opt.sound_render.empty();
        scene.particles().on_sound = [this](const std::vector<std::string>& names, const glm::vec3& at) {
            if (!logic.audio) return;
            Audio::Params p;
            p.position = &at;
            logic.audio->play_one_of(names, p);
        };
    }
    if (opt.scripts) post_params = &logic.post, zfar = &logic.zfar;
    logic.auto_qte = headless;   // nobody to press the QTE buttons
    logic.trace = opt.trace_scripts;
    logic.skip_intro = opt.skip_intro;
    logic.log = [this](const std::string& line) {
        if (line.rfind("print: ", 0) == 0 && !opt.trace_scripts) return;   // scripts print a lot
        std::printf("[script] %s\n", line.c_str());
    };
    std::printf("zones: %zu zones, %zu portals\n", scene.zones().zones().size(), scene.zones().portals().size());
    combat.log = [](const std::string& line) { std::printf("[combat] %s\n", line.c_str()); };
    combat.player_invulnerable = opt.god;
    combat.death_screen = !headless;
    logic.combat = &combat;
    if (opt.credits >= 0) settings.credits = opt.credits;
    if (opt.difficulty >= 0) settings.difficulty = opt.difficulty;
    shop.load(gol);
    combat.set_difficulty(settings.difficulty), shop.difficulty = settings.difficulty;
    logic.on_checkpoint = [this] {
        if (!saving(opt.level)) return;
        SaveGame g;
        g.level = opt.level, g.checkpoint = logic.checkpoint_data();
        if (!g.write(save_path)) std::fprintf(stderr, "could not write %s\n", save_path.c_str());
    };
    if (!shop.items().empty()) menu.shop = &shop, menu.combat = &combat;
    shop.audio = logic.audio;
    end_screen.text = [this](const std::string& id) {
        const std::string t = main_strings.find(id);
        return t.empty() ? id : t;
    };
    death.text = [this](const std::string& id) {
        const std::string t = main_strings.find(id);
        return t.empty() ? id : t;
    };
    menu.text = [this](const std::string& id) {
        const std::string t = logic.strings.find(id);
        return t.empty() ? id : t;
    };
    menu.touch = logic.input == InputScheme::Touch;
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
        listen_at_player();
        if (opt.list_actors)
            for (const std::string& line : combat.describe()) std::printf("  %s\n", line.c_str());
    }
    if (!opt.spawn_at.empty()) spawn_at_option();
    for (const std::string& code : opt.exec) logic.exec(code);
    for (const std::string& h : opt.hide_models) std::printf("hid %zu placements of *%s*\n", scene.hide_models(h), h.c_str());
    if (!opt.continue_file.empty() && opt.scripts) {
        SaveGame g;
        if (!SaveGame::read(opt.continue_file, g)) throw std::runtime_error("cannot read the save " + opt.continue_file);
        resume(g);
    }
}

Game::~Game() = default;

// The level after `level` in the campaign's story order (gol "GameLevels"); empty after the last.
std::string Game::next_level(const std::string& level) const {
    const size_t slash = level.find_last_of('/');
    std::string base = level.substr(slash == std::string::npos ? 0 : slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".lvc") == 0) base.resize(base.size() - 4);
    for (size_t i = 0; i + 1 < campaign.size(); ++i)
        if (base == campaign[i].file) return level.substr(0, slash == std::string::npos ? 0 : slash + 1) + campaign[i + 1].file + ".lvc";
    return {};
}

// A screen-filling quad in the overlay (fades).
glm::vec4 Game::use_button(int w, int h) {
    hud.set_screen(w, h);
    const Sprite* cs = hud.sprite("controls.bsprite");
    const glm::vec4 b = (cs ? Hud::frame_bounds(*cs, 35) : glm::vec4(0, 0, 64, 64)) * hud.sprite_scale("controls.bsprite");
    const glm::vec2 size(b.z - b.x, b.w - b.y);
    const float top = float(h) * 0.5f + 100.0f * float(h) / 640.0f;
    return {(float(w) - size.x) * 0.5f, top, (float(w) + size.x) * 0.5f, top + size.y};
}

void Game::fullscreen(Overlay& o, int w, int h, const glm::vec4& c) {
    const uint32_t rgba = pack_rgba(c);
    const ColorVertex v[4] = {{{0, 0, 0}, rgba}, {{float(w), 0, 0}, rgba}, {{float(w), float(h), 0}, rgba}, {{0, float(h), 0}, rgba}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

const CampaignLevel* Game::campaign_level(const std::string& level) const {
    for (const CampaignLevel& c : campaign)
        if (level.find("/" + c.file + ".lvc") != std::string::npos || level == c.file + ".lvc") return &c;
    return nullptr;
}

FrameParams Game::draw_loading(int w, int h) {
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
}

// A frame of it before a level is read (the window shows nothing new until then).
void Game::show_loading(const std::string& level, bool section) {
    loading.begin(section ? nullptr : campaign_level(level));
    if (!window) return;
    SDL_PumpEvents();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    renderer.render_to_window({}, draw_loading(w, h));
}

FrameParams Game::draw_main_menu(int w, int h) {
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
}

FrameParams Game::frame(const Camera& cam_in, int w, int h) {
    Camera cam = cam_in;
    if (zfar) cam.zfar = *zfar;
    FrameParams f = cam.frame(w, h);
    scene.build_particles(f.camera, particle_list);
    if (opt.scripts) combat.build_beams(f.camera, particle_list);
    f.particles = &particle_list;
    f.fog_color = scene.fog_color;
    const glm::vec3 swing = opt.scripts ? logic.fog_swing() : glm::vec3(0);   // Fog_AnimationStart
    f.fog_start = scene.fog_start + swing.x;
    f.fog_end = scene.fog_end + swing.y;
    f.fog_density = scene.fog_density + swing.z;
    f.time = clock;
    f.lights = scene.lights();
    f.post = post_params;
    return f;
}

int Game::chapter_of(const std::string& level) const {
    for (size_t c = 0; c < campaign.size(); ++c)
        if (level.find("/" + campaign[c].file + ".lvc") != std::string::npos) return int(c);
    return -1;
}

bool Game::saving(const std::string& level) const {
    const int c = chapter_of(level);
    return !save_path.empty() && c >= 0 && (c + 1 >= settings.chapters || !opt.save_file.empty());
}

EndLevelScreen::Stats Game::end_stats() {
    const Combat::Stats& cs = combat.stats();
    EndLevelScreen::Stats st;
    st.headshots = int(cs.headshot_kills), st.blast_kills = int(cs.repel_kills), st.paralyze_kills = int(cs.paralyze_kills);
    st.slow_kills = int(cs.slow_kills), st.kills = int(cs.kills), st.retries = int(cs.deaths);
    st.criticals = int(cs.criticals);
    st.accuracy = cs.shots ? std::min(float(cs.hits) / float(cs.shots), 1.0f) : 1.0f;
    st.play_time = cs.play_time;
    return st;
}

EndLevelScreen::Rules Game::end_rules() {
    return EndLevelScreen::read_rules(gol, logic.play_time_limit(0), logic.play_time_limit(1));
}

// Settings that act on the game: sound levels (the menu's Sound page).
void Game::apply_sound() {
    if (!logic.audio) return;
    logic.audio->master = settings.sound ? 0.8f : 0.0f;
    logic.audio->sfx = logic.audio->voices = settings.sfx_volume;
    logic.audio->music = 0.6f * settings.music_volume;
}

// HUD_StartFrost / HUD_StopFrost: the frost spreads or melts over time; and the hits' HUD effects.
void Game::update_frost(float dt) {
    if (logic.frost.serial != frost_serial) {
        frost_serial = logic.frost.serial;
        if (logic.frost.on > 0) hud.start_frost(logic.frost.ms);
        else hud.stop_frost(logic.frost.ms);
    }
    hud.frost_max = logic.frost.max;
    hud.update_frost(dt);
    // The shield's ripple and the blood (CLevel::StartHudEffect, CHud::ShowBlood).
    if (combat.take_hud_damage()) hud.damage_effect();
    if (const int s = combat.take_scratch(); s >= 0) hud.scratch(s == 1);
    if (const float blood = combat.take_hud_blood(); blood >= 0) hud.show_blood(blood);
    if (combat.take_blood_fade()) hud.fade_blood();
    hud.update_effects(dt);
}

void Game::draw_text(FrameParams& f, int w, int h) {
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
        const float flare_dt = flare_clock < 0 ? 10.0f : clock - flare_clock;   // the first frame: settled
        flare_clock = clock;
        flares.frame(flare_dt, f.camera, f.proj * f.view, w, h, hud_sprites);
    }
    if (opt.scripts) {
        Hud::State hs;
        hs.visible = !combat.player_dead && logic.hud_enabled && (!logic.player_locked() || logic.interface_in_cinematic) && !menu.open &&
                     !logic.interacting();   // stepping up to use: PlayerComponent::SetState hides the interface
        hs.clip = combat.ammo_in_clip(), hs.reserve = combat.ammo_reserve();
        hs.grenades = (combat.grenades_enabled || combat.mines_picked()) && logic.controls.items && !logic.limited_active();
        hs.weapons = logic.controls.weapons && !logic.limited_active();
        hs.grenade_count = combat.mines_picked() ? combat.mines : combat.grenades;
        hs.shield = combat.player_health / std::max(combat.player_max_health, 1.0f);
        hs.jeep_shield = combat.vehicle_shield();
        hs.mech = logic.in_mech();
        hs.core = combat.core;
        if (combat.core >= 0) hs.core_left = (hs.shield - 1) / std::max(combat.core_percent / 100, 1e-3f);
        hs.weapon_sprite = combat.weapon_sprite();
        hs.clip_size = combat.clip_size();
        hud.frame(f, w, h, hs, hud_sprites);
        if (hs.visible) draw_crosshair(w, h);
        if (hs.visible && combat.hit_marker())   // CLevel::Render2DInterface
            if (const Sprite* cb = hud.sprite("chatboard.bsprite"))
                hud.draw_frame(hud_sprites, *cb, 231, {float(w) * 0.5f, float(h) * 0.5f}, hud.sprite_scale("chatboard.bsprite"), glm::vec4(1));
        if (float angle = 0; hs.visible && combat.hit_indicator(angle))   // CLevel::Render2DInterface
            if (const Sprite* cs = hud.sprite("controls.bsprite"))
                hud.draw_frame_rotated(hud_sprites, *cs, 60, {float(w) * 0.5f, float(h) * 0.5f}, hud.sprite_scale("controls.bsprite"),
                                       glm::radians(angle), glm::vec4(1));
        const std::string& qte = logic.qte_kind();
        if (logic.input == InputScheme::Touch && (hs.visible || !qte.empty())) {
            const LevelLogic::Controls& lc = logic.controls;
            touch.shown = {lc.move, lc.move && lc.rush, lc.shoot && combat.shoot_enabled, lc.aim, lc.jump, lc.powers};
            if (!hs.visible) touch.shown = {false, false, qte == "fire", false, false, qte == "repel"};   // a QTE in a cinematic
            touch.layout(w, h);
            touch.power_ready = combat.power_ready();
            touch.rush = player.rush_left();
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
    if (opt.scripts && logic.hud_enabled && !menu.open && (logic.use_available() || use_pressed_ > 0))
        if (const Sprite* cs = hud.sprite("controls.bsprite")) {
            const glm::vec4 r = use_button(w, h), b = Hud::frame_bounds(*cs, 35);
            const float scale = (r.z - r.x) / std::max(b.z - b.x, 1.0f);
            hud.draw_frame(hud_sprites, *cs, use_pressed_ > 0 ? 36 : 35, glm::vec2(r.x, r.y) - glm::vec2(b.x, b.y) * scale, scale, glm::vec4(1));
            if (logic.input != InputScheme::Touch)
                objective_text.draw(hud_sprites, "[E]", {(r.x + r.z) * 0.5f, r.w + 14 * scale}, w, glm::vec4(1));
        }
    if (opt.scripts && !logic.player_locked() && !logic.interacting() && logic.hud_enabled)
        for (const LevelLogic::ObjectiveMarker& m : logic.objective_markers) {
            const LevelLogic::Object* o = logic.object(m.id);
            if (!o || !o->enabled) continue;
            glm::vec3 at = o->position;
            if (!m.node.empty()) scene.node_position(o->index, m.node, at);
            hud.draw_marker(hud_sprites, objective_text, f.proj * f.view, at, glm::distance(f.camera, at), w, h, settings.metric);
        }
    if (opt.scripts && logic.path_marker.on && !logic.player_locked() && !logic.interacting() && logic.hud_enabled)
        hud.draw_marker(hud_sprites, objective_text, f.proj * f.view, logic.path_marker.position,
                        glm::distance(f.camera, logic.path_marker.position), w, h, settings.metric);
    if (opt.scripts && !logic.player_locked() && !logic.interacting() && logic.hud_enabled)
        for (const glm::vec3& g : combat.grenade_threats()) hud.draw_grenade_warning(hud_sprites, f.view, g, w, h);
    if (opt.scripts && logic.timer >= 0 && !logic.player_locked() && !logic.interacting() && logic.hud_enabled)
        hud.draw_timer(hud_sprites, objective_text, logic.timer, w, h);
    if (const Combat::PickupInfo& pi = combat.pickup_info(); pi.time > 0 && opt.scripts && !logic.player_locked() && !logic.interacting() && logic.hud_enabled)
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
}

// A level from its start (the next campaign level, Restart Level); the arsenal carries over.
void Game::load_level(const std::string& level) {
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
    listen_at_player();
}

// Continue (Application::Load): the save's level from its checkpoint (as a respawn
// there), or from its start with the arsenal carried in.
void Game::resume(const SaveGame& g) {
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
        listen_at_player();
    } else {
        load_level(g.level);
        if (g.has_arsenal) combat.restore_arsenal(g.arsenal), shop.apply_owned(combat, settings);
    }
    std::printf("continued %s%s\n", g.level.c_str(), g.checkpoint.empty() ? "" : " from its checkpoint");
}

// Level logic at a fixed step, so triggers and doors see every position.
void Game::step_logic(float dt) {
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
        listen_at_player();
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
        listen_at_player();
    }
    // EndLevel: a moment on the faded screen, then the next campaign level (the
    // same one again after a failure). The arsenal carries over.
    if (!logic.level_ended) {
        ended_for = 0;
    } else if ((ended_for += dt) > 3.0f) {
        // Complete: the end-of-level screen first; its Continue comes back here.
        if (logic.level_success && !headless && !end_screen.open && !end_screen_done) {
            const bool finished = next_level(opt.level).empty();
            end_screen.show(end_stats(), end_rules(), finished);
            if (finished) settings.hard_unlocked = true;   // Hard is offered from now on
            // CUpgradesManager::GiveUpgradePoints, kept with the settings.
            settings.credits = std::max(settings.credits + end_screen.earned().total, 0);
            settings.save(settings_path);
            std::printf("end of level: %d credits (%d in all)\n", end_screen.earned().total, settings.credits);
        }
        if (end_screen.open) return;
        end_screen_done = false;
        const std::string next = logic.level_success ? next_level(opt.level) : opt.level;
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
}

// StartJump / StartLanding: the weapon's own clips (ironsight ones while aiming).
// --spawn-at: X,Y,Z[,YAW] or an object's id.
void Game::spawn_at_option() {
    float v[4] = {0, 0, 0, 0};
    if (std::sscanf(opt.spawn_at.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) >= 3) {
        player.spawn({v[0], v[1], v[2]}, v[3]);
    } else {
        const LevelLogic::Object* o = logic.object(std::stoi(opt.spawn_at));
        if (!o) throw std::runtime_error("no object with id " + opt.spawn_at);
        player.spawn(o->position, o->yaw);
    }
    listen_at_player();
}

void Game::listen_at_player() {
    if (logic.audio) logic.audio->set_listener(player.eye(), player.view_forward(), glm::vec3(0, 0, 1));
}

// CWeaponComponent::DrawTargetCrossHair: the weapon's sprite's frame 4 in the middle and its
// arms (5-8) pushed out by the spread, tinted red over an enemy; a scope's (the sniper
// rifle's) only while looking through it, the screen round it dimmed and the distance to
// what it is on in red under it.
void Game::draw_crosshair(int w, int h) {
    const Combat::Crosshair x = combat.crosshair();
    if (x.sprite.empty() || (x.scope ? !viewmodel.scoped() : x.alpha <= 0)) return;
    const Sprite* sp = hud.sprite(x.sprite);
    if (!sp) return;
    const float k = hud.sprite_scale(x.sprite), px = float(h) / 640.0f;
    const glm::vec2 c(float(w) * 0.5f, float(h) * 0.5f);
    const float d = std::round(x.arms * px);
    const glm::vec4 color(glm::vec3(x.color), x.color.a * x.alpha);
    if (x.scope) {   // four rectangles of black at 150 round frame 4
        const glm::vec4 b = Hud::frame_bounds(*sp, 4);
        const glm::vec4 r(c.x + b.x * k, c.y + b.y * k, c.x + b.z * k, c.y + b.w * k);
        const glm::vec4 dark(0, 0, 0, 150.0f / 255.0f);
        const float W = float(w), H = float(h);
        if (r.z < W) objective_text.fill(hud_sprites, {r.z, 0, W, H}, dark, dark);
        if (r.x > 0) objective_text.fill(hud_sprites, {0, 0, r.x, H}, dark, dark);
        if (r.w < H) objective_text.fill(hud_sprites, {r.x, r.w, r.z, H}, dark, dark);
        if (r.y > 0) objective_text.fill(hud_sprites, {r.x, 0, r.z, r.y}, dark, dark);
    }
    hud.draw_frame(hud_sprites, *sp, 4, c, k, color);
    static const glm::vec2 kCorner[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, kPlus[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    for (int i = 0; i < 4; ++i) hud.draw_frame(hud_sprites, *sp, 5 + i, c + (x.plus ? kPlus[i] : kCorner[i]) * d, k, color);
    if (x.scope && x.distance >= 0) {
        char line[32];
        std::snprintf(line, sizeof line, "%0.2fm", x.distance);
        objective_text.draw(hud_sprites, line, c + glm::vec2(0, 64 * px), w, glm::vec4(1, 0, 0, 1), {0.5f, 0.0f});
    }
    // The zoom slider (CLevelInterfaceNormal::EnableZoomSlider; a CSlider of controls.bsprite
    // at 30 left of the right edge and 20 below the middle in 480x320 units): its track
    // (frame 39) and its knob (41) up it from the widest view to the narrowest.
    float widest = 0, narrowest = 0;
    const Sprite* cs = hud.sprite("controls.bsprite");
    if (x.scope && cs && viewmodel.zoom_range(widest, narrowest) && widest != narrowest) {
        const float kc = hud.sprite_scale("controls.bsprite");
        const glm::vec4 track = Hud::frame_bounds(*cs, 39);
        const glm::vec2 at(float(w) - 30.0f * float(w) / 480.0f, float(h) * 0.5f - (track.w - track.y) * kc * 0.5f + 20.0f * float(h) / 320.0f);
        hud.draw_frame(hud_sprites, *cs, 39, at, kc, glm::vec4(1));
        const float fov = zoom_fov_ > 0 ? std::clamp(zoom_fov_, std::min(widest, narrowest), std::max(widest, narrowest)) : widest;
        const float along = (fov - widest) / (narrowest - widest);
        const glm::vec2 knob(at.x + (track.x + track.z) * 0.5f * kc, at.y + glm::mix(track.w, track.y, along) * kc);
        hud.draw_frame(hud_sprites, *cs, 41, knob, kc, glm::vec4(1));
    }
}

void Game::fire_step(float dt, const CombatInput& in, const glm::vec3& eye, const glm::vec3& aim, bool aiming) {
    glm::vec3 at;
    combat.set_muzzle(viewmodel.muzzle(at) ? &at : nullptr);
    const size_t shots = combat.stats().shots;
    CombatInput with_aim = in;
    with_aim.aiming = aiming;
    combat.update(dt, with_aim, eye, aim);
    if (combat.stats().shots != shots) viewmodel.flash(aiming);
}

void Game::movement_states(bool aim) {
    if (player.jumps != view_jumps) view_jumps = player.jumps, viewmodel.movement_state(aim ? "AimJump" : "Jump");
    static const char* const kLanding[2][3] = {{"Landing", "LandingMed", "LandingHi"}, {"LandingAim", "LandingAimMed", "LandingAimHi"}};
    if (const int l = combat.take_landing(); l >= 0) {
        viewmodel.movement_state(kLanding[aim][l]);
        if (logic.audio && player_type_ >= 0)   // CActorSoundsComponent: event 44, landing
            logic.anim_sounds().footstep(*logic.audio, player_type_,
                                         AnimSounds::ground_material(scene, player.center - glm::vec3(0, 0, player.radius.z)),
                                         AnimSounds::Step::Land, nullptr);
    }
}

void Game::player_sounds(float dt, bool walking) {
    const glm::vec2 now(player.center);
    const float speed = dt > 0 ? glm::length(now - step_last_) / dt : 0;
    step_last_ = now;
    if (!logic.audio) return;
    if (player_type_ == -2) {
        player_type_ = -1;
        for (const auto& [id, t] : gol)
            if (t.name == "Player") player_type_ = id;
    }
    if (player_type_ < 0) return;
    AnimSounds& sounds = logic.anim_sounds();
    const glm::vec3 feet = player.center - glm::vec3(0, 0, player.radius.z);
    if (player.jumps != sound_jumps_) {
        sound_jumps_ = player.jumps;
        sounds.footstep(*logic.audio, player_type_, AnimSounds::ground_material(scene, feet), AnimSounds::Step::Jump, nullptr);
    }
    // Moving on the ground: a step at once, then one every ComputeStepTimer (shorter the faster).
    if (!walking || !player.on_ground || speed < 0.5f) {
        step_timer_ = 0;
        return;
    }
    if ((step_timer_ -= dt * 1000) >= 0) return;
    const float k = std::min(speed / player.walk_speed, 1.0f);
    step_timer_ = player.step_max_ms - k * (player.step_max_ms - player.step_min_ms);
    sounds.footstep(*logic.audio, player_type_, AnimSounds::ground_material(scene, feet),
                    player.rushing ? AnimSounds::Step::Run : AnimSounds::Step::Walk, nullptr);
}

void Game::arms_distort() {
    const LevelLogic::ArmsDistort& d = logic.arms_distort;
    if (d.serial == distort_serial) return;
    distort_serial = d.serial;
    viewmodel.arms_distort(d.on);
    if (d.on) viewmodel.arms_distort_blend(d.target, d.ms);
}

int Game::run() {
    view_spawns = player.spawns, view_jumps = player.jumps;
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
    if (opt.audit > 0) return run_audit();
    if (opt.walk_test > 0) run_walk_test();
    if (headless || opt.walk_test > 0) return opt.screenshot.empty() ? 0 : run_screenshot();
    run_window();
    return 0;
}

}  // namespace nova
