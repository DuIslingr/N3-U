#include "game/viewmodel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompStatesSet = 0x0e;
constexpr int kCompEffects = 0x92;
constexpr int kCompWeapon = 0x47;     // [1]: the rig's WEAPON_* node for this weapon
constexpr float kDefaultAimFov = 35;   // CComponentWeapon [45] of most weapons

const GolType* type_named(const Gol& gol, const std::string& name, int* id = nullptr) {
    for (const auto& [i, t] : gol)
        if (t.name == name) {
            if (id) *id = i;
            return &t;
        }
    return nullptr;
}

}  // namespace

const ViewModel::Clips& ViewModel::clips(const std::string& weapon) {
    if (auto it = clips_.find(weapon); it != clips_.end()) return it->second;
    Clips& c = clips_[weapon];
    const GolType* t = type_named(gol_, weapon);
    if (!t) return c;
    if (const Value* w = resolved_template(*t, gol_, kCompWeapon); w && w->size() > 4) {
        if ((*w)[1].type == Value::String) c.node = (*w)[1].s;
        if (w->size() > 71 && weapon != "Crystal_ItemWeapon")   // drawWeapons leaves the crystal's alone
            c.aim_fov = (*w)[45].as_float(), c.model_fov = (*w)[70].as_float(), c.model_aim_fov = (*w)[71].as_float(),
            c.zoom = (*w)[46].i != 0, c.zoom_from = (*w)[47].as_float(), c.zoom_to = (*w)[48].as_float();
        // Fire mode: [9] muzzle object, [10] its ironsight one, [11] the node.
        if (const Value& modes = (*w)[4]; modes.size() > 0 && modes[0].size() > 11) {
            c.muzzle_type = modes[0][9].i, c.aim_muzzle_type = modes[0][10].i;
            if (modes[0][11].type == Value::String) c.muzzle_node = modes[0][11].s;
        }
    }
    if (const Value* fx = resolved_template(*t, gol_, kCompEffects); fx && fx->size() > 0)
        for (const Value& e : (*fx)[0].list)
            if (e.size() > 2 && e[0].type == Value::String && e[1].type == Value::String)
                c.fx.push_back({e[0].s, e[1].s, e[2].i != 0});
    // Each state: [0] name, [1] type, [2] loops, [6] the next state's index, [11] clips
    // ({name, speed, ...}).
    if (const Value* v = resolved_template(*t, gol_, kCompStatesSet); v && v->size() > 1)
        for (const Value& st : (*v)[1].list) {
            State s;
            if (st.size() > 0 && st[0].type == Value::String) s.name = st[0].s;
            if (st.size() > 6) s.type = int(st[1].as_float()), s.loop = st[2].as_float() != 0, s.next = int(st[6].as_float());
            if (st.size() > 11)
                for (const Value& clip : st[11].list) {
                    if (clip.size() < 2 || clip[0].type != Value::String) continue;
                    s.clips.emplace_back(clip[0].s, clip[1].as_float());
                    // [2]: {"GameEvent", ..., {{name, start ms, end ms, id, argument}}}
                    if (clip.size() > 2)
                        for (const Value& group : clip[2].list)
                            if (group.size() >= 3 && group[0].type == Value::String && group[0].s == "GameEvent")
                                for (const Value& ev : group[2].list)
                                    if (ev.size() >= 5) c.events[clip[0].s].push_back({ev[1].as_float(), ev[3].i, ev[4].i});
                }
            c.index.emplace(s.name, int(c.states.size()));
            c.states.push_back(std::move(s));
        }
    return c;
}

void ViewModel::start() {
    int type = -1;
    if (!type_named(gol_, "AssaultRifle_Player", &type)) return;
    object_ = scene_.spawn_object(type, -1000, glm::vec3(0), 0);
    scene_.set_object_viewmodel(object_);
    cockpit_ = size_t(-1), cockpit_visible_ = false, cockpit_playing_.clear();
    if (type_named(gol_, "Player_Mech_Cockpit", &type)) {
        cockpit_ = scene_.spawn_object(type, -1001, glm::vec3(0), 0);
        scene_.set_object_viewmodel(cockpit_);
        scene_.set_object_visible(cockpit_, false);
    }
    // CPlayerMechComponent::Init: the mech's shield (HUDShield_Mech) on the cockpit's
    // dummy_link_shield_mech, its "shield_mech" clip posed by the health
    // (UpdateShieldPercent).
    mech_shield_ = size_t(-1), mech_shield_shown_ = -1;
    if (cockpit_ != size_t(-1) && type_named(gol_, "HUDShield_Mech", &type)) {
        mech_shield_ = scene_.spawn_object(type, -1002, glm::vec3(0), 0);
        scene_.set_object_viewmodel(mech_shield_);
        scene_.play_clip(mech_shield_, "shield_mech", false);
        scene_.set_object_visible(mech_shield_, false);
    }
    muzzles_.clear(), flashing_ = -1;   // their objects went with the old level
    fx_.clear(), fx_weapon_.clear(), events_serial_ = 0;
    weapon_.clear(), base_.clear(), requested_.clear(), queued_.clear(), movement_.clear();
    sway_ = sway_start_ = glm::vec2(0), sway_left_ = 0, sway_view_ = false, sway_layer_ = false;
    camera_request_.clear(), camera_clip_.clear(), camera_offset_ = glm::mat4(1);
    visible_ = false, lowered_ = false, was_reloading_ = false;
    current_ = -1, left_ = 0, playing_.clear();
    distort_ = false;
}

void ViewModel::arms_distort(bool on) {
    if (on == distort_ || object_ == size_t(-1)) return;
    distort_ = on && scene_.set_object_material(object_, "hand_mesh_new_distort.bdae", "armsDistort");
    if (!on) scene_.set_object_material(object_, "", "");
    distort_blend_ = -0.3f, distort_target_ = 0, distort_ms_ = 0;
}

void ViewModel::arms_distort_blend(float target, float ms) {
    distort_target_ = target, distort_ms_ = ms;
    if (ms < 1) distort_blend_ = target;
}

void ViewModel::show_weapon(const std::string& node) {
    weapon_hidden_ = false;
    // The weapons are the WEAPON_* nodes not inside another one (the handgun keeps its mesh
    // and magazine in WEAPON_handgun and WEAPON_Handgun_charger under WEAPON_Handgun_R).
    std::vector<std::string> weapons;
    for (const std::string& n : scene_.node_names(object_))
        if (n.rfind("WEAPON_", 0) == 0 && n.find("_PIVOT") == std::string::npos) weapons.push_back(n);
    glm::vec3 at;
    for (const std::string& n : weapons) {
        const bool nested = std::any_of(weapons.begin(), weapons.end(), [&](const std::string& m) {
            return m != n && scene_.node_position(object_, m + "/" + n, at);
        });
        if (!nested) scene_.set_node_visible(object_, n, n == node);
    }
}

void ViewModel::set_state(const std::string& state) { requested_ = state; }

void ViewModel::set_weapon_hidden(const Clips& c, bool hidden) {
    if (weapon_hidden_ == hidden) return;
    weapon_hidden_ = hidden;
    if (hidden) scene_.set_node_visible(object_, c.node, false);
    else show_weapon(c.node);
}

float ViewModel::aim_fov() { return weapon_.empty() ? kDefaultAimFov : clips(weapon_).aim_fov; }

bool ViewModel::zoom_range(float& widest, float& narrowest) {
    if (weapon_.empty()) return false;
    const Clips& c = clips(weapon_);
    widest = std::max(c.zoom_from, c.zoom_to), narrowest = std::min(c.zoom_from, c.zoom_to);
    return c.zoom;
}

float ViewModel::cockpit_fov_factor() const {
    Scene::CameraView hands, cockpit;
    if (object_ == size_t(-1) || cockpit_ == size_t(-1) || !scene_.camera_view(object_, hands) || !scene_.camera_view(cockpit_, cockpit) ||
        hands.fov <= 0)
        return 1;
    return cockpit.fov / hands.fov;
}

float ViewModel::model_fov_add() {
    if (weapon_.empty() || !visible_) return 0;
    const Clips& c = clips(weapon_);
    const float normal = c.model_fov - 70, aimed = c.model_aim_fov - 70;
    const bool in = playing_ == "Aim", out = playing_ == "UnAim" || playing_ == "AimToRush";
    Scene::ClipState cs;
    if ((in || out) && left_ > 0 && scene_.clip_state(object_, cs) && cs.length > 1) {
        const float k = std::clamp(cs.reverse ? 1 - cs.ms / cs.length : cs.ms / cs.length, 0.0f, 1.0f);
        return in ? normal + (aimed - normal) * k : aimed + (normal - aimed) * k;
    }
    return aiming_ ? aimed : normal;
}

bool ViewModel::play(const Clips& c, const std::string& state, float fade_ms) {
    auto it = c.index.find(state);
    if (it == c.index.end()) return false;
    const State& s = c.states[size_t(it->second)];
    if (s.clips.empty()) return false;
    const auto& [clip, speed] = s.clips[s.clips.size() > 1 ? size_t(std::rand()) % s.clips.size() : 0];
    const float rate = speed == 0 ? 1.0f : std::fabs(speed);   // 0: the postures, posed by the view's turn
    if (!scene_.play_clip(object_, clip, s.loop, speed < 0, fade_ms)) return false;
    scene_.set_clip_speed(object_, rate);
    current_ = it->second, playing_ = state;
    Scene::ClipState cs;
    left_ = s.loop ? 0.0f : (scene_.clip_state(object_, cs) ? cs.length / 1000.0f : 0.5f) / rate;
    return true;
}

void ViewModel::update(float dt, const glm::vec3& eye, float yaw_deg, float pitch_deg, const Input& in) {
    if (object_ == size_t(-1)) return;
    // The mech's cockpit: its own rig and states.
    const bool cockpit = in.visible && in.cockpit && cockpit_ != size_t(-1);
    if (cockpit_ != size_t(-1) && cockpit != cockpit_visible_) {
        scene_.set_object_visible(cockpit_, cockpit), cockpit_visible_ = cockpit;
        if (mech_shield_ != size_t(-1)) scene_.set_object_visible(mech_shield_, cockpit);
    }
    if (cockpit) {
        if (visible_) scene_.set_object_visible(object_, false), visible_ = false;
        const Clips& c = clips("Player_Mech_Cockpit");
        std::string want = in.firing ? c.get("Attack") : in.moving ? c.get("CockpitWalk") : c.get("CockpitIdle");
        if (want.empty()) want = c.get("CockpitIdle");
        if (!want.empty() && want != cockpit_playing_) scene_.play_clip(cockpit_, want, true), cockpit_playing_ = want;
        place(cockpit_, eye, yaw_deg, pitch_deg);
        if (mech_shield_ != size_t(-1)) {
            scene_.set_object_transform(mech_shield_, scene_.object_matrix(cockpit_, "dummy_link_shield_mech"));
            if (std::abs(in.mech_shield - mech_shield_shown_) > 1e-3f) {
                mech_shield_shown_ = in.mech_shield;
                scene_.set_clip_time(mech_shield_, std::clamp(in.mech_shield, 0.0f, 1.0f) * scene_.clip_length(mech_shield_, "shield_mech") * 0.999f);
            }
        }
        camera_request_.clear(), camera_clip_.clear(), camera_offset_ = glm::mat4(1);
        stop_all_fx(), fx_weapon_.clear();
        return;
    }
    if (in.visible != visible_) scene_.set_object_visible(object_, in.visible), visible_ = in.visible;
    if (!in.visible) {
        camera_request_.clear(), camera_clip_.clear(), camera_offset_ = glm::mat4(1);
        stop_all_fx(), fx_weapon_.clear();
        return;
    }
    const Clips& c = clips(in.weapon);
    aiming_ = in.aiming;
    if (!in.aiming && weapon_hidden_) set_weapon_hidden(c, false);   // UnAim: SetVisible
    update_camera_layer(dt, c);
    update_sway(dt, yaw_deg, pitch_deg, in.aiming, c);
    if (distort_) {   // PlayerComponent::distort_update
        if (distort_ms_ > 0) {
            distort_ms_ -= dt * 1000.0f;
            distort_blend_ += (distort_target_ - distort_blend_) * (1.0f - std::pow(0.75f, dt * 30.0f));
            if (distort_ms_ < 1) distort_blend_ = distort_target_;
        }
        scene_.set_object_param(object_, "BlendFactor", glm::vec4(distort_blend_), 1);
    }
    // What is playing: a state that does not loop holds the rest off, except the moves
    // between the input's own states (aiming, sprinting, the fire's start and end, jumps,
    // landings), which the next of those cut short.
    const State* cur = current_ >= 0 && size_t(current_) < c.states.size() ? &c.states[size_t(current_)] : nullptr;
    left_ = std::max(left_ - dt, 0.0f);
    auto soft = [](const State* s) {
        if (!s || s->loop) return true;
        switch (s->type) {
            case 3: case 4: case 10: case 11: case 24: case 26: case 37: case 38: return true;
            // 42 also holds GetWeapon and Weapon_Show_Off, which play through.
            default: return s->name == "StartRush" || s->name == "EndRush" || s->name == "AimToRush";
        }
    };
    if (in.weapon != weapon_) {   // switched: the new weapon comes up
        weapon_ = in.weapon;
        sway_ = sway_start_ = glm::vec2(0), sway_left_ = 0;
        show_weapon(c.node);
        lowered_ = false, queued_.clear();
        if (!play(c, "GetUp")) play(c, "GetWeapon");
    }
    if (!requested_.empty()) {
        const std::string r = std::exchange(requested_, {});
        if (r == "GetDown") {
            lowered_ = play(c, "GetDown");   // then Hidden: held down
        } else if (r == "ThrowGrenade") {   // the throw, then back to the weapon
            lowered_ = false;
            if (play(c, "ThrowGrenade")) queued_ = "ThrowGrenade_End";
        } else if (r == "Idle") {
            lowered_ = false, left_ = 0;
        } else {
            lowered_ = false;
            if (!play(c, r) && r == "UnAim") left_ = 0;
        }
    }
    if (in.reloading && !was_reloading_) play(c, "Reload");
    was_reloading_ = in.reloading;
    cur = current_ >= 0 && size_t(current_) < c.states.size() ? &c.states[size_t(current_)] : nullptr;
    const bool busy = left_ > 0 && !soft(cur);
    // CWeaponManager::StartLanding plays only over an idle state (type 3), StartJump over an
    // idle or walking one (4).
    if (!movement_.empty()) {
        const bool landing = movement_.rfind("Landing", 0) == 0;
        const int type = cur && (cur->loop || left_ > 0) ? cur->type : 3;
        if (!lowered_ && !in.reloading && (type == 3 || (!landing && type == 4))) play(c, movement_);
        movement_.clear();
    }

    // CWeaponComponent::FireWeapon: each shot sets the attack state (AimAttack while aiming;
    // AttackWalk walking, if the weapon has one), started over unless its clip is still
    // playing (SetMainAnimationState, mode 2): a fire start (Attack -> Attack_Loop) kicks again
    // at every shot.
    auto attacking = [](const State* s) { return s && (s->type == 12 || s->type == 37 || s->type == 39); };
    if (in.shot && !lowered_ && !in.reloading && !busy) {
        const std::string attack = in.aiming ? "AimAttack" : in.moving && c.has("AttackWalk") ? "AttackWalk" : "Attack";
        auto it = c.index.find(attack);
        if (it != c.index.end()) {
            const State& a = c.states[size_t(it->second)];
            Scene::ClipState cs;
            const bool same = !a.clips.empty() && scene_.clip_state(object_, cs) && cs.name == a.clips[0].first &&
                              (left_ > 0 || (cur && cur->loop));
            if (!same) play(c, attack);
        }
        cur = current_ >= 0 && size_t(current_) < c.states.size() ? &c.states[size_t(current_)] : nullptr;
    }
    // CWeaponManager::FireButtonRelease / FireStop: an attack ends into the stop state
    // (AimStopAttack while aiming, Attack_End); CWeaponComponent::Update: a fire loop (type 37)
    // with none goes straight to the idle. Faded: the fire clip is cut mid-way.
    bool stopped = false;
    if (!in.firing && !lowered_ && attacking(cur) && (was_firing_ || cur->type == 37)) {
        constexpr float kFireCutMs = 120;
        stopped = play(c, in.aiming ? "AimStopAttack" : "Attack_End", kFireCutMs) ||
                  (cur->type == 37 && play(c, in.aiming ? "AimIdle" : "Idle", kFireCutMs));
        cur = current_ >= 0 && size_t(current_) < c.states.size() ? &c.states[size_t(current_)] : nullptr;
    }
    was_firing_ = in.firing;
    // The looping state the input asks for, and the move into it from the last one; while
    // shooting, the attack playing (and what it leads to) stands.
    std::string base;
    if (in.firing && attacking(cur)) base = cur->name;
    else if (in.aiming) base = in.moving ? "AimWalk" : "AimIdle";
    else if (in.moving && in.running && c.has("RushWalk")) base = "RushWalk";
    else base = in.moving ? "Walk" : "Idle";
    if (!c.has(base)) base = in.aiming && c.has("AimIdle") ? "AimIdle" : "Idle";
    if (base != base_ && !base_.empty() && !lowered_ && !busy && !stopped) {
        const bool was_aim = base_.rfind("Aim", 0) == 0, aim = base.rfind("Aim", 0) == 0;
        std::string move;
        if (aim && !was_aim) move = "Aim";
        else if (was_aim && !aim) move = base == "RushWalk" ? "AimToRush" : "UnAim";
        else if (attacking(c.find(base)) || attacking(c.find(base_))) move = "";
        else if (base == "RushWalk") move = "StartRush";
        else if (base_ == "RushWalk") move = "EndRush";
        if (!move.empty()) play(c, move);
    }
    base_ = base;
    cur = current_ >= 0 && size_t(current_) < c.states.size() ? &c.states[size_t(current_)] : nullptr;
    if (lowered_ || (cur && !cur->loop && left_ > 0)) {
        place(object_, eye, yaw_deg, pitch_deg);
        return;
    }
    // A state that ended: a queued one, its next (a chain such as Attack -> Attack_Loop,
    // when that is still wanted), or the input's.
    if (!queued_.empty()) {
        const std::string q = std::exchange(queued_, {});
        if (play(c, q)) {
            place(object_, eye, yaw_deg, pitch_deg);
            return;
        }
    }
    if (cur && !cur->loop && cur->next > 1 && size_t(cur->next) < c.states.size()) {
        const State& n = c.states[size_t(cur->next)];
        if (n.clips.empty()) {   // Hidden
            lowered_ = true;
            place(object_, eye, yaw_deg, pitch_deg);
            return;
        }
        if ((!n.loop || n.name == base || (in.firing && attacking(&n))) && play(c, n.name)) {
            place(object_, eye, yaw_deg, pitch_deg);
            return;
        }
    }
    if (!cur || cur->name != base || (!cur->loop && left_ <= 0)) play(c, base);
    place(object_, eye, yaw_deg, pitch_deg);
}

void ViewModel::update_camera_layer(float dt, const Clips& c) {
    if (!camera_request_.empty()) {
        const State* s = c.find(camera_request_);
        camera_request_.clear();
        if (s && !s->clips.empty()) camera_clip_ = s->clips[size_t(std::rand()) % s->clips.size()].first, camera_ms_ = 0;
    }
    camera_offset_ = glm::mat4(1);
    if (camera_clip_.empty()) return;
    camera_ms_ += dt * 1000.0f;
    glm::mat4 from, now;
    float length = 0;
    if (!scene_.sample_node(object_, "Camera01", camera_clip_, 0, from, &length) ||
        !scene_.sample_node(object_, "Camera01", camera_clip_, camera_ms_, now) || camera_ms_ >= length) {
        camera_clip_.clear();
        return;
    }
    // In the camera's frame (looking down -Z, +Y up), then turned into the view's.
    const glm::mat4 turn = glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(1, 0, 0));
    camera_offset_ = glm::inverse(turn) * glm::inverse(from) * now * turn;
}

void ViewModel::place(size_t rig, const glm::vec3& eye, float yaw_deg, float pitch_deg) {
    // The rig's camera (Collada: looking down -Z) turned to look along +Y, in rig space.
    glm::mat4 placement = scene_.object_matrix(rig);
    const glm::mat4 turn = glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(1, 0, 0));
    const glm::mat4 cam_world = scene_.object_matrix(rig, "Camera01");
    glm::mat4 cam_rig = glm::inverse(placement) * cam_world * turn;
    const glm::mat4 view = glm::rotate(glm::rotate(glm::translate(glm::mat4(1), eye), glm::radians(yaw_deg), glm::vec3(0, 0, 1)),
                                       glm::radians(pitch_deg), glm::vec3(1, 0, 0));
    // The game sees through the rig's animated camera: placed by where it rests, the view
    // moved by the rest of the way (camera_offset).
    if (glm::mat4 rest; rig == object_ && !weapon_.empty() &&
                        scene_.sample_node(rig, "Camera01", clips(weapon_).get("Idle"), 0, rest)) {
        // (sample_node: in its parent's frame.)
        const glm::mat4 rest_rig = glm::inverse(placement) * scene_.object_matrix(rig, "dummy_camera") * rest * turn;
        camera_offset_ = glm::inverse(rest_rig) * cam_rig * camera_offset_;
        cam_rig = rest_rig;
    }
    scene_.set_object_transform(rig, view * glm::inverse(cam_rig));
    if (rig != object_) return;
    place_muzzle();
    if (weapon_.empty()) return;
    const Clips& c = clips(weapon_);
    update_events(c);
    if (fx_weapon_ == weapon_) {
        for (size_t i = 0; i < fx_.size(); ++i)
            if (fx_[i] >= 0) scene_.particles().move(fx_[i], fx_matrix(c, int(i)));
    }
}

void ViewModel::update_sway(float dt, float yaw_deg, float pitch_deg, bool aiming, const Clips& c) {
    // At most 0.126 rad behind (0.021 aiming); once the view holds still it catches up
    // over 1 / 0.012 ms (twice as long aiming).
    const float most = aiming ? 0.020944f : 0.125664f;
    const float yaw = glm::radians(yaw_deg), pitch = glm::radians(pitch_deg);
    if (!sway_view_) last_yaw_ = yaw, last_pitch_ = pitch, sway_view_ = true;
    const glm::vec2 turn(std::remainder(yaw - last_yaw_, glm::two_pi<float>()), pitch - last_pitch_);
    last_yaw_ = yaw, last_pitch_ = pitch;
    if (turn.x != 0 || turn.y != 0) {   // a new view: the weapon starts from where it was
        glm::vec2 lag = sway_ - turn;
        if (const float l = glm::length(lag); l > most) lag *= most / l;
        sway_start_ = lag, sway_left_ = 1;
    } else {
        sway_left_ = std::max(sway_left_ - 0.012f * dt * 1000.0f * (aiming ? 0.5f : 1.0f), 0.0f);
    }
    sway_ = sway_start_ * sway_left_;
    // The posture blender (CStatesSetComponent::SetCircularBlenderPosition): the two
    // direction clips either side of the lag's direction, each that far in (0.95 of its
    // length at the most), as turns of the arms from the centre posture.
    const float m = std::min(glm::length(sway_) / most, 1.0f);
    if (m < 0.01f) {
        if (sway_layer_) scene_.set_additive(object_, {}), sway_layer_ = false;
        return;
    }
    const std::string pre = aiming ? "Posture_Aim_" : "Posture_";
    static const char* const kDirs[4] = {"Left", "Up", "Right", "Down"};   // anticlockwise from the left
    float a = std::atan2(sway_.y, sway_.x) / glm::half_pi<float>();
    if (a < 0) a += 4;
    const int k = int(a) % 4;
    const float w = a - std::floor(a);
    std::vector<glm::quat> centre, pa, pb;
    float la = 0, lb = 0;
    const std::string ca = c.get(pre + kDirs[k]), cb = c.get(pre + kDirs[(k + 1) % 4]), cc = c.get(pre + "Center");
    if (ca.empty() || cb.empty() || cc.empty() || !scene_.sample_rotations(object_, cc, 0, centre) ||
        !scene_.sample_rotations(object_, ca, 0, pa, &la) || !scene_.sample_rotations(object_, cb, 0, pb, &lb))
        return;
    scene_.sample_rotations(object_, ca, m * 0.95f * la, pa);
    scene_.sample_rotations(object_, cb, m * 0.95f * lb, pb);
    std::vector<glm::quat> turn_by(centre.size());
    for (size_t n = 0; n < centre.size(); ++n) {
        const glm::quat inv = glm::inverse(centre[n]);
        turn_by[n] = glm::normalize(glm::slerp(inv * pa[n], inv * pb[n], w));
    }
    scene_.set_additive(object_, std::move(turn_by));
    sway_layer_ = true;
}

glm::mat4 ViewModel::fx_matrix(const Clips& c, int index) const {
    const std::string& node = c.fx[size_t(index)].node;
    return scene_.object_matrix(object_, node.empty() || node == "-1" ? c.node : c.node + "/" + node);
}

void ViewModel::start_fx(const Clips& c, int index) {
    if (index < 0 || size_t(index) >= c.fx.size()) return;
    if (fx_.size() < c.fx.size()) fx_.resize(c.fx.size(), -1);
    stop_fx(index);
    fx_[size_t(index)] = scene_.particles().start(c.fx[size_t(index)].effect, fx_matrix(c, index));
}

void ViewModel::stop_fx(int index) {
    if (index < 0 || size_t(index) >= fx_.size() || fx_[size_t(index)] < 0) return;
    scene_.particles().stop(fx_[size_t(index)]);
    fx_[size_t(index)] = -1;
}

void ViewModel::stop_all_fx() {
    for (size_t i = 0; i < fx_.size(); ++i) stop_fx(int(i));
}

// The playing clip's GameEvents passed since the last update (CEffectsComponent::OnEvent:
// 50 starts the weapon's effect [argument], 51 stops it); the weapon's own effects run
// while it is up.
void ViewModel::update_events(const Clips& c) {
    const bool up = visible_ && !lowered_ && !weapon_.empty();
    if (fx_weapon_ != (up ? weapon_ : std::string())) {
        stop_all_fx();
        fx_.assign(c.fx.size(), -1);
        fx_weapon_ = up ? weapon_ : std::string();
        if (up)
            for (size_t i = 0; i < c.fx.size(); ++i)
                if (c.fx[i].autostart) start_fx(c, int(i));
        events_serial_ = 0;
    }
    if (!up) return;
    Scene::ClipState cs;
    if (!scene_.clip_state(object_, cs)) return;
    if (cs.serial != events_serial_) events_serial_ = cs.serial, events_ms_ = cs.reverse ? cs.length + 1 : -1;
    auto it = c.events.find(cs.name);
    if (it == c.events.end()) {
        events_ms_ = cs.ms;
        return;
    }
    auto passed = [&](float from, float to) {   // (from, to]
        for (const Clips::Event& e : it->second) {
            if (!(e.ms > from && e.ms <= to)) continue;
            if (e.id == 50) start_fx(c, e.arg);
            else if (e.id == 51) stop_fx(e.arg);
            else if (e.id == 12 || e.id == 13) set_weapon_hidden(c, e.id == 12);   // CWeaponComponent::OnEvent: SetVisible
        }
    };
    if (cs.reverse) {
        if (cs.ms <= events_ms_) passed(cs.ms, events_ms_);   // backwards: the same span
        else passed(-1, events_ms_), passed(cs.ms, cs.length + 1);
    } else if (cs.ms >= events_ms_) {
        passed(events_ms_, cs.ms);
    } else {   // looped round
        passed(events_ms_, cs.length + 1), passed(-1, cs.ms);
    }
    events_ms_ = cs.ms;
}

void ViewModel::flash(bool aiming) {
    if (object_ == size_t(-1) || !visible_ || weapon_.empty()) return;
    const Clips& c = clips(weapon_);
    const int type = aiming && c.aim_muzzle_type >= 0 ? c.aim_muzzle_type : c.muzzle_type;
    if (type < 0 || c.muzzle_node.empty()) return;
    auto [it, fresh] = muzzles_.try_emplace(type);
    Muzzle& m = it->second;
    if (fresh) {
        m.object = scene_.spawn_object(type, -1100 - int(muzzles_.size()), glm::vec3(0), 0);
        if (m.object == size_t(-1)) return;
        scene_.set_object_viewmodel(m.object);
        // Its states set's first state (Idle): one clip per shot, at random. Without one, its
        // animation's clip from the start (CAnimationComponent::SetCurrentAnimationTime 0).
        if (const GolType* t = gol_.count(type) ? &gol_.at(type) : nullptr) {
            if (const Value* v = resolved_template(*t, gol_, kCompStatesSet); v && v->size() > 1 && (*v)[1].size() > 0)
                if (const Value& st = (*v)[1][0]; st.size() > 11)
                    for (const Value& clip : st[11].list)
                        if (clip.size() > 0 && clip[0].type == Value::String) m.clips.push_back(clip[0].s);
            if (const Value* a = resolved_template(*t, gol_, 0x0c); m.clips.empty() && a && a->size() > 1 && (*a)[1].type == Value::String &&
                !(*a)[1].s.empty())
                m.clips.push_back((*a)[1].s);
        }
    }
    if (m.object == size_t(-1) || m.clips.empty()) return;
    if (flashing_ >= 0 && flashing_ != type) scene_.set_object_visible(muzzles_[flashing_].object, false);
    scene_.play_clip(m.object, m.clips[size_t(std::rand()) % m.clips.size()], false);
    scene_.set_object_visible(m.object, true);
    flashing_ = type;
    place_muzzle();
}

// The muzzle object follows the weapon's muzzle node and goes once its clip is over
// (CWeaponComponent::UpdateMuzzleObjects).
void ViewModel::place_muzzle() {
    if (flashing_ < 0) return;
    const Muzzle& m = muzzles_[flashing_];
    const Clips& c = clips(weapon_);
    if (!visible_ || lowered_ || scene_.clip_finished(m.object)) {
        scene_.set_object_visible(m.object, false), flashing_ = -1;
        return;
    }
    scene_.set_object_transform(m.object, scene_.object_matrix(object_, c.node + "/" + c.muzzle_node));
}

bool ViewModel::muzzle(glm::vec3& out) const {
    if (object_ == size_t(-1) || !visible_ || weapon_.empty()) return false;
    auto it = clips_.find(weapon_);
    if (it == clips_.end() || it->second.muzzle_node.empty()) return false;
    return scene_.node_position(object_, it->second.node + "/" + it->second.muzzle_node, out);
}

}  // namespace nova
