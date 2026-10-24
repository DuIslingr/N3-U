// Sound playback: the game's sound table (assets/sound_bank.h) mixed through an SDL
// audio stream. Sounds are decoded on first use (ADPCM/PCM WAV, and the few .vxn
// interactive streams with their states) and cached. Voices follow the table: banks limit how many play at
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
#include "audio/reverb.h"
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
        bool ui = false;                       // a menu's sound: plays while the game's are held
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
    // The pause menu, the main menu, the loading screen: voices hold where they are, but
    // those of the sound `except` (the title music) play on.
    void set_paused(bool paused, const std::string& except = {});
    // SetDSPPreset(name, ms): the reverb's preset (by its exact name, any case; one the bank
    // lacks gets the defaults), reached over `ms`. DeviceOptions' "Reverb" is on, as on all
    // but single-core devices.
    void set_reverb_preset(const std::string& name, float ms);
    void set_position(int handle, const glm::vec3& position);
    bool playing(int handle) const;
    bool playing(const std::string& name) const;   // any voice of this sound label
    // Interactive (.vxn) sounds (VoxSoundManager::SetInteractiveMusicState): move a voice,
    // or every voice of a sound label, to a named state ("mmenu", "loading", "ride"...),
    // as the file's transitions allow, crossfading by its rule.
    void set_state(int handle, const std::string& state);
    void set_state(const std::string& name, const std::string& state);
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
    // Decode every sound in the table; returns the labels that failed.
    std::vector<std::string> check_all();

private:
    struct Clip {
        std::vector<float> samples;   // stereo, interleaved, at the device rate
        size_t loop_start = 0, loop_end = 0;   // frames; looping voices repeat this part
        // .vxn: segments (frame ranges), and per state a playlist of them, each played once
        // or looped; transitions[from][to]: allowed; the crossfade (frames out, in).
        struct Element {
            int segment = 0;
            bool loop = false;
        };
        std::vector<std::pair<size_t, size_t>> segments;
        std::vector<std::vector<Element>> playlists;
        std::vector<std::string> states;
        std::vector<std::vector<bool>> transitions;
        size_t fade_out = 0, fade_in = 0;
        bool interactive() const { return !playlists.empty(); }
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
        bool aux = false;             // on the AUX1 bus: through the reverb
        float left = 1, right = 1;    // 3D gains (target)
        float cur_left = 0, cur_right = 0;
        float fade = 1, fade_target = 1, fade_step = 0;   // per frame
        bool stopping = false;
        uint64_t started = 0;
        // Interactive clips: the state's playlist and where in it; the segment left behind
        // at a state change fades out alongside.
        int state = 0, element = 0;
        bool done = false, ui = false;
        size_t tail_frame = 0, tail_end = 0;
        float tail_gain = 0, tail_step = 0;
    };
    // Interactive voices: the current element's segment, and moving on at its end.
    const Clip::Element* element(const Voice& v) const;
    void next_element(Voice& v) const;
    void set_state(Voice& v, int state) const;
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
    bool paused_ = false;
    std::string unpaused_;
    std::vector<float> scratch_;
    Reverb reverb_;
    std::vector<ReverbPreset> reverb_presets_;
    std::vector<float> aux_;          // the AUX1 bus's mix, before the reverb
    int next_handle_ = 1;
    uint64_t counter_ = 0;
    glm::vec3 listener_{0}, forward_{0, 1, 0}, up_{0, 0, 1};
    std::mt19937 rng_{1234};
};

}  // namespace nova
