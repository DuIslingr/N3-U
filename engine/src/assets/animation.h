// Keyframe animation from BDAE files. See tools/bdae_anim.py for the format.
//
// Every track in a file shares one timeline (milliseconds); named clips are
// ranges on it. Key data is split into time segments whose buffers the
// samplers index; key times are absolute 30 fps frames (or ms).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/bytes.h"

namespace nova {

struct AnimTrack {
    enum class Property : uint8_t { Translation, Rotation, Scale } property;
    int component = -1;               // -1: all components; 0/1/2: single axis
    bool axis_angle = false;          // rotation stored as an angle (radians) about `defaults`
    std::vector<float> times;         // ms
    std::vector<float> values;        // times.size() * stride
    int stride = 0;
    glm::vec3 defaults{0};            // full vector for single-axis tracks, or the rotation axis
    bool has_defaults = false;
};

struct AnimClip {
    std::string name;
    float start = 0, end = 0;         // ms
};

struct AnimationLibrary {
    std::vector<AnimClip> clips;
    std::unordered_map<std::string, std::vector<AnimTrack>> tracks;   // by target node id
    float timeline_start = 0, timeline_end = 0;

    const AnimClip* clip(const std::string& name) const;
};

AnimationLibrary load_animation(const Bytes& bdae);

// A node's local transform at time `t` (ms), starting from its rest pose.
struct NodePose {
    glm::vec3 translation;
    glm::quat rotation;
    glm::vec3 scale;
};
void sample_tracks(const std::vector<AnimTrack>& tracks, float t, NodePose& pose);

}  // namespace nova
