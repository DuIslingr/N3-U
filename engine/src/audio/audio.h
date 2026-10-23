// Sound playback: the game's sound table (assets/sound_bank.h) mixed through an SDL
// audio stream. Sounds are decoded on first use (ADPCM/PCM WAV; the few .vxn streams are
// not supported yet) and cached. Voices follow the table: banks limit how many play at
// once, 3D groups attenuate with distance (inverse distance from refdistance, clamped at
// maxdistance) and pan by the listener's orientation, and play/stop can fade.
#pragma once

#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "assets/sound_bank.h"
#include "core/bytes.h"

struct SDL_AudioStream;
struct SDL_Mutex;

namespace nova {

class Vfs;

class Audio {
public:
    // `device`: open the default playback device; else the caller mixes with render()
    // (offline, e.g. into a file for checks).
    Audio(const Vfs& vfs, bool device);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool enabled() const { return stream_ != nullptr || offline_; }
    // Offline: mix the next `frames` stereo frames (48 kHz float) into `out`.
    void render(float* out, int frames);
    int rate() const { return rate_; }

    struct Params {
        const glm::vec3* position = nullptr;   // 3D sounds: where (else at the listener)
        int loop = -1;                         // -1: as the table says
        float fade_in_ms = 0;
        float gain = 1;
    };
    // Play a sound by label or file name; returns a handle, or -1.
    int play(const std::string& name, const Params& p);
    int play(const std::string& name) { return play(name, Params{}); }
    // Play a random sound of an event (StartSoundFromEvent); its voices are tagged with it.
    int play_event(const std::string& event, const Params& p);
    // Stop voices of a sound or event label (fading out over `fade_ms`).
    void stop(const std::string& name, float fade_ms = 0);
    void stop(int handle, float fade_ms = 0);
    void stop_all();
    void set_position(int handle, const glm::vec3& position);
    bool playing(int handle) const;
    // Any voice started from an event whose label starts with `prefix`.
    bool event_playing(const std::string& prefix) const;
    // A random sound out of a list of labels (gol sound lists).
    int play_one_of(const std::vector<std::string>& names, const Params& p);

    // Listener (the camera); updates the voices' 3D gains.
    void set_listener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    float master = 0.8f, music = 0.6f, sfx = 1.0f, voices = 1.0f;
    bool trace = false;   // log each sound started

    const SoundTable& table() const { return table_; }
    size_t voice_count() const;
    // Decode every sound in the table; returns the labels that failed (vxn streams aside).
    std::vector<std::string> check_all();

private:
    struct Clip {
        std::vector<float> samples;   // stereo, interleaved, at the device rate
        size_t loop_start = 0, loop_end = 0;   // frames; looping voices repeat this part
    };
    // Decode a WAV image to device-format samples (appended to `out`); false on failure.
    bool decode_wav(const Bytes& wav, std::vector<float>& out) const;
    // .vxn (VoxN): its segments decoded in order; loops repeat the second segment.
    std::shared_ptr<Clip> decode_vxn(const Bytes& data) const;
    struct Voice {
        int handle = 0;
        std::shared_ptr<const Clip> clip;
        const SoundDef* def = nullptr;
        std::string tag;              // event label, if started from one
        size_t frame = 0;
        bool loop = false;
        bool positional = false;
        glm::vec3 position{0};
        float gain = 1, bus = 1;      // the caller's gain, the group's volume
        float left = 1, right = 1;    // 3D gains (target)
        float cur_left = 0, cur_right = 0;
        float fade = 1, fade_target = 1, fade_step = 0;   // per frame
        bool stopping = false;
        uint64_t started = 0;
    };
    std::shared_ptr<const Clip> clip(const SoundDef& def);
    int start(const SoundDef& def, const Params& p, const std::string& tag);
    void spatialize(Voice& v) const;
    static void callback(void* user, SDL_AudioStream* stream, int additional, int total);
    void mix(float* out, int frames);

    const Vfs& vfs_;
    SoundTable table_;
    SDL_AudioStream* stream_ = nullptr;
    bool offline_ = false;
    SDL_Mutex* mutex_ = nullptr;
    int rate_ = 48000;
    std::unordered_map<std::string, std::shared_ptr<const Clip>> clips_;
    std::unordered_set<std::string> warned_;
    std::vector<Voice> voices_;
    std::vector<float> scratch_;
    int next_handle_ = 1;
    uint64_t counter_ = 0;
    glm::vec3 listener_{0}, forward_{0, 1, 0}, up_{0, 0, 1};
    std::mt19937 rng_{1234};
};

}  // namespace nova
