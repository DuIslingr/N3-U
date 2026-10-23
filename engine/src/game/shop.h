// The Military Shop (CUpgradesManager): what credits buy from the pause page. Its list is
// gol 4510 UpgradesManager [index, name, modifier, cost, icon (upgrades.bsprite), description,
// item?, ...]: the ITEM tab holds the shield cores, grenade and mine packs and the ammo
// refill (each used at once, bought again at will), the UPGRADE tab the Plasma, Lightning and
// Rail guns and the Repel and Paralyze upgrades (bought once, kept for good).
#pragma once

#include <string>
#include <vector>

#include "assets/level.h"

namespace nova {

class Audio;
class Combat;
struct Settings;

class Shop {
public:
    struct Item {
        int index = 0;
        std::string name, description;   // string ids, "FPS_UPGRADE_VANGUARDCORE"
        float modifier = 0;
        int cost = 0;
        int icon = 0;                    // upgrades.bsprite frame
        bool item = false;               // the ITEM tab (else UPGRADE)
    };
    void load(const Gol& gol);
    Audio* audio = nullptr;   // a purchase's sound (the iOS build's sfx_menu_nova3_*)
    const std::vector<Item>& items() const { return items_; }

    // CUpgradesManager::GetUpgradeCost: the list's cost by the difficulty's factor
    // (DifficultyLevels [4]: 1 at every difficulty).
    int difficulty = 1;
    int cost(int i) const;
    bool owned(int i, const Settings& s) const;
    // CUpgradesManager::CanUpgrade: affordable, not owned, and of use now.
    bool can_buy(int i, const Combat& combat, const Settings& s) const;
    // CUpgradesManager::BuyUpgrade: the credits taken and the upgrade applied.
    bool buy(int i, Combat& combat, Settings& s) const;
    // At a level's start: what was bought for good (the weapons, Repel and Paralyze).
    void apply_owned(Combat& combat, const Settings& s) const;

private:
    std::vector<Item> items_;
    float cost_factor_[3] = {1, 1, 1};
    const Item* find(int i) const { return i >= 0 && i < int(items_.size()) ? &items_[size_t(i)] : nullptr; }
    void apply(const Item& it, Combat& combat) const;
};

}  // namespace nova
