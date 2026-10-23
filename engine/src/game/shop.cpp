#include "game/shop.h"

#include <algorithm>

#include "game/combat.h"
#include "game/settings.h"

namespace nova {

namespace {

constexpr int kUpgradesManager = 4510, kDifficultyLevels = 4509;
constexpr int kNormal = 1;            // the difficulty's index in DifficultyLevels' lists
constexpr int kMaxGrenades = 300;     // PowerManager: ThrowGrenade's maximum ammo
const char* const kWeapons[] = {"ExecutorPlasmaGun", "AlienLightningGun", "RailGun"};   // upgrades 6, 7, 8

}  // namespace

void Shop::load(const Gol& gol) {
    items_.clear();
    if (auto it = gol.find(kUpgradesManager); it != gol.end())
        if (const Value* v = resolved_template(it->second, gol, 0x74); v && v->size() > 0)
            for (const Value& e : (*v)[0].list) {
                if (e.size() < 9) continue;
                Item item;
                item.index = int(items_.size());
                item.name = e[1].s, item.modifier = e[2].as_float(), item.cost = int(e[3].as_float());
                item.icon = int(e[4].as_float()), item.description = e[5].s, item.item = e[6].i != 0;
                items_.push_back(item);
            }
    if (auto it = gol.find(kDifficultyLevels); it != gol.end())
        if (const Value* v = resolved_template(it->second, gol, 0x3d); v && v->size() > 3 && (*v)[3].size() > kNormal) {
            const Value& f = (*v)[3][kNormal];
            cost_factor_ = f.size() > 0 ? f[0].as_float() : f.as_float();
        }
}

int Shop::cost(int i) const {
    const Item* it = find(i);
    return it ? int(float(it->cost) * cost_factor_) : 0;
}

bool Shop::owned(int i, const Settings& s) const {
    const Item* it = find(i);
    return it && !it->item && (s.upgrades >> i & 1);
}

bool Shop::can_buy(int i, const Combat& combat, const Settings& s) const {
    const Item* it = find(i);
    if (!it || owned(i, s)) return false;
    switch (it->index) {
        case 0: case 1: case 2: if (!combat.core_allowed()) return false; break;
        case 3: if (combat.grenades + int(it->modifier) > kMaxGrenades) return false; break;
        case 4: return false;   // mines are not thrown in this port yet
        case 5: if (!combat.can_refill()) return false; break;
        default: break;
    }
    const int c = cost(i);
    return c > 0 && c <= s.credits;
}

bool Shop::buy(int i, Combat& combat, Settings& s) const {
    if (!can_buy(i, combat, s)) return false;
    const Item& it = items_[size_t(i)];
    s.credits -= cost(i);
    if (!it.item) s.upgrades |= 1 << i;
    apply(it, combat);
    return true;
}

void Shop::apply(const Item& it, Combat& combat) const {
    // CUpgradesManager::ApplyUpgrade, by the upgrade's index.
    switch (it.index) {
        case 0: case 1: case 2: combat.take_core(it.index, it.modifier); break;
        case 3: combat.grenades = std::min(combat.grenades + int(it.modifier), kMaxGrenades); break;
        case 4: combat.mines += int(it.modifier); break;
        case 5: combat.refill_weapons(); break;
        case 6: case 7: case 8: combat.unlock_weapon(kWeapons[it.index - 6]); break;
        case 9: combat.repel_scale = it.modifier; break;
        case 10: combat.paralyze_damage = it.modifier; break;
        default: break;
    }
}

void Shop::apply_owned(Combat& combat, const Settings& s) const {
    for (const Item& it : items_)
        if (owned(it.index, s)) apply(it, combat);
}

}  // namespace nova
