// The end of a level (GS_EndLevel): over a darkened view, CONGRATULATIONS! and the level's
// STATISTICS, a row every 300 ms sliding in from the left as it fades in, then Back to Menu
// and Continue (Chatboard's slanted buttons at (59, 280) and (252, 280) of 480x320).
//
// The credits (the shop's currency, CUpgradesManager::GiveUpgradePoints) come from the
// kills, then bonuses and penalties in percent of those: the shield, accuracy and play time
// against the retries (CComponentLevelStatistics, gol 3038). Each value counts up as its row
// comes in, and TOTAL CREDITS sums them.
#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "assets/level.h"
#include "game/hud.h"
#include "render/renderer.h"

namespace nova {

class TextRenderer;

class EndLevelScreen {
public:
    struct Stats {
        int headshots = 0, blast_kills = 0, paralyze_kills = 0, slow_kills = 0, kills = 0, retries = 0;
        int criticals = 0;                    // falls below 30% of the shield
        float accuracy = 1, play_time = 0;   // 0..1 (1 without a shot), seconds
    };
    // CComponentLevelStatistics, and the level's play time limits (level properties [43], [44]).
    struct Rules {
        int per_kill[5] = {1, 2, 2, 2, 1};   // headshot, blast, paralyze, slow time, any kill
        int shield[3] = {10, 5, 0};          // % with no fall below 30%, fewer than 4, more (taken off)
        int accuracy = 10;                   // % at 100% accuracy
        int retry = 2;                       // % taken off a retry
        float time[3] = {10, 5, -10};        // % within the first limit, the second, beyond
        float limit_minutes[2] = {12, 22};
    };
    struct Credits {
        int kills = 0;                       // the credits for the kills, which the bonuses are percent of
        int shield = 0, accuracy = 0, accuracy_bonus = 0, time = 0;   // percent
        int total = 0;
    };
    static Credits credits(const Stats& stats, const Rules& rules);
    // The rules from gol 3038 LevelStatistics (the defaults where it is missing); the limits
    // are the level's.
    static Rules read_rules(const Gol& gol, float limit_minutes_1, float limit_minutes_2);
    std::function<std::string(const std::string&)> text = [](const std::string& id) { return id; };
    bool open = false;
    void show(const Stats& stats, const Rules& rules, bool game_finished);
    const Credits& earned() const { return credits_; }
    void update(float dt);

    enum class Action { None, Continue, Menu };
    enum class Key { Left, Right, Activate };
    Action key(Key k);
    enum class Pointer { Move, Press, Release };
    Action pointer(glm::vec2 at, Pointer phase);
    void draw(Overlay& overlay, ParticleList& out, Hud& hud, TextRenderer& title, TextRenderer& rows, int w, int h);

private:
    Stats stats_;
    Rules rules_;
    Credits credits_;
    bool finished_ = false;
    float time_ = 0;
    int selected_ = 1, pressed_ = -1;   // 0 Back to Menu, 1 Continue
    glm::vec4 rects_[2]{};
    bool ready() const;
};

}  // namespace nova
