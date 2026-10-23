#include "audio/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/vfs.h"

namespace nova {

namespace {

struct Lock {
    SDL_Mutex* m;
    explicit Lock(SDL_Mutex* mutex) : m(mutex) { if (m) SDL_LockMutex(m); }
    ~Lock() { if (m) SDL_UnlockMutex(m); }
};

}  // namespace

Audio::Audio(const Vfs& vfs, bool device) : vfs_(vfs) {
    if (vfs_.exists("NOVA3_VOX_SDD.xml")) {
        const Bytes xml = vfs_.read("NOVA3_VOX_SDD.xml");
        table_ = parse_sound_table(std::string(xml.begin(), xml.end()));
    }
    if (!device) {
        offline_ = true;
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, rate_};
    mutex_ = SDL_CreateMutex();
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Audio::callback, this);
    if (!stream_) {
        std::fprintf(stderr, "audio: %s\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream_);
}

Audio::~Audio() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    if (mutex_) SDL_DestroyMutex(mutex_);
}

size_t Audio::voice_count() const {
    Lock lock(mutex_);
    return voices_.size();
}

std::vector<std::string> Audio::check_all() {
    std::vector<std::string> failed;
    for (const SoundDef& s : table_.sounds) {
        if (s.uid < 0 || s.format == "vxn") continue;
        auto c = clip(s);
        if (!c || c->samples.empty()) failed.push_back(s.label + " (" + s.filename + ")");
        clips_.clear();   // keep memory flat
    }
    return failed;
}

std::shared_ptr<const Audio::Clip> Audio::clip(const SoundDef& def) {
    if (auto it = clips_.find(def.filename); it != clips_.end()) return it->second;
    std::shared_ptr<Clip> c;
    if (def.format == "vxn" || def.filename.size() < 4 || def.filename.compare(def.filename.size() - 4, 4, ".vxn") == 0) {
        if (warned_.insert(def.filename).second) std::fprintf(stderr, "audio: %s: .vxn streams are not supported yet\n", def.filename.c_str());
    } else if (!vfs_.exists(def.filename)) {
        if (warned_.insert(def.filename).second) std::fprintf(stderr, "audio: no file %s\n", def.filename.c_str());
    } else {
        const Bytes data = vfs_.read(def.filename);
        SDL_AudioSpec src{};
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (SDL_LoadWAV_IO(SDL_IOFromConstMem(data.data(), data.size()), true, &src, &buf, &len)) {
            const SDL_AudioSpec dst{SDL_AUDIO_F32, 2, rate_};
            Uint8* out = nullptr;
            int out_len = 0;
            if (SDL_ConvertAudioSamples(&src, buf, int(len), &dst, &out, &out_len)) {
                c = std::make_shared<Clip>();
                c->samples.resize(size_t(out_len) / sizeof(float));
                std::memcpy(c->samples.data(), out, c->samples.size() * sizeof(float));
                SDL_free(out);
            }
            SDL_free(buf);
        }
        if (!c && warned_.insert(def.filename).second) std::fprintf(stderr, "audio: %s: %s\n", def.filename.c_str(), SDL_GetError());
    }
    clips_[def.filename] = c;
    return c;
}

void Audio::spatialize(Voice& v) const {
    if (!v.positional) {
        v.left = v.right = 1;
        return;
    }
    const glm::vec3 to = v.position - listener_;
    const float d = glm::length(to);
    const float ref = v.def && v.def->ref_distance > 0 ? v.def->ref_distance : 2.0f;
    const float max = v.def && v.def->max_distance > 0 ? v.def->max_distance : 60.0f;
    // OpenAL's inverse distance clamped, rolloff 1; nothing beyond the max distance.
    const float dc = std::clamp(d, ref, max);
    float g = ref / dc;
    if (d > max) g *= std::max(0.0f, 1.0f - (d - max) / (0.25f * max));
    // Equal-power pan; sources near the listener sit in the middle.
    const glm::vec3 right = glm::normalize(glm::cross(forward_, up_));
    float pan = d > 1e-3f ? glm::dot(to / d, right) : 0.0f;
    pan *= std::clamp(d / ref, 0.0f, 1.0f);
    const float a = (pan + 1) * 0.25f * 3.14159265f;
    v.left = g * std::cos(a) * 1.41421356f;
    v.right = g * std::sin(a) * 1.41421356f;
}

int Audio::start(const SoundDef& def, const Params& p, const std::string& tag) {
    if (!enabled()) return -1;
    auto c = clip(def);
    if (!c || c->samples.empty()) return -1;
    Voice v;
    v.clip = c;
    v.def = &def;
    v.tag = tag;
    v.loop = p.loop < 0 ? def.loop : p.loop != 0;
    v.gain = p.gain;
    const auto g = table_.groups.find(def.group);
    const std::string slider = g != table_.groups.end() ? g->second.slider : "SFX";
    v.bus = slider == "MUSIC" ? music : slider == "VFX" ? voices : sfx;
    v.positional = p.position && g != table_.groups.end() && g->second.positional;
    if (p.position) v.position = *p.position;
    if (p.fade_in_ms > 0) {
        v.fade = 0;
        v.fade_step = 1.0f / (p.fade_in_ms * 0.001f * float(rate_));
    }
    spatialize(v);
    v.cur_left = v.left, v.cur_right = v.right;
    Lock lock(mutex_);
    v.handle = next_handle_++;
    v.started = ++counter_;
    // Bank limits: steal the oldest voice of the bank (or the lowest priority one).
    if (auto b = table_.banks.find(def.bank); b != table_.banks.end()) {
        std::vector<size_t> same;
        for (size_t i = 0; i < voices_.size(); ++i)
            if (voices_[i].def && voices_[i].def->bank == def.bank && !voices_[i].stopping) same.push_back(i);
        if (int(same.size()) >= b->second.max_playbacks) {
            size_t victim = same.front();
            for (size_t i : same) {
                const Voice& a = voices_[i];
                const Voice& cur = voices_[victim];
                const bool lower = b->second.steal_lowest_priority && a.def->priority != cur.def->priority
                                       ? a.def->priority < cur.def->priority
                                       : a.started < cur.started;
                if (lower) victim = i;
            }
            if (b->second.steal_lowest_priority && voices_[victim].def->priority > def.priority) return -1;
            voices_.erase(voices_.begin() + long(victim));
        }
    }
    if (trace)
        std::printf("[sound] %s%s%s\n", def.label.c_str(), v.loop ? " (loop)" : "",
                    v.positional ? (" at " + std::to_string(int(glm::length(v.position - listener_))) + " m").c_str() : "");
    voices_.push_back(std::move(v));
    return voices_.back().handle;
}

int Audio::play(const std::string& name, const Params& p) {
    const SoundDef* def = table_.sound(name);
    if (!def) {
        if (table_.event(name)) return play_event(name, p);
        if (warned_.insert(name).second) std::fprintf(stderr, "audio: no sound %s\n", name.c_str());
        return -1;
    }
    return start(*def, p, {});
}

int Audio::play_event(const std::string& event, const Params& p) {
    const SoundEvent* e = table_.event(event);
    if (!e || e->sounds.empty()) {
        if (warned_.insert(event).second) std::fprintf(stderr, "audio: no sound event %s\n", event.c_str());
        return -1;
    }
    const int uid = e->sounds[std::uniform_int_distribution<size_t>(0, e->sounds.size() - 1)(rng_)];
    if (uid < 0 || size_t(uid) >= table_.sounds.size() || table_.sounds[size_t(uid)].uid < 0) return -1;
    return start(table_.sounds[size_t(uid)], p, e->label);
}

int Audio::play_one_of(const std::vector<std::string>& names, const Params& p) {
    if (names.empty()) return -1;
    return play(names[std::uniform_int_distribution<size_t>(0, names.size() - 1)(rng_)], p);
}

void Audio::stop(const std::string& name, float fade_ms) {
    const SoundDef* def = table_.sound(name);
    const SoundEvent* ev = table_.event(name);
    Lock lock(mutex_);
    for (Voice& v : voices_) {
        if (!((def && v.def == def) || (ev && v.tag == ev->label))) continue;
        v.stopping = true;
        v.fade_target = 0;
        v.fade_step = fade_ms > 0 ? 1.0f / (fade_ms * 0.001f * float(rate_)) : 1.0f;
    }
}

void Audio::stop(int handle, float fade_ms) {
    Lock lock(mutex_);
    for (Voice& v : voices_) {
        if (v.handle != handle) continue;
        v.stopping = true;
        v.fade_target = 0;
        v.fade_step = fade_ms > 0 ? 1.0f / (fade_ms * 0.001f * float(rate_)) : 1.0f;
    }
}

void Audio::stop_all() {
    Lock lock(mutex_);
    voices_.clear();
}

void Audio::set_position(int handle, const glm::vec3& position) {
    Lock lock(mutex_);
    for (Voice& v : voices_)
        if (v.handle == handle) v.position = position, spatialize(v);
}

bool Audio::playing(int handle) const {
    Lock lock(mutex_);
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.handle == handle; });
}

bool Audio::event_playing(const std::string& prefix) const {
    Lock lock(mutex_);
    return std::any_of(voices_.begin(), voices_.end(),
                       [&](const Voice& v) { return !v.stopping && v.tag.compare(0, prefix.size(), prefix) == 0 && !v.tag.empty(); });
}

void Audio::set_listener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    Lock lock(mutex_);
    listener_ = position;
    forward_ = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0, 1, 0);
    up_ = up;
    for (Voice& v : voices_) spatialize(v);
}

void Audio::callback(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* self = static_cast<Audio*>(user);
    const int frames = additional / int(2 * sizeof(float));
    if (frames <= 0) return;
    self->scratch_.assign(size_t(frames) * 2, 0.0f);
    {
        Lock lock(self->mutex_);
        self->mix(self->scratch_.data(), frames);
    }
    SDL_PutAudioStreamData(stream, self->scratch_.data(), frames * int(2 * sizeof(float)));
}

void Audio::render(float* out, int frames) {
    std::fill(out, out + frames * 2, 0.0f);
    Lock lock(mutex_);
    mix(out, frames);
}

void Audio::mix(float* out, int frames) {
    const float smooth = 1.0f / 256.0f;   // 3D gain changes glide over ~5 ms
    for (Voice& v : voices_) {
        const std::vector<float>& s = v.clip->samples;
        const size_t length = s.size() / 2;
        const float g = v.gain * v.bus * master;
        for (int f = 0; f < frames; ++f) {
            if (v.frame >= length) {
                if (!v.loop) break;
                v.frame = 0;
            }
            if (v.fade != v.fade_target) {
                v.fade = v.fade < v.fade_target ? std::min(v.fade + v.fade_step, v.fade_target)
                                                : std::max(v.fade - v.fade_step, v.fade_target);
            }
            v.cur_left += (v.left - v.cur_left) * smooth;
            v.cur_right += (v.right - v.cur_right) * smooth;
            const float k = g * v.fade;
            out[2 * f] += s[2 * v.frame] * k * v.cur_left;
            out[2 * f + 1] += s[2 * v.frame + 1] * k * v.cur_right;
            ++v.frame;
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& v) {
                                     return (v.stopping && v.fade <= 0) || (!v.loop && v.frame >= v.clip->samples.size() / 2);
                                 }),
                  voices_.end());
    for (int i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

}  // namespace nova
