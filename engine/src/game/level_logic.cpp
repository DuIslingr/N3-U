#include "game/level_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>

#include <glm/gtc/matrix_transform.hpp>

#include "audio/audio.h"
#include "game/player.h"
#include "game/scene.h"

namespace nova {

namespace {

constexpr int kCompTrigger = 0x06;        // CTriggerComponent
constexpr int kCompRunScript = 0x1E;      // CRunScriptComponent (OnDead)
constexpr int kCompInteractive = 0x44;    // CInteractiveComponent
constexpr int kCompDoor = 0x61;           // CDoorAIComponent
constexpr int kCompEffects = 0x92;        // CEffectsComponent: [[effect, node, autostart, ...], ...]
constexpr int kCompEffectBuiltIn = 0xAE;  // CEffectBuiltInComponent: effect, ?, node, autostart, ?
constexpr int kCompBuiltinBox = 0xA6;     // CComponentBuiltinBox: width, depth, height
constexpr int kCompBuiltinCylinder = 0xA8;   // CComponentBuiltinCylinder: radius, height

constexpr float kUseRange = 2.5f;         // metres from the player's eye

// Every string in a value tree that holds script functions.
void collect_scripts(const Value& v, std::vector<std::string>& out) {
    if (v.type == Value::String) {
        if (v.s.find("function") != std::string::npos) out.push_back(v.s);
        return;
    }
    for (const Value& x : v.list) collect_scripts(x, out);
}

}  // namespace

LevelLogic::LevelLogic(Scene& s, Player& p, const Gol& g) : scene(s), player(p), gol(g) {
    runtime_.log = [this](const std::string& line) {
        log(line);
        if (line.rfind("error", 0) == 0) message(line, 8);
    };
    register_game_api(*this);
}

LevelLogic::Object* LevelLogic::object(int id) {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

void LevelLogic::load_objects() {
    const Level& level = scene.level();
    for (size_t i = 0; i < level.objects.size(); ++i) {
        const LevelObject& o = level.objects[i];
        // Scripts: every chunk defines Obj<id>_<Event> functions in the shared state.
        std::vector<std::string> chunks;
        collect_scripts(o.fields, chunks);
        for (const auto& [comp, v] : o.components) collect_scripts(v, chunks);
        for (const std::string& c : chunks) {
            ++stats_.chunks;
            if (!runtime_.load(c, o.kind + " " + o.name + " #" + std::to_string(i))) ++stats_.chunk_errors;
        }

        Object obj;
        obj.index = i;
        obj.type = o.kind == "object" ? o.name : o.kind;
        if (o.kind == "level_properties" && !o.fields.list.empty()) {
            properties_id_ = o.fields[0].i;
            if (o.fields.size() > 44 && o.fields[41].type == Value::String) {
                music_event_ = o.fields[41].s;
                while (!music_event_.empty() && music_event_.back() == ' ') music_event_.pop_back();
                music_gap_min_ = o.fields[43].as_float(), music_gap_max_ = o.fields[44].as_float();
            }
            continue;
        }
        if (o.kind == "waypoint" && o.fields.size() > 4) {
            obj.id = o.fields[0].i;
            obj.position = {o.fields[2].as_float(), o.fields[3].as_float(), o.fields[4].as_float()};
            objects_[obj.id] = obj;
            continue;
        }
        if (o.kind == "spawn_point" && o.base) {
            obj.id = o.base->id;
            obj.position = o.base->position;
            obj.yaw = o.base->rotation.z;
            // The level start: the first spawn point flagged as such (as Scene picks it).
            if (start_spawn_id_ < 0 && o.fields.size() > 0 && o.fields[0].i == 1) start_spawn_id_ = obj.id;
            objects_[obj.id] = obj;
            continue;
        }
        if (o.kind != "object") continue;
        auto g = gol.find(o.type);
        if (g == gol.end()) continue;
        auto comp = [&](int c) -> const Value* {
            if (auto it = o.components.find(c); it != o.components.end()) return &it->second;
            return resolved_template(g->second, gol, c);
        };

        if (const Value* t = o.components.count(kCompTrigger) ? &o.components.at(kCompTrigger) : nullptr; t && t->size() >= 16) {
            // CTriggerComponent::Load: id, position, rotation, scale, run count, enabled, script,
            // target mode, ?, type mask, ...; the shape is the builtin box/cylinder times the scale.
            const Value& v = *t;
            obj.id = v[0].i;
            obj.position = {v[1].as_float(), v[2].as_float(), v[3].as_float()};
            obj.yaw = v[6].as_float();
            const glm::vec3 scale(v[7].as_float(), v[8].as_float(), v[9].as_float());
            obj.enabled = obj.visible = v[11].i != 0;
            Trigger tr;
            tr.runs_left = v[10].i;
            tr.player_target = v[13].i == 0 || (v[15].i & 1);
            if (const Value* cyl = o.components.count(kCompBuiltinCylinder) ? comp(kCompBuiltinCylinder) : nullptr; cyl && cyl->size() >= 2) {
                tr.cylinder = true;
                tr.center = glm::vec2(obj.position);
                tr.radius = scale.x * (*cyl)[0].as_float();
                const float h = scale.z * (*cyl)[1].as_float();
                tr.min = obj.position - glm::vec3(tr.radius, tr.radius, 0);
                tr.max = obj.position + glm::vec3(tr.radius, tr.radius, h);
            } else {
                // CTriggerShapeAABB: x and y centred, z from the position up; rotation ignored.
                glm::vec3 box(15, 3, 2);
                if (const Value* b = comp(kCompBuiltinBox); b && b->size() >= 3) box = {(*b)[0].as_float(), (*b)[1].as_float(), (*b)[2].as_float()};
                const glm::vec3 lo = glm::vec3(-0.5f * box.x, -0.5f * box.y, 0) * scale;
                const glm::vec3 hi = glm::vec3(0.5f * box.x, 0.5f * box.y, box.z) * scale;
                tr.min = obj.position + glm::min(lo, hi);
                tr.max = obj.position + glm::max(lo, hi);
            }
            obj.trigger = int(triggers_.size());
            triggers_.push_back(tr);
            ++stats_.triggers;
        } else {
            auto tf = o.base ? o.base : std::optional<Transform>();
            if (!tf) continue;   // objects without their own base are not addressable
            obj.id = tf->id;
            obj.position = tf->position;
            obj.yaw = tf->rotation.z;
            obj.enabled = obj.visible = tf->visible;
        }
        if (const Value* d = comp(kCompDoor); d && d->size() >= 1) {
            Door door;
            door.radius = (*d)[0].as_float();
            if (d->size() >= 3 && (*d)[1].type == Value::String) door.unlock_light = (*d)[1].s;
            if (d->size() >= 3 && (*d)[2].type == Value::String) door.lock_light = (*d)[2].s;
            obj.door = int(doors_.size());
            doors_.push_back(door);
            ++stats_.doors;
        }
        read_effects(obj, o);
        if (const Value* it = comp(kCompInteractive); it && it->size() >= 1) {
            obj.interactive = true;
            obj.interact_enabled = (*it)[0].i != 0;
            ++stats_.interactive;
        }
        objects_[obj.id] = obj;
    }
    stats_.objects = objects_.size();
}

void LevelLogic::read_effects(Object& obj, const LevelObject& o) {
    obj.effects.clear();
    auto g = gol.find(o.type);
    if (g == gol.end()) return;
    auto comp = [&](int c) -> const Value* {
        if (auto it = o.components.find(c); it != o.components.end()) return &it->second;
        return resolved_template(g->second, gol, c);
    };
    auto str = [](const Value& v) { return v.type == Value::String ? v.s : std::string(); };
    if (const Value* b = comp(kCompEffectBuiltIn); b && b->size() >= 4) {
        obj.effects.push_back({str((*b)[0]), str((*b)[2]), (*b)[3].i != 0});
    } else if (const Value* l = comp(kCompEffects); l && l->size() >= 1) {
        for (const Value& e : (*l)[0].list)
            if (e.size() >= 3) obj.effects.push_back({str(e[0]), str(e[1]), e[2].i != 0});
    }
}

bool LevelLogic::start_effect(Object& o, int index) {
    if (o.effects.empty()) return false;
    const bool builtin = o.effects.size() == 1 && o.effects[0].node.empty();
    if (builtin) index = 0;
    if (index < 0 || size_t(index) >= o.effects.size()) return false;
    Object::EffectSlot& e = o.effects[size_t(index)];
    if (e.handle >= 0) scene.particles().stop(e.handle);
    glm::mat4 at = glm::translate(glm::mat4(1), o.position) * glm::rotate(glm::mat4(1), glm::radians(o.yaw), glm::vec3(0, 0, 1));
    if (o.index != std::numeric_limits<size_t>::max()) at = scene.object_matrix(o.index, e.node);
    e.handle = scene.particles().start(e.effect, at);
    if (trace) log("effect " + e.effect + " on " + std::to_string(o.id) + (e.handle < 0 ? " (unknown)" : ""));
    return e.handle >= 0;
}

void LevelLogic::stop_effect(Object& o, int index) {
    if (o.effects.size() == 1) index = 0;
    if (index < 0 || size_t(index) >= o.effects.size()) return;
    Object::EffectSlot& e = o.effects[size_t(index)];
    if (e.handle >= 0) scene.particles().stop(e.handle);
    e.handle = -1;
}

bool LevelLogic::set_post_var(const std::string& effect, const std::string& var, float to, float seconds, const float* from) {
    float* v = nullptr;
    if (effect == "DOF") {
        if (var == "fFocus") v = &post.dof_focus;
        else if (var == "fRange") v = &post.dof_range;
        else if (var == "fFactor") v = &post.dof_factor;
        else if (var == "fBlurStep") v = &post.blur_step;
    } else if (effect == "HEIGHTFOG") {
        if (var == "FogTop") v = &post.fog_top;
        else if (var == "FogBottom") v = &post.fog_bottom;
        else if (var == "FogOpacity") v = &post.fog_opacity;
    }
    if (!v) return false;
    post_fades_.erase(std::remove_if(post_fades_.begin(), post_fades_.end(), [&](const PostFade& f) { return f.value == v; }),
                      post_fades_.end());
    if (from) *v = *from;
    if (seconds <= 0) *v = to;
    else post_fades_.push_back({v, *v, to, 0, seconds});
    return true;
}

void LevelLogic::start(bool keep_player) {
    runtime_.stop_all();
    if (audio && !keep_player) audio->stop_all(), music_ = -1, music_wait_ = 1;
    music_event_.clear();
    dialog_.clear(), dialog_active_ = false, dialog_voice_ = -1, dialog_wait_ = 0;
    if (!keep_player) post = PostParams{}, post_fades_.clear();
    objects_.clear();
    triggers_.clear();
    doors_.clear();
    delayed_.clear();
    messages_.clear();
    stats_ = {};
    level_ended = false;
    pending_section = 0;
    fog_locked = false;
    fog_zone_ = -2;
    level_fog_color_ = scene.fog_color;
    level_fog_start_ = scene.fog_start;
    level_fog_end_ = scene.fog_end;
    level_fog_density_ = scene.fog_density;
    load_objects();
    // Apply the starting state: hidden objects are disabled; doors start closed.
    for (auto& [id, o] : objects_) {
        if (o.index == std::numeric_limits<size_t>::max()) continue;
        if (!o.visible) {
            scene.set_object_visible(o.index, false);
            scene.set_object_collision(o.index, false);
        }
        if (o.door >= 0) scene.play_clip(o.index, "closed", true);
        if (o.visible)
            for (size_t k = 0; k < o.effects.size(); ++k)
                if (o.effects[k].autostart) start_effect(o, int(k));
    }
    // The camera's zone is looked up by position on the first frame (CZonesManager::
    // UpdateVisibility); only teleports set it. A section reload (LoadLevelSections) puts
    // the player back where it was; the level's start script runs again, the spawn's does not.
    scene.zones().force_zone(ZoneSystem::kNone);
    if (properties_id_ >= 0) start_script("Obj" + std::to_string(properties_id_) + "_OnEnter", properties_id_);
    if (start_spawn_id_ >= 0 && !keep_player) start_script("Obj" + std::to_string(start_spawn_id_) + "_Default", start_spawn_id_);
}

void LevelLogic::start_script(const std::string& function, int owner) {
    if (runtime_.start(function, owner) && trace) log("start " + function);
}

LevelLogic::Object& LevelLogic::add_object(int id, size_t index, const std::string& type, const glm::vec3& position, float yaw) {
    Object& o = objects_[id];
    o = Object{};
    o.id = id;
    o.index = index;
    o.type = type;
    o.position = position;
    o.yaw = yaw;
    o.enabled = o.visible = false;
    if (index < scene.level().objects.size()) read_effects(o, scene.level().objects[index]);
    ++stats_.objects;
    return o;
}

void LevelLogic::enable(Object& o, bool on) {
    if (o.enabled == on) return;
    if (trace) log((on ? "enable " : "disable ") + std::to_string(o.id) + " " + o.type);
    o.enabled = on;
    show(o, on);
    if (!on)
        for (size_t k = 0; k < o.effects.size(); ++k) stop_effect(o, int(k));
    if (o.trigger >= 0 && !on) triggers_[size_t(o.trigger)].inside = false;
}

void LevelLogic::show(Object& o, bool on) {
    o.visible = on;
    if (o.index == std::numeric_limits<size_t>::max()) return;
    scene.set_object_visible(o.index, on);
    if (o.door < 0 || doors_[size_t(o.door)].state == Door::State::Closed) scene.set_object_collision(o.index, on);
}

void LevelLogic::message(const std::string& text, float seconds) {
    messages_.push_back({text, seconds});
    while (messages_.size() > 8) messages_.pop_front();
    log("message: " + text);
}

void LevelLogic::teleport_player(const Object& spawn) {
    player.spawn(spawn.position, spawn.yaw);
    if (spawn.index != std::numeric_limits<size_t>::max()) scene.zones().force_zone(scene.zones().object_zone(spawn.index));
}

void LevelLogic::send_event_later(const std::string& event, float seconds, int id) {
    delayed_.push_back({event, id, seconds});
}

void LevelLogic::fade_fog(int what, glm::vec4 target, float seconds) {
    FogFade& f = fog_[what];
    switch (what) {
        case 0: f.from = scene.fog_color; break;
        case 1: f.from = glm::vec4(scene.fog_start); break;
        case 2: f.from = glm::vec4(scene.fog_end); break;
        default: f.from = glm::vec4(scene.fog_density); break;
    }
    f.to = target;
    f.elapsed = 0;
    f.duration = std::max(seconds, 0.0f);
    f.active = true;
}

// CLevelFog::UpdateFromZones: the camera's zone sets the fog (its own, or the level's),
// unless a script has locked it.
void LevelLogic::update_zone_fog() {
    const int zone = scene.zones().current();
    if (zone == fog_zone_ || fog_locked) return;
    const bool first = fog_zone_ == -2;
    fog_zone_ = zone;
    glm::vec4 color = level_fog_color_;
    float start = level_fog_start_, end = level_fog_end_, density = level_fog_density_;
    if (zone >= 0) {
        const ZoneSystem::Fog& f = scene.zones().zones()[size_t(zone)].fog;
        if (f.override) color = f.color, start = f.start, end = f.end, density = f.density;
    }
    const float seconds = first ? 0.0f : 1.0f;
    fade_fog(0, color, seconds);
    fade_fog(1, glm::vec4(start), seconds);
    fade_fog(2, glm::vec4(end), seconds);
    fade_fog(3, glm::vec4(density), seconds);
}

void LevelLogic::update_fog(float dt) {
    for (int i = 0; i < 4; ++i) {
        FogFade& f = fog_[i];
        if (!f.active) continue;
        f.elapsed += dt;
        const float t = f.duration > 0 ? std::min(f.elapsed / f.duration, 1.0f) : 1.0f;
        const glm::vec4 v = f.from + (f.to - f.from) * t;
        if (i == 0) scene.fog_color = glm::vec4(glm::vec3(v), scene.fog_color.a);
        else if (i == 1) scene.fog_start = v.x;
        else if (i == 2) scene.fog_end = v.x;
        else scene.fog_density = v.x;
        if (t >= 1) f.active = false;
    }
}

// CTriggerComponent::Update / TestTargetForAction / DoAction for the player: the
// player's capsule (feet up to its height, with its radius) against the trigger shape.
void LevelLogic::update_triggers() {
    const glm::vec3 feet = player.center - glm::vec3(0, 0, player.radius.z);
    const float height = 2 * player.radius.z, r = player.radius.x;
    for (auto& [id, o] : objects_) {
        if (o.trigger < 0) continue;
        Trigger& t = triggers_[size_t(o.trigger)];
        if (!o.enabled || !t.player_target) continue;
        bool in = feet.z <= t.max.z && feet.z + height >= t.min.z;
        if (in && t.cylinder) {
            const float reach = t.radius + r;
            in = glm::dot(glm::vec2(feet) - t.center, glm::vec2(feet) - t.center) <= reach * reach;
        } else if (in) {
            in = feet.x + r >= t.min.x && feet.x - r <= t.max.x && feet.y + r >= t.min.y && feet.y - r <= t.max.y;
        }
        const std::string prefix = "Obj" + std::to_string(id) + "_";
        auto act = [&](const char* event) {
            const std::string fn = prefix + event;
            if (!runtime_.has_function(fn)) return;
            start_script(fn, id);
            if (t.runs_left > 0 && --t.runs_left == 0) enable(o, false);
        };
        const bool was = t.inside;
        t.inside = in;
        if (trace && in != was) log((in ? "enter trigger " : "leave trigger ") + std::to_string(id));
        if (in && !was) act("OnEnter");
        if (!in && was) act("OnExit");
        if (!o.enabled) continue;
        act(in ? "WhileIn" : "WhileOut");
    }
}

// CDoorAIComponent: a closed door opens when the player comes within its radius, unless
// it is locked (scripts, or enemies around and not kept unlocked); it closes again when
// the player has left. Collision is off from the moment it starts opening.
void LevelLogic::update_doors(float) {
    const glm::vec3 eye = player.eye();
    for (auto& [id, o] : objects_) {
        if (o.door < 0 || !o.enabled) continue;
        Door& d = doors_[size_t(o.door)];
        bool near = glm::distance(eye, o.position) <= d.radius;
        for (const glm::vec3& a : actor_positions) near = near || glm::distance(a, o.position) <= d.radius * 0.6f;
        const bool locked = d.locked && !d.keep_unlocked;
        // The lock lights follow the lock state, whatever the door's clip says.
        if (!d.unlock_light.empty()) scene.set_node_visible(o.index, d.unlock_light, !locked);
        if (!d.lock_light.empty()) scene.set_node_visible(o.index, d.lock_light, locked);
        switch (d.state) {
            case Door::State::Closed:
                if (d.forced_open || (near && !locked)) set_door(o, true);
                break;
            case Door::State::Opening:
                // Half open: walkable, and no longer hides what is behind it.
                if (scene.clip_progress(o.index) >= 0.5f) scene.set_object_collision(o.index, false);
                if (scene.clip_finished(o.index)) {
                    d.state = Door::State::Open;
                    scene.set_object_collision(o.index, false);
                    const bool ok = scene.play_clip(o.index, "open", true);
                    if (trace) log("door " + std::to_string(id) + " open" + (ok ? "" : " (no clip)"));
                }
                break;
            case Door::State::Open:
                if (!d.forced_open && (!near || locked)) set_door(o, false);
                break;
            case Door::State::Closing:
                if (scene.clip_finished(o.index)) {
                    d.state = Door::State::Closed;
                    scene.play_clip(o.index, "closed", true);
                    scene.zones().set_door_open(o.index, false);
                }
                break;
        }
    }
}

void LevelLogic::set_door(Object& o, bool open_now) {
    Door& d = doors_[size_t(o.door)];
    if (trace) log((open_now ? "open door " : "close door ") + std::to_string(o.id));
    // A door is an open portal from the moment it starts opening until it has fully closed.
    if (open_now) scene.zones().set_door_open(o.index, true);
    if (open_now) {
        if (d.state == Door::State::Open || d.state == Door::State::Opening) return;
        d.state = Door::State::Opening;
        // Its collision (which also hides what is behind it from view rays) goes once half open.
        if (!scene.play_clip(o.index, "opening", false)) {
            d.state = Door::State::Open;
            scene.set_object_collision(o.index, false);
        }
    } else {
        if (d.state == Door::State::Closed || d.state == Door::State::Closing) return;
        // There is no closing clip: "opening" plays backwards. Collision returns at once,
        // so nothing can walk into a closing door.
        d.state = Door::State::Closing;
        if (!scene.play_clip(o.index, "opening", false, true)) {
            d.state = Door::State::Closed;
            scene.play_clip(o.index, "closed", true);
            scene.zones().set_door_open(o.index, false);
        }
        if (o.visible) scene.set_object_collision(o.index, true);
    }
}

bool LevelLogic::exec(const std::string& code) {
    static int counter = 0;
    const std::string name = "Exec" + std::to_string(++counter);
    if (!runtime_.load("function " + name + "(...)\n" + code + "\nend\n", name)) return false;
    start_script(name, 0);
    return true;
}

bool LevelLogic::use() {
    const glm::vec3 eye = player.eye(), fwd = player.view_forward();
    Object* best = nullptr;
    float best_d = kUseRange;
    for (auto& [id, o] : objects_) {
        if (!o.interactive || !o.enabled || !o.interact_enabled) continue;
        const glm::vec3 to = o.position + glm::vec3(0, 0, 1) - eye;
        const float d = glm::length(to);
        if (d > best_d || glm::dot(to, fwd) < 0) continue;
        best = &o;
        best_d = d;
    }
    if (!best) return false;
    const std::string prefix = "Obj" + std::to_string(best->id) + "_";
    start_script(prefix + "OnEnter", best->id);
    // There are no minigames yet: count the use as won.
    start_script(prefix + "OnMinigameWin", best->id);
    return true;
}

void LevelLogic::dialog(const std::vector<std::string>& lines) {
    for (const std::string& l : lines) dialog_.push_back(l);
    dialog_active_ = true;
}

void LevelLogic::update_dialog(float dt) {
    if (!dialog_active_) return;
    if (dialog_voice_ >= 0 && audio && audio->playing(dialog_voice_)) return;
    dialog_voice_ = -1;
    if ((dialog_wait_ -= dt) > 0) return;
    if (dialog_.empty()) {
        dialog_active_ = false;
        runtime_.send_event("MessageEnd");
        return;
    }
    const std::string line = dialog_.front();
    dialog_.pop_front();
    dialog_voice_ = audio ? audio->play(line) : -1;
    // Without a voice the line stays up for a few seconds; after one, a short pause.
    dialog_wait_ = dialog_voice_ >= 0 ? 0.3f : 3.0f;
    message(line, dialog_voice_ >= 0 ? 4.0f : 3.0f);
}

void LevelLogic::update_music(float dt) {
    if (!audio || music_event_.empty()) return;
    if (audio->event_playing("env_m_action")) {   // scripted action music has the floor
        if (music_ >= 0) audio->stop(music_, 3000), music_ = -1;
        music_wait_ = 4;
        return;
    }
    if (music_ >= 0 && audio->playing(music_)) return;
    if (music_ >= 0) {   // a track ended: a pause before the next
        music_ = -1;
        music_wait_ = music_gap_min_ + float(std::rand()) / float(RAND_MAX) * std::max(music_gap_max_ - music_gap_min_, 0.0f);
    }
    if ((music_wait_ -= dt) > 0) return;
    Audio::Params p;
    p.fade_in_ms = 2000;
    music_ = audio->play_event(music_event_, p);
    if (music_ < 0) music_wait_ = 30;
}

void LevelLogic::update(float dt) {
    update_music(dt);
    update_dialog(dt);
    // PostEffects::Update: variables move linearly to their targets.
    for (PostFade& f : post_fades_) {
        f.elapsed += dt;
        const float t = std::min(f.elapsed / f.duration, 1.0f);
        *f.value = f.from + (f.to - f.from) * t;
    }
    post_fades_.erase(std::remove_if(post_fades_.begin(), post_fades_.end(), [](const PostFade& f) { return f.elapsed >= f.duration; }),
                      post_fades_.end());
    for (auto it = delayed_.begin(); it != delayed_.end();) {
        it->seconds -= dt;
        if (it->seconds <= 0) {
            runtime_.send_event(it->event, it->id);
            it = delayed_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& m : messages_) m.seconds_left -= dt;
    while (!messages_.empty() && messages_.front().seconds_left <= 0) messages_.pop_front();
    update_triggers();
    update_doors(dt);
    update_zone_fog();
    update_fog(dt);
    runtime_.update(dt * 1000.0f);
}

}  // namespace nova
