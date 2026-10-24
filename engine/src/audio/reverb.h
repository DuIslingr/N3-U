// The sound engine's reverb (Gameloft VOX's vox::ReverbHQC, the DSP on the AUX1 bus that
// 3D voices and the sfx_reverb groups play through): an EAX-style reverb, ported from the
// game's fixed-point code. A two-pole lowpass into a pre-delay line; four early-reflection
// lines mixed through a Householder-like matrix; four delay lines tapped from the pre-delay
// feeding a feedback network (a damping lowpass, an allpass and a Hadamard-like mix per
// line). The output is the dry input plus the early and late sound.
//
// Its presets (SetDSPPreset, per area: NOVA3_Outside, NOVA3_LargeMetalRoom, ...) come from
// sounds_hi/nova3_reverbs.fxb, a VST bank ("hr1V") of 15-parameter patches.
#pragma once

#include <string>
#include <vector>

#include "core/bytes.h"

namespace nova {

// vox::ReverbHQParameters, as vox::ReverbHQParameters::loadReverbSettings maps a patch's
// normalized values (defaults: its constructor's).
struct ReverbParams {
    float density = 1;              // [0]: scales the late delays by 1 + 4 * density
    float diffusion = 1;            // [1]
    float gain_hf = 0.89f;          // [2]: the input lowpass's gain at hf_reference
    float hf_reference = 5000;      // [3]: 750 * 2^(5 x) Hz
    float decay_time = 1.49f;       // [4]: 8 x s
    float decay_hf_ratio = 0.83f;   // [5]
    bool decay_hf_limit = true;     // [6]: x >= 0.5
    float air_absorption_hf = 0.994f;   // [7]
    float gain = 0.427f;            // [8]
    float reflections_gain = 0.0375f;   // [9]
    float reflections_delay = 0.007f;   // [10]: 0.1 x s
    float late_gain = 0.945f;       // [11]
    float late_delay = 0.011f;      // [12]: 0.1 x s
    float dry = 1;                  // [13]
    float wet = 0.2f;               // [14]
};

struct ReverbPreset {
    std::string name;
    ReverbParams params;
};

// The bank's patches (none if it is not one).
std::vector<ReverbPreset> load_reverb_bank(const Bytes& fxb);

class Reverb {
public:
    void set_rate(int rate);
    // SetDSPPreset: change to `p` over `seconds` (each parameter moving linearly).
    void set_params(const ReverbParams& p, float seconds);
    // Add the reverb of the stereo `in` (the AUX1 bus's mix, dry sound included) to `out`.
    void process(const float* in, float* out, int frames);

private:
    void apply(const ReverbParams& p);   // vox::ReverbHQC::_SetParameters
    float rate_ = 48000;
    ReverbParams from_, to_, now_;
    float fade_ = 0, fade_left_ = 0;
    bool ready_ = false;

    struct Line {
        std::vector<float> buf;
        uint32_t mask = 0;
        uint32_t delay = 0;
        float gain = 0;
    };
    Line pre_, early_[4], allpass_[4], late_[4];
    uint32_t taps_[4] = {};       // late taps into the pre-delay
    uint32_t early_tap_ = 0;
    float input_lp_ = 0, lp1_ = 0, lp2_ = 0;
    float damp_[4] = {}, damp_state_[4] = {};
    float late_in_ = 0, allpass_k_ = 0, mix_ = 0;
    float early_level_ = 0, late_level_ = 0, dry_ = 0, wet_ = 0;
    uint32_t n_ = 0;
};

}  // namespace nova
