#include "game/navmesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>
#include <queue>

namespace nova {

namespace {

float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

glm::vec2 closest_on_segment(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float t = len2 > 0 ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

}  // namespace

void Navigation::connect(int a, int b, int edge) {
    for (const Link& l : tris_[size_t(a)].links)
        if (l.tri == b && l.edge >= 0) return;
    tris_[size_t(a)].links.push_back({b, edge});
}

void Navigation::build(const Level& level) {
    verts_.clear();
    tris_.clear();
    std::unordered_map<int, std::vector<std::pair<int, glm::vec3>>> doors;   // door id -> (triangle, doorway point)
    for (const LevelObject& o : level.objects) {
        if (o.kind != "nav_mesh") continue;
        // Vertices are world-space; polygons are triangles with neighbour indices (-1: none).
        const int vbase = int(verts_.size()), tbase = int(tris_.size());
        for (const auto& v : o.nav.vertices) verts_.push_back(v);
        for (const auto& p : o.nav.polygons) {
            Tri t;
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                if (p[k] < 0 || p[k] >= int(o.nav.vertices.size())) ok = false;
                t.v[k] = vbase + p[k];
            }
            if (!ok) t.v[0] = t.v[1] = t.v[2] = vbase;   // keep indices aligned with the neighbour links
            tris_.push_back(t);
        }
        for (const auto& l : o.nav.links)
            if (l.polygon >= 0 && l.polygon < int(o.nav.polygons.size())) doors[l.door].push_back({tbase + l.polygon, l.point});
    }
    // Shared edges, by end point positions (within and across meshes).
    auto key = [](const glm::vec3& p) {
        return std::make_tuple(int(std::lround(p.x * 50)), int(std::lround(p.y * 50)), int(std::lround(p.z * 50)));
    };
    std::map<std::pair<std::tuple<int, int, int>, std::tuple<int, int, int>>, std::vector<std::pair<int, int>>> edges;
    for (size_t i = 0; i < tris_.size(); ++i) {
        const Tri& t = tris_[i];
        for (int e = 0; e < 3; ++e) {
            auto a = key(verts_[size_t(t.v[e])]), b = key(verts_[size_t(t.v[(e + 1) % 3])]);
            if (a == b) continue;
            if (b < a) std::swap(a, b);
            edges[{a, b}].push_back({int(i), e});
        }
    }
    for (const auto& [k, list] : edges)
        for (const auto& [ta, ea] : list)
            for (const auto& [tb, eb] : list)
                if (ta != tb) connect(ta, tb, ea);
    // Doors: every pair of meshes linking the same door.
    for (const auto& [door, list] : doors)
        for (const auto& [ta, pa] : list)
            for (const auto& [tb, pb] : list)
                if (ta != tb) tris_[size_t(ta)].links.push_back({tb, -1, door, pa, pb});
    if (tris_.empty()) return;
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (Tri& t : tris_) {
        const glm::vec3 a = verts_[size_t(t.v[0])], b = verts_[size_t(t.v[1])], c = verts_[size_t(t.v[2])];
        t.center = (a + b + c) / 3.0f;
        t.lo = glm::min(glm::min(glm::vec2(a), glm::vec2(b)), glm::vec2(c));
        t.hi = glm::max(glm::max(glm::vec2(a), glm::vec2(b)), glm::vec2(c));
        lo = glm::min(lo, t.lo), hi = glm::max(hi, t.hi);
    }
    origin_ = lo;
    grid_w_ = int((hi.x - lo.x) / kCell) + 1;
    grid_h_ = int((hi.y - lo.y) / kCell) + 1;
    grid_.assign(size_t(grid_w_) * size_t(grid_h_), {});
    for (size_t i = 0; i < tris_.size(); ++i) {
        const Tri& t = tris_[i];
        const int x0 = int((t.lo.x - origin_.x) / kCell), x1 = int((t.hi.x - origin_.x) / kCell);
        const int y0 = int((t.lo.y - origin_.y) / kCell), y1 = int((t.hi.y - origin_.y) / kCell);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) grid_[size_t(y) * size_t(grid_w_) + size_t(x)].push_back(int(i));
    }
}

std::vector<int> Navigation::candidates(const glm::vec2& p) const {
    const int x = int(std::floor((p.x - origin_.x) / kCell)), y = int(std::floor((p.y - origin_.y) / kCell));
    if (x < 0 || y < 0 || x >= grid_w_ || y >= grid_h_) return {};
    return grid_[size_t(y) * size_t(grid_w_) + size_t(x)];
}

bool Navigation::inside(const Tri& t, const glm::vec2& p) const {
    const glm::vec2 a(verts_[size_t(t.v[0])]), b(verts_[size_t(t.v[1])]), c(verts_[size_t(t.v[2])]);
    const float d1 = cross2(b - a, p - a), d2 = cross2(c - b, p - b), d3 = cross2(a - c, p - c);
    const bool neg = d1 < -1e-6f || d2 < -1e-6f || d3 < -1e-6f, pos = d1 > 1e-6f || d2 > 1e-6f || d3 > 1e-6f;
    return !(neg && pos);
}

float Navigation::height(int tri, const glm::vec2& p) const {
    const Tri& t = tris_[size_t(tri)];
    const glm::vec3 a = verts_[size_t(t.v[0])], b = verts_[size_t(t.v[1])], c = verts_[size_t(t.v[2])];
    const glm::vec3 n = glm::cross(b - a, c - a);
    if (std::abs(n.z) < 1e-6f) return t.center.z;
    return a.z - (n.x * (p.x - a.x) + n.y * (p.y - a.y)) / n.z;
}

int Navigation::locate(const glm::vec3& p, float max_dz) const {
    int best = -1;
    float best_dz = max_dz;
    for (int i : candidates(glm::vec2(p))) {
        const Tri& t = tris_[size_t(i)];
        if (p.x < t.lo.x || p.x > t.hi.x || p.y < t.lo.y || p.y > t.hi.y || !inside(t, glm::vec2(p))) continue;
        const float dz = std::abs(height(i, glm::vec2(p)) - p.z);
        if (dz <= best_dz) best = i, best_dz = dz;
    }
    return best;
}

int Navigation::nearest(const glm::vec3& p, float max_dist, glm::vec3& out) const {
    if (const int t = locate(p); t >= 0) {
        out = {p.x, p.y, height(t, glm::vec2(p))};
        return t;
    }
    int best = -1;
    float best_d = max_dist;
    const int r = int(std::ceil(max_dist / kCell));
    const int cx = int(std::floor((p.x - origin_.x) / kCell)), cy = int(std::floor((p.y - origin_.y) / kCell));
    for (int y = std::max(cy - r, 0); y <= std::min(cy + r, grid_h_ - 1); ++y) {
        for (int x = std::max(cx - r, 0); x <= std::min(cx + r, grid_w_ - 1); ++x) {
            for (int i : grid_[size_t(y) * size_t(grid_w_) + size_t(x)]) {
                const Tri& t = tris_[size_t(i)];
                glm::vec2 q(p);
                if (!inside(t, q)) {   // closest point on the boundary
                    float dmin = std::numeric_limits<float>::max();
                    for (int e = 0; e < 3; ++e) {
                        const glm::vec2 c = closest_on_segment(glm::vec2(p), glm::vec2(verts_[size_t(t.v[e])]),
                                                               glm::vec2(verts_[size_t(t.v[(e + 1) % 3])]));
                        if (glm::distance(c, glm::vec2(p)) < dmin) dmin = glm::distance(c, glm::vec2(p)), q = c;
                    }
                }
                const glm::vec3 on(q, height(i, q));
                const float d = glm::distance(on, p);
                if (d < best_d) best = i, best_d = d, out = on;
            }
        }
    }
    return best;
}

bool Navigation::find_path(int from_tri, const glm::vec3& from, int to_tri, const glm::vec3& to,
                           std::vector<glm::vec3>& out, const std::function<bool(int)>& door_open,
                           std::vector<uint8_t>* off_mesh) const {
    out.clear();
    if (off_mesh) off_mesh->clear();
    if (from_tri < 0 || to_tri < 0) return false;
    if (from_tri == to_tri) {
        out.push_back(to);
        if (off_mesh) off_mesh->push_back(0);
        return true;
    }
    // A* over triangles (cost: distance between centres; the goal's position as heuristic).
    const size_t n = tris_.size();
    std::vector<float> g(n, std::numeric_limits<float>::max());
    std::vector<int> came(n, -1);
    std::vector<const Link*> via(n, nullptr);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    g[size_t(from_tri)] = 0;
    open.push({glm::distance(tris_[size_t(from_tri)].center, to), from_tri});
    int expanded = 0;
    while (!open.empty() && expanded < 20000) {
        const int cur = open.top().second;
        open.pop();
        ++expanded;
        if (cur == to_tri) break;
        const Tri& t = tris_[size_t(cur)];
        for (const Link& l : t.links) {
            if (l.edge < 0 && door_open && !door_open(l.door)) continue;
            const int nb = l.tri;
            const float cost = g[size_t(cur)] + glm::distance(t.center, tris_[size_t(nb)].center);
            if (cost >= g[size_t(nb)]) continue;
            g[size_t(nb)] = cost;
            came[size_t(nb)] = cur;
            via[size_t(nb)] = &l;
            open.push({cost + glm::distance(tris_[size_t(nb)].center, to), nb});
        }
    }
    if (came[size_t(to_tri)] < 0) return false;
    std::vector<int> corridor;
    for (int t = to_tri; t >= 0; t = came[size_t(t)]) {
        corridor.push_back(t);
        if (t == from_tri) break;
    }
    std::reverse(corridor.begin(), corridor.end());

    // Portals (left, right) between consecutive triangles, as seen walking along the corridor.
    std::vector<std::pair<glm::vec3, glm::vec3>> portals;
    std::vector<glm::vec3> door_exits;
    portals.push_back({from, from});
    for (size_t i = 0; i + 1 < corridor.size(); ++i) {
        const Tri& t = tris_[size_t(corridor[i])];
        const Link& l = *via[size_t(corridor[i + 1])];
        if (l.edge < 0) {   // through a doorway: pass both of its points
            portals.push_back({l.from, l.from});
            portals.push_back({l.to, l.to});
            door_exits.push_back(l.to);
            continue;
        }
        glm::vec3 a = verts_[size_t(t.v[l.edge])], b = verts_[size_t(t.v[(l.edge + 1) % 3])];
        const glm::vec2 dir = glm::vec2(tris_[size_t(corridor[i + 1])].center - t.center);
        if (cross2(dir, glm::vec2(a - t.center)) < cross2(dir, glm::vec2(b - t.center))) std::swap(a, b);
        portals.push_back({a, b});   // a: left, b: right
    }
    portals.push_back({to, to});

    // Simple stupid funnel algorithm (Mononen), in XY.
    glm::vec3 apex = portals[0].first, left = portals[0].first, right = portals[0].second;
    size_t apex_i = 0, left_i = 0, right_i = 0;
    auto area = [](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        return cross2(glm::vec2(b - a), glm::vec2(c - a));   // > 0: c left of a->b
    };
    for (size_t i = 1; i < portals.size(); ++i) {
        const glm::vec3 pl = portals[i].first, pr = portals[i].second;
        // Tighten the right side.
        if (area(apex, right, pr) >= 0) {
            if (apex == right || area(apex, left, pr) < 0) {
                right = pr, right_i = i;
            } else {
                out.push_back(left);
                apex = left, apex_i = left_i;
                left = right = apex, left_i = right_i = apex_i;
                i = apex_i;
                continue;
            }
        }
        // Tighten the left side.
        if (area(apex, left, pl) <= 0) {
            if (apex == left || area(apex, right, pl) > 0) {
                left = pl, left_i = i;
            } else {
                out.push_back(right);
                apex = right, apex_i = right_i;
                left = right = apex, left_i = right_i = apex_i;
                i = apex_i;
                continue;
            }
        }
    }
    if (out.empty() || glm::distance(out.back(), to) > 1e-3f) out.push_back(to);
    if (off_mesh)
        for (const glm::vec3& p : out)
            off_mesh->push_back(std::any_of(door_exits.begin(), door_exits.end(), [&](const glm::vec3& d) { return d == p; }));
    return true;
}

}  // namespace nova
