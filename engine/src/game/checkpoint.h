// LevelLogic's checkpoint (what SaveGame keeps): shared by level_logic.cpp, which takes and
// restores it, and save_game.cpp, which writes it to the save file and reads it back.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "game/combat.h"
#include "game/level_logic.h"
#include "game/scene.h"

namespace nova {

struct LevelLogic::Checkpoint {
    int spawn = -1;
    uint32_t section = 1;   // CLevel's loaded sections
    struct SavedObject {
        bool enabled = true, visible = true, dead = false, interact_enabled = true;
        float health = 0;
        std::string state;
        glm::vec3 position{0};
        float yaw = 0;
        std::vector<uint8_t> effects;   // running (CEffectsComponent::SaveSave)
        bool has_clip = false;          // CAnimationComponent::SaveSave: the clip it plays
        Scene::ClipState clip;
    };
    std::unordered_map<int, SavedObject> objects;
    std::vector<int> trigger_runs;      // CTriggerComponent::SaveSave
    struct SavedDoor {
        Door::State state = Door::State::Closed;
        bool locked = false, keep_unlocked = false, forced_open = false;   // CDoorAIComponent::SaveSave
    };
    std::vector<SavedDoor> doors;
    std::vector<std::pair<int, glm::vec3>> movers;   // where moving objects stand
    struct SavedVehicle {
        int id = 0;
        glm::vec3 position{0};
        float yaw = 0;
    };
    std::vector<SavedVehicle> vehicles;
    Objectives objectives;              // CPlayerBaseComponent, CHud
    std::vector<ObjectiveMarker> markers;
    PathMarker path_marker;
    glm::vec4 fog_color{0};             // CLevelFog::Save
    float fog_start = 0, fog_end = 0, fog_density = 0;
    bool fog_locked = false;
    PostParams post;                    // PostEffects::Save
    HeatHaze heat;
    std::string grading;                // the colour grading
    bool grading_on = false;
    int mech = -1, vehicle = -1, gun = -1;   // PlayerComponent::SaveSave: what the player is in
    Combat::Saved combat;
};

}  // namespace nova
