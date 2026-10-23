// Lens flares (CLensFlareComponent, gol types LensFlare/LensFlareB with fx_lensflare_*.bdae):
// the object follows the camera, so its "sunGlow" node marks a fixed direction, the sun.
// Every 8 frames five rays a degree or two apart are cast toward it; each one blocked takes
// a fifth off the flare's target brightness (three or more hide the glow), which it eases
// toward. Near the screen's edges it fades. The glow is drawn at the sun scaled by
// params[0]; the "lensFlare" node's quads lie along the line from the sun through the
// screen's centre, sized between params[3] and params[2] by the brightness.
#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "render/renderer.h"

namespace nova {

class Scene;
class LevelLogic;

class LensFlares {
public:
    LensFlares(Scene& scene, LevelLogic& logic) : scene_(scene), logic_(logic) {}
    void start();   // after the level is loaded
    // Advance and draw (additive quads in screen pixels) for this view.
    void frame(float dt, const glm::vec3& camera, const glm::mat4& view_proj, int w, int h, ParticleList& out);

private:
    struct Flare {
        int id = -1;               // the level object (its enabled state switches it)
        size_t model = size_t(-1); // our own copy of its model, placed at the camera
        glm::mat4 rotation{1};
        float params[5] = {30, 1000, 3, 0.5f, 2};
        float fade = 0, target = 1;
        bool glow = true;
        std::vector<std::string> chain;   // the lensFlare node's quads
    };
    Scene& scene_;
    LevelLogic& logic_;
    std::vector<Flare> flares_;
    unsigned frames_ = 0;
};

}  // namespace nova
