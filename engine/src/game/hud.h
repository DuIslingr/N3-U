// The in-game HUD (CHud): NV_hud.bdae, a small 3D scene seen through its own camera
// (hud_camera) and drawn over the finished frame: the pause button, the weapon panel with
// its ammo counter and switch arrows, the grenade button. Multiplayer parts, touch zones
// and pressed ("active") states are hidden.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "assets/bsprite.h"

#include <glm/glm.hpp>

#include "render/renderer.h"

namespace nova {

class Scene;
class TextRenderer;

class Hud {
public:
    explicit Hud(Scene& scene) : scene_(scene) {}
    // After the level is loaded.
    void start();
    struct State {
        bool visible = true;
        int clip = 0, reserve = 0;   // ammo
        int clip_size = 1;
        bool grenades = false;
        int grenade_count = 0;
        // The weapons panel with the ammo (CHud::EnabledWeapons): off while the interface's
        // controls are (limited control: CLevelInterfaceNormal::EnableControls(false)).
        bool weapons = true;
        float shield = 1;            // the player's health, 0..1 (the shield bar at the top)
        float jeep_shield = -2;      // riding a jeep: its health, 0..1 (HUDShield_Jeep instead); -2 none
        // A shield core's overcharge (the Military Shop): its kind (0 Vanguard, 1 Crypton,
        // 2 Plasma; -1 none) and how much of it is left, 0..1 (CHud::SetShieldPercent).
        int core = -1;
        float core_left = 0;
        std::string weapon_sprite;   // the weapon's .bsprite: frame 0 is its icon
    };
    // A prompt with touch-button glyphs (^NN^: frames of tutorial.bsprite) drawn inline,
    // centred with its top at `at`.
    void draw_prompt(ParticleList& out, TextRenderer& text, const std::string& s, glm::vec2 at, const glm::vec4& color);
    // The objectives box (CLevel::RenderPrimaryObjective): from (40, 100) in 480x320 base
    // pixels, the primary objective with its marker, then the secondaries with check boxes.
    struct ObjectiveLine {
        std::string text;
        bool primary = false, done = false;
    };
    void draw_objectives(ParticleList& out, TextRenderer& text, const std::vector<ObjectiveLine>& lines, float alpha, int w, int h);
    // CLevel::DrawGrenades: an enemy grenade that can reach the player: hud3d's grenade icon
    // (frame 15) on a ring 110 px (at 640 high) round the middle, its way.
    void draw_grenade_warning(ParticleList& out, const glm::mat4& view, const glm::vec3& at, int w, int h);
    // ILevelInterface::DrawTimer: the time (MM:SS) centred at (W/2, 72) on Chatboard frame 230.
    void draw_timer(ParticleList& out, TextRenderer& text, float seconds, int w, int h);
    // A pick-up taken (CLevelInterfaceNormal::Draw, ShowPowerUpInfo): its icon (frame 0 of
    // `sprite`) at (W - 40, 120) of 480x320, what it gave under it.
    void draw_pickup(ParticleList& out, TextRenderer& text, const std::string& sprite, int amount, int w, int h);
    // CLevel::RenderObjective: the guide marker (Chatboard.bsprite) over a point with its
    // distance; off screen, an arrow at the edge pointing its way.
    void draw_marker(ParticleList& out, TextRenderer& text, const glm::mat4& view_proj, const glm::vec3& at, float distance, int w, int h,
                     bool metric = true);
    // CHud frost (HUD_Frost: hud_frost.bdae in the HUD scene): it grows over `ms`
    // (StartFrost) up to the max factor of its clip, and melts back (StopFrost).
    void start_frost(float ms) { frost_target_ = 1, frost_rate_ = 1000.0f / std::max(ms, 1.0f); }
    void stop_frost(float ms) { frost_target_ = 0, frost_rate_ = 1000.0f / std::max(ms, 1.0f); }
    float frost_max = 1;
    void update_frost(float dt);
    // HUD_Damage (hud_damage.bdae, PostEffects HUD effect 24): the shield's blue ripple over
    // the screen, its clip once from the start (CLevel::StartHudEffect(4)).
    void damage_effect();
    // HUD_Blood2 (hud_blood_2.bdae, CHud::ShowBlood): blood held at a point of its clip by the
    // shield left (`fraction` of the maximum: under 1/4 all of it), until fade_blood() plays
    // the rest of the clip (the regeneration starting) and it goes.
    void show_blood(float fraction);
    void fade_blood();
    void update_effects(float dt);
    // Set the frame's HUD camera and draw the HUD's numbers into `sprites`.
    void frame(FrameParams& f, int w, int h, const State& s, ParticleList& sprites);

private:
    // CHud::SetNodePosition: put a part where the HUD camera sees it at pixel (x, y).
    void place(const std::string& node, glm::vec2 px, const glm::mat4& view_proj, int w, int h);
    // CLevel::DrawNumericText into one of CHud's quad buffers: hud3d.bsprite's digits (frames
    // 0-9, '/' 14), right-aligned and vertically centred on the buffer's origin, a buffer
    // being a child of HUD node `node` at `offset`, scaled by `scale`; quads lie in its XZ
    // plane (sprite y down = -Z).
    void numeric_text(ParticleList& out, const std::string& text, const std::string& node, glm::vec3 offset, float scale,
                      const glm::mat4& view_proj, int w, int h);

public:
    // A sprite sheet: on large screens (min side 960+ px) the 2x variant the game ships
    // (<name>_1720x1280 or _2560x1504), else the 960x640 one. set_screen() picks which.
    const Sprite* sprite(const std::string& name);
    void set_screen(int w, int h) { screen_min_ = float(std::min(w, h)), large_ = screen_min_ >= 960; }
    // Screen pixels per pixel of that sheet (its frames are laid out for a 640-high screen at 1x).
    float sprite_scale(const std::string& name);
    // Draw a sprite frame with its anchor at `at` (pixels), scaled.
    void draw_frame(ParticleList& out, const Sprite& s, int frame, glm::vec2 at, float scale, const glm::vec4& color) {
        draw_frame(out, s, frame, at, glm::vec2(scale), color);
    }
    // Scaled per axis; a negative x scale mirrors the frame about its anchor. Only what lies
    // inside `clip` (x0, y0, x1, y1 in pixels) is drawn.
    void draw_frame(ParticleList& out, const Sprite& s, int frame, glm::vec2 at, glm::vec2 scale, const glm::vec4& color,
                    const glm::vec4& clip = glm::vec4(-1e9f, -1e9f, 1e9f, 1e9f));
    // An animation (ticks of 1/30 s, looping) at `seconds` into it.
    void draw_anim(ParticleList& out, const Sprite& s, int anim, glm::vec2 at, float scale, float seconds, const glm::vec4& color);
    // Turned by `radians` (clockwise on screen) about its anchor.
    void draw_frame_rotated(ParticleList& out, const Sprite& s, int frame, glm::vec2 at, float scale, float radians,
                            const glm::vec4& color);
    // A sprite file by its exact name (no resolution variant chosen); null if missing.
    const Sprite* sprite_file(const std::string& file);
    // Frames not drawn where other frames place them (menu layouts' position markers).
    std::vector<int> skip_frames;
    // A frame's box relative to its anchor: min x, min y, max x, max y (sprite pixels).
    static glm::vec4 frame_bounds(const Sprite& s, int frame);

private:

    Scene& scene_;
    std::unordered_map<std::string, Sprite> sprites_;          // by name, plus "@2x" for the large ones
    std::unordered_map<std::string, float> sprite_factor_;     // 1 or 2
    float screen_min_ = 640;
    bool large_ = false;
    size_t object_ = size_t(-1), ammo_ = size_t(-1), frost_object_ = size_t(-1), shield_ = size_t(-1);
    float shield_shown_ = -1, jeep_shown_ = -1;
    size_t jeep_shield_ = size_t(-1);
    bool jeep_visible_ = false;
    int core_shown_ = -1;
    float frost_ = 0, frost_target_ = 0, frost_rate_ = 1;   // 0..1 of the clip
    size_t damage_ = size_t(-1), blood_ = size_t(-1);
    bool damage_shown_ = false, blood_shown_ = false, blood_fading_ = false;
    bool frost_shown_ = false;
    bool weapons_ = true;
    glm::ivec2 placed_for_{0};
    bool visible_ = false, grenades_ = true;
};

}  // namespace nova
