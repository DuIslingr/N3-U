// Zones and portals (CZonesManager / CZone / CZonePortal).
//
// A level is split into zones: every object belongs to the zone record that precedes
// it in the level file; meta zones hold objects shared by the zones they list. Zones are joined by portals,
// quads with an enabled flag that scripts switch (EnablePortal / DisablePortal).
// The camera's zone is found by bounds once, then follows the camera through
// portals; each frame the visible zones are those reached from it through enabled
// portals that the (progressively narrowed) view frustum sees.
#pragma once

#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/level.h"

namespace nova {

struct Frustum {
    std::vector<glm::vec4> planes;   // inside: dot(xyz, p) + w >= 0
    static Frustum from_view_proj(const glm::mat4& view_proj);
    bool sees(const glm::vec3* points, size_t n) const;   // not entirely outside any plane
};

class ZoneSystem {
public:
    static constexpr int kNone = -1;

    struct Fog {
        bool override = false;   // false: the level's fog
        glm::vec4 color{0};
        float start = 0, end = 0, density = 1;
    };
    struct Zone {
        int id = 0;
        std::vector<int> children;   // meta zones shown with this zone
        glm::vec3 min{std::numeric_limits<float>::max()}, max{-std::numeric_limits<float>::max()};
        Fog fog;
        std::vector<int> portals;
    };
    struct Portal {
        int id = 0;
        int zone[2] = {kNone, kNone};
        bool enabled = true;
        std::array<glm::vec3, 4> corners;
        glm::vec3 center{0};
        glm::vec4 plane{0};
        size_t door = std::numeric_limits<size_t>::max();   // level object of a door portal
    };

    // Build from the level: membership, portals, meta zones. Bounds come from
    // grow(), called for each object's world bounds.
    void build(const Level& level, const Gol& gol);
    void grow(size_t object, const glm::vec3& min, const glm::vec3& max);

    // Zone of a level object (kNone: global, drawn whatever the zone).
    int object_zone(size_t object) const { return object < object_zone_.size() ? object_zone_[object] : kNone; }

    // Track the camera and compute the visible zones.
    void update(const glm::vec3& camera, const glm::mat4& view_proj);
    bool visible(int zone) const { return zone == kNone || (size_t(zone) < visible_.size() && visible_[size_t(zone)]); }
    int current() const { return current_; }

    bool set_portal_enabled(int id, bool enabled);
    // Put the camera in `zone` (teleports: CZonesManager::TeleportInZone with the spawn
    // point's zone); the next update keeps it rather than looking it up by position.
    void force_zone(int zone);
    // A door's portal (closed until set_door_open): a width x height quad in the XY
    // plane of `transform`, joining the zones with these ids.
    void add_door_portal(size_t object, int zone_a, int zone_b, const glm::mat4& transform, float width, float height);
    void set_door_open(size_t object, bool open);
    const std::vector<Zone>& zones() const { return zones_; }
    const std::vector<Portal>& portals() const { return portals_; }
    bool enabled = true;     // false: everything visible

private:
    int zone_at(const glm::vec3& p) const;
    static void make_quad(Portal& p, const glm::mat4& transform, float width, float height);
    void reach(int zone, const Frustum& frustum, int via, const glm::vec3& camera, int depth);

    std::vector<Zone> zones_;
    std::vector<Portal> portals_;
    std::unordered_map<int, int> zone_index_;   // zone id -> index
    std::vector<int> object_zone_;
    std::vector<uint8_t> visible_, portal_in_path_;
    int current_ = kNone;
    glm::vec3 last_camera_{0};
    bool have_camera_ = false;
};

}  // namespace nova
