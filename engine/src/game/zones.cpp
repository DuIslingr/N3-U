#include "game/zones.h"

#include <algorithm>
#include <cmath>

#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompZonesLink = 0x72;     // CComponentZonesLink (doors): zone a, zone b, flag
constexpr int kCompGlobal = 0x8A;        // objects the zones manager keeps global
constexpr int kMaxPortalDepth = 24;

glm::vec4 normalized_plane(const glm::vec4& p) {
    const float len = glm::length(glm::vec3(p));
    return len > 0 ? p / len : p;
}

glm::vec4 plane_through(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
    return {n, -glm::dot(n, a)};
}

}  // namespace

Frustum Frustum::from_view_proj(const glm::mat4& m) {
    // Rows of the matrix (glm is column-major); clip space z in [0, 1].
    auto row = [&](int r) { return glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]); };
    const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    Frustum f;
    f.planes = {normalized_plane(r3 + r0), normalized_plane(r3 - r0), normalized_plane(r3 + r1),
                normalized_plane(r3 - r1), normalized_plane(r2), normalized_plane(r3 - r2)};
    return f;
}

bool Frustum::sees(const glm::vec3* points, size_t n) const {
    for (const glm::vec4& p : planes) {
        size_t outside = 0;
        for (size_t i = 0; i < n; ++i) outside += glm::dot(glm::vec3(p), points[i]) + p.w < 0;
        if (outside == n) return false;
    }
    return true;
}

void ZoneSystem::build(const Level& level, const Gol& gol) {
    zones_.clear();
    portals_.clear();
    zone_index_.clear();
    object_zone_.assign(level.objects.size(), kNone);
    current_ = kNone;
    have_camera_ = false;

    // Membership: an object belongs to the zone (or meta zone) record before it.
    int current = kNone;
    std::vector<std::pair<int, std::vector<int>>> meta_children;   // zone index, child zone ids
    for (size_t i = 0; i < level.objects.size(); ++i) {
        const LevelObject& o = level.objects[i];
        if ((o.kind == "zone" || o.kind == "meta_zone") && o.base) {
            Zone z;
            z.id = o.base->id;
            if (o.kind == "zone" && o.fields.size() >= 9 && o.fields[1].i != 0) {
                // Fog colour as an SColor's bytes (B, G, R, A), like the level properties.
                auto byte = [&](size_t k) { return float(uint8_t(o.fields[k].i)) / 255.0f; };
                z.fog.override = true;
                z.fog.color = {byte(4), byte(3), byte(2), 1};
                z.fog.start = o.fields[6].as_float();
                z.fog.end = o.fields[7].as_float();
                z.fog.density = o.fields[8].as_float();
            }
            current = int(zones_.size());
            zone_index_[z.id] = current;
            if (o.kind == "meta_zone" && o.fields.size() >= 1) {
                std::vector<int> ids;
                for (const Value& c : o.fields[0].list) ids.push_back(c.list.empty() ? c.i : c[0].i);   // [i] groups
                meta_children.emplace_back(current, ids);
            }
            zones_.push_back(z);
            continue;
        }
        if (o.kind == "zone_end") {
            current = kNone;
            continue;
        }
        if (o.kind == "portal" && o.base && o.fields.size() >= 5) {
            Portal p;
            p.id = o.base->id;
            p.zone[0] = o.fields[0].i;   // zone ids, resolved below
            p.zone[1] = o.fields[1].i;
            p.enabled = o.base->visible;
            // CZonePortal::Init: a width x height quad in the portal's XY plane.
            const float w = o.fields[3].as_float(), h = o.fields[4].as_float();
            make_quad(p, placement_matrix(*o.base), w, h);
            portals_.push_back(p);
            continue;
        }
        if (o.kind != "object") {
            object_zone_[i] = current;   // spawn points, waypoints...: their zone, for teleports
            continue;
        }
        bool global = o.components.count(kCompGlobal) != 0;
        if (auto it = o.components.find(kCompZonesLink); it != o.components.end() && it->second.size() >= 2)
            global = global || (it->second[0].i > 0 && it->second[1].i > 0);
        (void)gol;
        object_zone_[i] = global ? kNone : current;
    }
    // A meta zone holds objects shared by several zones (CZone::CreateMetaZone): each
    // listed zone shows it whenever that zone is visible.
    for (auto& [meta, ids] : meta_children) {
        for (int id : ids)
            if (auto it = zone_index_.find(id); it != zone_index_.end() && it->second != meta) zones_[size_t(it->second)].children.push_back(meta);
    }
    // Resolve portal zones; portals that do not join two known zones are dropped.
    std::vector<Portal> kept;
    for (Portal p : portals_) {
        auto a = zone_index_.find(p.zone[0]), b = zone_index_.find(p.zone[1]);
        if (a == zone_index_.end() || b == zone_index_.end()) continue;
        p.zone[0] = a->second;
        p.zone[1] = b->second;
        zones_[size_t(p.zone[0])].portals.push_back(int(kept.size()));
        zones_[size_t(p.zone[1])].portals.push_back(int(kept.size()));
        kept.push_back(p);
    }
    portals_ = std::move(kept);
    visible_.assign(zones_.size(), 1);
    portal_in_path_.assign(portals_.size(), 0);
}

void ZoneSystem::grow(size_t object, const glm::vec3& min, const glm::vec3& max) {
    const int z = object_zone(object);
    if (z == kNone) return;
    zones_[size_t(z)].min = glm::min(zones_[size_t(z)].min, min);
    zones_[size_t(z)].max = glm::max(zones_[size_t(z)].max, max);
}

int ZoneSystem::zone_at(const glm::vec3& p) const {
    // CZonesManager::GetZoneFromPos: the first zone whose bounds contain the point.
    for (size_t i = 0; i < zones_.size(); ++i) {
        const Zone& z = zones_[i];
        if (glm::all(glm::greaterThanEqual(p, z.min)) && glm::all(glm::lessThanEqual(p, z.max))) return int(i);
    }
    return kNone;
}

void ZoneSystem::make_quad(Portal& p, const glm::mat4& m, float w, float h) {
    const glm::vec2 local[4] = {{w / 2, h / 2}, {-w / 2, h / 2}, {-w / 2, -h / 2}, {w / 2, -h / 2}};
    for (int k = 0; k < 4; ++k) p.corners[size_t(k)] = glm::vec3(m * glm::vec4(local[k], 0, 1));
    p.center = (p.corners[0] + p.corners[1] + p.corners[2] + p.corners[3]) * 0.25f;
    p.plane = plane_through(p.corners[0], p.corners[1], p.corners[2]);
}

void ZoneSystem::add_door_portal(size_t object, int zone_a, int zone_b, const glm::mat4& transform, float width,
                                 float height) {
    auto a = zone_index_.find(zone_a), b = zone_index_.find(zone_b);
    if (a == zone_index_.end() || b == zone_index_.end() || a->second == b->second) return;
    Portal p;
    p.id = -int(object) - 1;
    p.zone[0] = a->second;
    p.zone[1] = b->second;
    p.enabled = false;
    p.door = object;
    make_quad(p, transform, width, height);
    const int index = int(portals_.size());
    zones_[size_t(p.zone[0])].portals.push_back(index);
    zones_[size_t(p.zone[1])].portals.push_back(index);
    portals_.push_back(p);
    portal_in_path_.assign(portals_.size(), 0);
}

void ZoneSystem::set_door_open(size_t object, bool open) {
    for (Portal& p : portals_)
        if (p.door == object) p.enabled = open;
}

void ZoneSystem::force_zone(int zone) {
    current_ = zone;
    have_camera_ = false;
}

bool ZoneSystem::set_portal_enabled(int id, bool on) {
    for (Portal& p : portals_)
        if (p.id == id) {
            p.enabled = on;
            return true;
        }
    return false;
}

void ZoneSystem::reach(int zone, const Frustum& frustum, int via, const glm::vec3& camera, int depth) {
    visible_[size_t(zone)] = 1;
    for (int c : zones_[size_t(zone)].children)
        if (!visible_[size_t(c)]) reach(c, frustum, kNone, camera, depth + 1);
    if (depth >= kMaxPortalDepth) return;
    for (int pi : zones_[size_t(zone)].portals) {
        Portal& p = portals_[size_t(pi)];
        if (pi == via || !p.enabled || portal_in_path_[size_t(pi)]) continue;
        if (!frustum.sees(p.corners.data(), 4)) continue;
        const int other = p.zone[0] == zone ? p.zone[1] : p.zone[0];
        // Narrow the frustum to the portal (CZonePortal::IntersectsFrustum): planes through
        // the camera and each edge, facing the portal's centre. Next to the portal the
        // edges degenerate; pass the frustum on unchanged.
        Frustum narrowed = frustum;
        const float dist = glm::dot(glm::vec3(p.plane), camera) + p.plane.w;
        if (std::fabs(dist) > 0.25f) {
            for (int k = 0; k < 4; ++k) {
                glm::vec4 plane = plane_through(camera, p.corners[size_t(k)], p.corners[size_t((k + 1) % 4)]);
                if (glm::dot(glm::vec3(plane), p.center) + plane.w < 0) plane = -plane;
                narrowed.planes.push_back(plane);
            }
        }
        portal_in_path_[size_t(pi)] = 1;
        reach(other, narrowed, pi, camera, depth + 1);
        portal_in_path_[size_t(pi)] = 0;
    }
}

void ZoneSystem::update(const glm::vec3& camera, const glm::mat4& view_proj) {
    if (!enabled || zones_.empty()) {
        visible_.assign(zones_.size(), 1);
        return;
    }
    // CZone::CheckChangedZone: moving through an enabled portal of the current zone
    // puts the camera in the zone on the other side. Jumps (teleports) look the zone
    // up again by position.
    if (current_ != kNone && have_camera_ && glm::distance(camera, last_camera_) < 4.0f) {
        for (int pi : zones_[size_t(current_)].portals) {
            const Portal& p = portals_[size_t(pi)];
            if (!p.enabled) continue;
            const float d0 = glm::dot(glm::vec3(p.plane), last_camera_) + p.plane.w;
            const float d1 = glm::dot(glm::vec3(p.plane), camera) + p.plane.w;
            if ((d0 > 0) == (d1 > 0) || d0 == d1) continue;
            const glm::vec3 hit = last_camera_ + (camera - last_camera_) * (d0 / (d0 - d1));
            // Inside the quad: on the inner side of every edge.
            bool inside = true;
            const glm::vec3 n(p.plane);
            for (int k = 0; k < 4 && inside; ++k) {
                const glm::vec3 a = p.corners[size_t(k)], b = p.corners[size_t((k + 1) % 4)];
                inside = glm::dot(glm::cross(b - a, hit - a), n) >= 0;
            }
            if (!inside) continue;
            current_ = p.zone[0] == current_ ? p.zone[1] : p.zone[0];
            break;
        }
    } else if (have_camera_ || current_ == kNone) {   // a jump, or the first frame without a forced zone
        current_ = zone_at(camera);
    }
    if (current_ == kNone) current_ = zone_at(camera);
    last_camera_ = camera;
    have_camera_ = true;

    if (current_ == kNone) {   // outside every zone: draw everything rather than nothing
        visible_.assign(zones_.size(), 1);
        return;
    }
    visible_.assign(zones_.size(), 0);
    reach(current_, Frustum::from_view_proj(view_proj), kNone, camera, 0);
}

}  // namespace nova
