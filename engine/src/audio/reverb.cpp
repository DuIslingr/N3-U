#include "audio/reverb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nova {

namespace {

// vox::ReverbHQC's line lengths (seconds): early reflections, the allpasses, and the late
// lines (times 1 + 4 * density, so up to 5 times these).
constexpr float kEarly[4] = {0.0015f, 0.0045f, 0.0135f, 0.0405f};
constexpr float kAllpass[4] = {0.0151f, 0.0167f, 0.0183f, 0.02f};
constexpr float kLate[4] = {0.0211f, 0.0311f, 0.0461f, 0.068f};
constexpr float kPreDelay = 0.236265629529953f;   // the pre-delay line's reach

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// vox::ReverbHQC::CalcLengths: the power of two above `samples` (1 for none).
uint32_t line_size(float samples) {
    uint32_t v = samples > 0 ? uint32_t(samples) : 0, size = 1;
    while (v) v >>= 1, size <<= 1;
    return size;
}

// The one-pole lowpass coefficient whose gain at the frequency (cos w = c) is sqrt(g)
// (vox::ReverbHQC::_SetParameters' input and damping filters).
float lowpass(float g, float c) {
    return ((1.0f - g * c) - std::sqrt(((1.0f - c) + (1.0f - c) + (c * c - 1.0f) * g) * g)) / (1.0f - g);
}

}  // namespace

std::vector<ReverbPreset> load_reverb_bank(const Bytes& fxb) {
    // A VST bank: "CcnK" size "FxBk" version id version count, 128 reserved bytes, then
    // per patch "CcnK" size "FxCk" version id version count, a 28-byte name and its
    // big-endian floats (vox::ReverbHQParameters::loadReverbSettings).
    std::vector<ReverbPreset> out;
    if (fxb.size() < 156 || std::memcmp(fxb.data(), "CcnK", 4) != 0 || std::memcmp(fxb.data() + 8, "FxBk", 4) != 0) return out;
    const uint32_t count = be32(fxb.data() + 24);
    size_t at = 156;
    for (uint32_t i = 0; i < count && at + 56 <= fxb.size(); ++i) {
        const uint8_t* p = fxb.data() + at;
        if (std::memcmp(p, "CcnK", 4) != 0 || std::memcmp(p + 8, "FxCk", 4) != 0) break;
        const uint32_t params = be32(p + 24);
        const size_t size = 56 + size_t(params) * 4;
        if (at + size > fxb.size()) break;
        at += size;
        if (params != 15) continue;
        float v[15];
        for (int k = 0; k < 15; ++k) {
            const uint32_t bits = be32(p + 56 + 4 * k);
            std::memcpy(&v[k], &bits, 4);
        }
        ReverbPreset r;
        r.name.assign(reinterpret_cast<const char*>(p + 28), strnlen(reinterpret_cast<const char*>(p + 28), 28));
        ReverbParams& q = r.params;
        q.density = v[0], q.diffusion = v[1], q.gain_hf = v[2];
        q.hf_reference = float(std::pow(2.0, double(v[3] * 5.0f)) * 750.0);
        q.decay_time = v[4] * 8.0f, q.decay_hf_ratio = v[5], q.decay_hf_limit = v[6] >= 0.5f;
        q.air_absorption_hf = v[7], q.gain = v[8], q.reflections_gain = v[9];
        q.reflections_delay = v[10] * 0.1f, q.late_gain = v[11], q.late_delay = v[12] * 0.1f;
        q.dry = v[13], q.wet = v[14];
        out.push_back(std::move(r));
    }
    return out;
}

void Reverb::set_rate(int rate) {
    rate_ = float(rate);
    // vox::ReverbHQC::CreateDelayLines.
    auto make = [&](Line& l, float seconds) {
        const uint32_t size = line_size(seconds);
        l.buf.assign(size, 0.0f), l.mask = size - 1;
    };
    make(pre_, rate_ * kPreDelay + 9.0f);
    for (int i = 0; i < 4; ++i) {
        make(early_[i], rate_ * kEarly[i]);
        make(allpass_[i], rate_ * kAllpass[i]);
        make(late_[i], rate_ * 5.0f * kLate[i]);
        early_[i].delay = uint32_t(rate_ * kEarly[i]);
        allpass_[i].delay = uint32_t(rate_ * kAllpass[i]);
    }
    ready_ = true;
    apply(now_);
}

void Reverb::set_params(const ReverbParams& p, float seconds) {
    from_ = now_, to_ = p;
    fade_ = std::max(seconds, 0.0f), fade_left_ = fade_;
    if (fade_ <= 0) now_ = p;
    if (ready_) apply(now_);
}

void Reverb::apply(const ReverbParams& p) {
    const float fs = rate_;
    // The input lowpass: gain_hf at hf_reference.
    const float c = float(std::cos(double(p.hf_reference * 6.2831855f / fs)));
    const float g = std::max(p.gain_hf, 1e-4f);
    input_lp_ = g < 0.9999f ? float(int(lowpass(g, c) * 16384.0f)) / 16384.0f : 0.0f;
    // Where the early sound and the late lines' inputs are tapped from the pre-delay.
    early_tap_ = uint32_t(std::max(p.reflections_delay * fs, 0.0f));
    float t = p.reflections_delay + p.late_delay;
    const double step = double((p.density * 4.0f + 1.0f) * 0.0211f) * 0.03125;
    taps_[0] = uint32_t(std::max(t * fs, 0.0f));
    t = float(double(t) + step), taps_[1] = uint32_t(std::max(t * fs, 0.0f));
    t = float(double(t) + step * 3.0), taps_[2] = uint32_t(std::max(t * fs, 0.0f));
    t = float(double(t) + step * 7.0), taps_[3] = uint32_t(std::max(t * fs, 0.0f));
    early_level_ = float(int(p.gain * p.reflections_gain * 4096.0f)) / 4096.0f;
    // Early lines: decaying by 60 dB over late_delay.
    for (int i = 0; i < 4; ++i)
        early_[i].gain = float(int(std::pow(2.0, double(kEarly[i] / p.late_delay * -9.9657f)) * 16384.0)) / 16384.0f;
    const float keep = float(1.0 - 0.5 * double(p.diffusion) * p.diffusion * p.diffusion);
    const int keep_q = int(keep * 16384.0f);
    late_level_ = float(((int(p.gain * 4096.0f) * int(p.late_gain * 16384.0f)) >> 14) * keep_q >> 14) / 4096.0f;
    const float g2 = float(std::pow(10.0, double((p.density * 0.1663f + 0.1663f) / p.decay_time * -3.0f)));
    late_in_ = float(std::min(int(std::sqrt((1.0f - g2 * g2) * 1.059986f) * 16384.0f), 16384)) / 16384.0f;
    allpass_k_ = float(int(double(p.diffusion) * p.diffusion * p.diffusion * 2457.6)) / 4096.0f;
    mix_ = float(int(std::sqrt((1.0f - keep * keep) * 0.33333334f) / keep * 2048.0f)) / 2048.0f;
    for (int i = 0; i < 4; ++i)
        allpass_[i].gain = float(int(std::pow(10.0, double(kAllpass[i] / p.decay_time * -3.0f)) * 4096.0)) / 4096.0f;
    // The high frequencies' decay: decay_hf_ratio of decay_time, no shorter than air
    // absorption allows when limited.
    float hf_ratio = p.decay_hf_ratio;
    if (p.decay_hf_limit && p.air_absorption_hf < 1.0f) {
        float v = float(-0.008738712807089252 / (std::log10(double(p.air_absorption_hf)) * double(p.decay_time)));
        v = std::max(v, 0.1f);
        if (hf_ratio > v) hf_ratio = v;
    }
    const float c2 = float(std::cos(double(p.hf_reference * 6.2831855f / fs)));
    for (int i = 0; i < 4; ++i) {
        const float d = (p.density * 4.0f + 1.0f) * kLate[i];
        late_[i].delay = uint32_t(std::max(d * fs, 0.0f));
        const int gq = int(std::pow(10.0, double(d / p.decay_time * -3.0f)) * 4096.0);
        float damp = 0;
        if (hf_ratio < 1.0f) {
            const float h = float(std::pow(10.0, double(d / (hf_ratio * p.decay_time) * -3.0f)));
            float r = h / (float(gq) * 6.1035156e-05f);
            r = std::max(r, 0.1f);
            r *= r;
            if (r < 0.9999f) damp = std::min(lowpass(r, c2), 0.98f);
            damp = float(int(damp * 4096.0f));
        }
        damp_[i] = damp / 4096.0f;
        late_[i].gain = float(keep_q * gq >> 14) / 4096.0f;
    }
    dry_ = float(int(p.dry * 4096.0f)) / 4096.0f;
    wet_ = float(int(p.wet * 4096.0f)) / 4096.0f;
}

void Reverb::process(const float* in, float* out, int frames) {
    if (!ready_) return;
    if (fade_left_ > 0) {   // vox::ReverbHQC::UpdateParameters: each parameter on its way
        fade_left_ = std::max(fade_left_ - float(frames) / rate_, 0.0f);
        const float k = fade_ > 0 ? 1.0f - fade_left_ / fade_ : 1.0f;
        auto mix = [&](float a, float b) { return a + (b - a) * k; };
        ReverbParams& p = now_;
        p.density = mix(from_.density, to_.density), p.diffusion = mix(from_.diffusion, to_.diffusion);
        p.gain_hf = mix(from_.gain_hf, to_.gain_hf), p.hf_reference = mix(from_.hf_reference, to_.hf_reference);
        p.decay_time = mix(from_.decay_time, to_.decay_time), p.decay_hf_ratio = mix(from_.decay_hf_ratio, to_.decay_hf_ratio);
        p.decay_hf_limit = k < 1 ? from_.decay_hf_limit : to_.decay_hf_limit;
        p.air_absorption_hf = mix(from_.air_absorption_hf, to_.air_absorption_hf), p.gain = mix(from_.gain, to_.gain);
        p.reflections_gain = mix(from_.reflections_gain, to_.reflections_gain);
        p.reflections_delay = mix(from_.reflections_delay, to_.reflections_delay);
        p.late_gain = mix(from_.late_gain, to_.late_gain), p.late_delay = mix(from_.late_delay, to_.late_delay);
        p.dry = mix(from_.dry, to_.dry), p.wet = mix(from_.wet, to_.wet);
        apply(p);
    }
    auto at = [&](const Line& l, uint32_t back) -> float { return l.buf[(n_ - back) & l.mask]; };
    auto put = [&](Line& l, float v) { l.buf[n_ & l.mask] = v; };
    for (int f = 0; f < frames; ++f) {
        const float left = in[2 * f], right = in[2 * f + 1];
        // The input, lowpassed twice, into the pre-delay.
        const float x = left + right;
        lp1_ = x + input_lp_ * (lp1_ - x);
        lp2_ = lp1_ + input_lp_ * (lp2_ - lp1_);
        put(pre_, lp2_);
        // Early reflections.
        float e[4];
        for (int i = 0; i < 4; ++i) e[i] = early_[i].gain * at(early_[i], early_[i].delay);
        const float er = at(pre_, early_tap_) + (e[0] + e[1] + e[2] + e[3]) * 0.5f;
        for (int i = 0; i < 4; ++i) put(early_[i], er - e[i]);
        const float early_l = er - e[0], early_r = er - e[1];
        // Late lines: a tap and the line's own feedback, damped.
        float v[4];
        for (int i = 0; i < 4; ++i) {
            float s = at(pre_, taps_[i]) * late_in_ + late_[i].gain * at(late_[i], late_[i].delay);
            s += damp_[i] * (damp_state_[i] - s);
            damp_state_[i] = s;
            v[i] = s;
        }
        // Allpasses (the game pairs lines 0-3 with its allpasses 1, 3, 0, 2).
        auto allpass = [&](Line& l, float s) {
            const float y = l.gain * at(l, l.delay) - allpass_k_ * s;
            put(l, s + allpass_k_ * y);
            return y;
        };
        const float y1 = allpass(allpass_[1], v[0]), y2 = allpass(allpass_[3], v[1]);
        const float y3 = allpass(allpass_[0], v[2]), y4 = allpass(allpass_[2], v[3]);
        const float late_l = y1 + mix_ * ((y4 + y2) - y3), late_r = y2 + mix_ * ((y4 + y3) - y1);
        put(late_[0], y3 + mix_ * ((y4 + y1) - y2));
        put(late_[1], late_l);
        put(late_[2], y4 + mix_ * ((-y2 - y1) - y3));
        put(late_[3], late_r);
        ++n_;
        out[2 * f] += wet_ * (late_level_ * late_l + early_level_ * early_l) + dry_ * left;
        out[2 * f + 1] += wet_ * (late_level_ * late_r + early_level_ * early_r) + dry_ * right;
    }
}

}  // namespace nova
