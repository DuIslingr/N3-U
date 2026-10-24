#include "audio/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <array>
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
    SDL_SetHint(SDL_HINT_WAVE_TRUNCATION, "dropframe");   // keep partial last blocks (vxn segments)
    SDL_SetHint(SDL_HINT_WAVE_FACT_CHUNK, "ignore");
    // The table of the main data, then any other version's mounted after it (the iOS
    // build's adds sounds this one lacks, such as the shop's).
    const auto& tables = vfs_.find("NOVA3_VOX_SDD.xml");
    for (size_t i = 0; i < tables.size(); ++i) {
        const Bytes xml = vfs_.read(tables[i]);
        SoundTable t = parse_sound_table(std::string(xml.begin(), xml.end()));
        if (i == 0) table_ = std::move(t);
        else merge_sound_table(table_, t);
    }
    if (const auto& banks = vfs_.find("nova3_reverbs.fxb"); !banks.empty()) reverb_presets_ = load_reverb_bank(vfs_.read(banks.front()));
    reverb_.set_rate(rate_);
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
        if (s.uid < 0) continue;
        auto c = clip(s);
        if (!c || c->samples.empty()) failed.push_back(s.label + " (" + s.filename + ")");
        clips_.clear();   // keep memory flat
    }
    return failed;
}

bool Audio::decode_wav(const Bytes& wav, std::vector<float>& out) const {
    SDL_AudioSpec src{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(wav.data(), wav.size()), true, &src, &buf, &len)) return false;
    const SDL_AudioSpec dst{SDL_AUDIO_F32, 2, rate_};
    Uint8* conv = nullptr;
    int conv_len = 0;
    const bool ok = SDL_ConvertAudioSamples(&src, buf, int(len), &dst, &conv, &conv_len);
    if (ok) {
        const size_t at = out.size();
        out.resize(at + size_t(conv_len) / sizeof(float));
        std::memcpy(out.data() + at, conv, size_t(conv_len));
        SDL_free(conv);
    }
    SDL_free(buf);
    return ok;
}

namespace {

uint32_t rd32(const Bytes& d, size_t p) { return p + 4 <= d.size() ? uint32_t(d[p] | d[p + 1] << 8 | d[p + 2] << 16 | uint32_t(d[p + 3]) << 24) : 0; }
uint16_t rd16(const Bytes& d, size_t p) { return p + 2 <= d.size() ? uint16_t(d[p] | d[p + 1] << 8) : 0; }

void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); }

}  // namespace

std::shared_ptr<Audio::Clip> Audio::decode_vxn(const Bytes& d) const {
    // "VoxN", u32 16, version, u32 file size, u32 header size; then chunks (id, u32 size):
    // Afmt {u16 format (1 PCM, 2 MS ADPCM, 0x11 IMA ADPCM), u16 channels, u32 rate,
    // u16 block align, u16 bits}, Msae (MS ADPCM: samples per block, coefficients),
    // Segm {u32 count, {u32 byte offset, u32 byte size, u32 samples, 3 x u32}}, interactive
    // music rules, Data (the sound).
    if (d.size() < 0x18 || std::memcmp(d.data(), "VoxN", 4) != 0) return nullptr;
    uint16_t format = 0, channels = 0, block = 0, bits = 0;
    uint32_t rate = 0;
    Bytes msae;
    std::vector<std::array<uint32_t, 3>> segments;
    std::vector<std::string> states;
    uint32_t playlist_count = 0;
    std::vector<std::array<int32_t, 3>> elements, transitions;   // {playlist, segment, plays}, {from, to, rule}
    float rule_out = 0, rule_in = 0;
    size_t data = 0, data_size = 0;
    for (size_t p = 0x18; p + 8 <= d.size();) {
        const uint32_t n = rd32(d, p + 4);
        const char* id = reinterpret_cast<const char*>(d.data() + p);
        if (std::memcmp(id, "Afmt", 4) == 0) {
            format = rd16(d, p + 8), channels = rd16(d, p + 10), rate = rd32(d, p + 12);
            block = rd16(d, p + 16), bits = rd16(d, p + 18);
        } else if (std::memcmp(id, "Msae", 4) == 0) {
            msae.assign(d.begin() + long(p + 8), d.begin() + long(std::min(d.size(), p + 8 + n)));
        } else if (std::memcmp(id, "Segm", 4) == 0) {
            for (uint32_t i = 0; i < rd32(d, p + 8); ++i)
                segments.push_back({rd32(d, p + 12 + 24 * i), rd32(d, p + 16 + 24 * i), rd32(d, p + 20 + 24 * i)});
        } else if (std::memcmp(id, "Stat", 4) == 0 && rd32(d, p + 8)) {
            // {i32 index, char name[...]}, as many as fit.
            const uint32_t count = rd32(d, p + 8), each = (n - 4) / count;
            for (uint32_t i = 0; i < count; ++i) {
                const size_t at = p + 12 + size_t(i) * each + 4;
                std::string name;
                for (size_t k = at; k < std::min<size_t>(at + each - 4, d.size()) && d[k]; ++k) name += char(d[k]);
                states.push_back(name);
            }
        } else if (std::memcmp(id, "Plst", 4) == 0) {
            playlist_count = rd32(d, p + 8);
        } else if (std::memcmp(id, "Grpe", 4) == 0 && rd32(d, p + 8)) {
            // {playlist, group, element, segment, ?, ?, plays (-1: loops), ?}
            const uint32_t count = rd32(d, p + 8), each = (n - 4) / count;
            for (uint32_t i = 0; i < count && each >= 28; ++i) {
                const size_t at = p + 12 + size_t(i) * each;
                elements.push_back({int32_t(rd32(d, at)), int32_t(rd32(d, at + 12)), int32_t(rd32(d, at + 24))});
            }
        } else if (std::memcmp(id, "Trsn", 4) == 0 && rd32(d, p + 8)) {
            // {from state, to state, rule (-1: none), ?}
            const uint32_t count = rd32(d, p + 8), each = (n - 4) / count;
            for (uint32_t i = 0; i < count && each >= 12; ++i) {
                const size_t at = p + 12 + size_t(i) * each;
                transitions.push_back({int32_t(rd32(d, at)), int32_t(rd32(d, at + 4)), int32_t(rd32(d, at + 8))});
            }
        } else if (std::memcmp(id, "Rule", 4) == 0 && rd32(d, p + 8) && n >= 4 + 28) {
            // The first rule: {?, ?, ?, ?, float fade out, float fade in, float ?, ...} (s).
            float out = 0, in = 0;
            const uint32_t a = rd32(d, p + 28), b = rd32(d, p + 32);
            std::memcpy(&out, &a, 4), std::memcpy(&in, &b, 4);
            rule_out = std::clamp(out, 0.0f, 5.0f), rule_in = std::clamp(in, 0.0f, 5.0f);
        } else if (std::memcmp(id, "Data", 4) == 0) {
            data = p + 8, data_size = std::min<size_t>(n, d.size() - data);
            break;
        }
        p += 8 + n;
    }
    if (!data || !channels || !rate) return nullptr;
    if (segments.empty()) segments.push_back({0, uint32_t(data_size), 0});
    // A WAV header for one segment's bytes.
    auto wav = [&](size_t off, size_t size) {
        Bytes fmt;
        uint16_t spb = 0;
        if (format == 2) spb = rd16(msae, 0);
        if (format == 0x11) spb = uint16_t((block - 4 * channels) * 8 / (4 * channels) + 1);
        put16(fmt, format), put16(fmt, channels), put32(fmt, rate);
        put32(fmt, format == 1 ? rate * block : uint32_t(uint64_t(rate) * block / std::max<uint16_t>(spb, 1)));
        put16(fmt, block), put16(fmt, format == 1 ? bits : 4);
        if (format == 2) put16(fmt, uint16_t(msae.size())), fmt.insert(fmt.end(), msae.begin(), msae.end());
        if (format == 0x11) put16(fmt, 2), put16(fmt, spb);
        Bytes w;
        for (char c : std::string("RIFF")) w.push_back(uint8_t(c));
        put32(w, uint32_t(4 + 8 + fmt.size() + 8 + size));
        for (char c : std::string("WAVEfmt ")) w.push_back(uint8_t(c));
        put32(w, uint32_t(fmt.size()));
        w.insert(w.end(), fmt.begin(), fmt.end());
        for (char c : std::string("data")) w.push_back(uint8_t(c));
        put32(w, uint32_t(size));
        w.insert(w.end(), d.begin() + long(data + off), d.begin() + long(data + off + size));
        return w;
    };
    auto c = std::make_shared<Clip>();
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& [off, size, samples] = segments[i];
        const size_t at = c->samples.size() / 2;
        if (off >= data_size) {
            c->segments.emplace_back(at, at);
            continue;
        }
        std::vector<float> seg;
        if (!decode_wav(wav(off, std::min<size_t>(size, data_size - off)), seg)) return nullptr;
        // Trim (or pad) to the segment's stated length, at the device rate.
        if (samples) seg.resize(size_t(double(samples) * rate_ / rate) * 2, 0.0f);
        c->samples.insert(c->samples.end(), seg.begin(), seg.end());
        c->segments.emplace_back(at, c->samples.size() / 2);
        if (i == 1) c->loop_start = at, c->loop_end = c->samples.size() / 2;
    }
    if (c->loop_end <= c->loop_start) c->loop_start = 0, c->loop_end = c->samples.size() / 2;
    // The states' playlists (state i plays playlist i), and which changes are allowed.
    if (playlist_count && !elements.empty()) {
        c->playlists.resize(playlist_count);
        for (const auto& [pl, seg, plays] : elements)
            if (pl >= 0 && pl < int(playlist_count) && seg >= 0 && seg < int(c->segments.size()))
                c->playlists[size_t(pl)].push_back({seg, plays < 0});
        c->states = states;
        c->states.resize(playlist_count);
        c->transitions.assign(playlist_count, std::vector<bool>(playlist_count, true));
        for (const auto& [from, to, rule] : transitions)
            if (from >= 0 && to >= 0 && from < int(playlist_count) && to < int(playlist_count)) c->transitions[size_t(from)][size_t(to)] = rule >= 0;
        c->fade_out = size_t(rule_out * float(rate_)), c->fade_in = size_t(rule_in * float(rate_));
    }
    return c;
}

std::shared_ptr<const Audio::Clip> Audio::clip(const SoundDef& def) {
    if (auto it = clips_.find(def.filename); it != clips_.end()) return it->second;
    std::shared_ptr<Clip> c;
    if (!vfs_.exists(def.filename)) {
        if (warned_.insert(def.filename).second) std::fprintf(stderr, "audio: no file %s\n", def.filename.c_str());
    } else {
        const Bytes data = vfs_.read(def.filename);
        if (data.size() >= 4 && std::memcmp(data.data(), "VoxN", 4) == 0) {
            c = decode_vxn(data);
        } else {
            c = std::make_shared<Clip>();
            if (!decode_wav(data, c->samples)) c.reset();
            else c->loop_end = c->samples.size() / 2;
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
    v.gain = p.gain, v.ui = p.ui;
    if (c->interactive()) {   // its first state's playlist, which says what loops
        v.loop = false;
        set_state(v, 0);
        if (v.done) return -1;
    }
    const auto g = table_.groups.find(def.group);
    const std::string slider = g != table_.groups.end() ? g->second.slider : "SFX";
    v.bus = slider == "MUSIC" ? music : slider == "VFX" ? voices : sfx;
    v.positional = p.position && g != table_.groups.end() && g->second.positional;
    v.aux = g != table_.groups.end() && g->second.bus == "AUX1";
    if (p.position) v.position = *p.position;
    if (p.fade_in_ms > 0) {
        v.fade = 0;
        v.fade_step = 1.0f / (p.fade_in_ms * 0.001f * float(rate_));
    }
    // One-shots out of earshot are not started (they would only take a voice).
    if (v.positional && !v.loop) {
        const float max = def.max_distance > 0 ? def.max_distance : 60.0f;
        if (glm::distance(v.position, listener_) > max * 1.25f) return -1;
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

void Audio::set_reverb_preset(const std::string& name, float ms) {
    // VoxSoundManager::SetDSPPreset: the bank's patch of that name (strcasecmp: a name with
    // trailing spaces, as a few scripts write, finds none), else vox::ReverbHQParameters' own.
    ReverbParams p;
    for (const ReverbPreset& r : reverb_presets_)
        if (r.name.size() == name.size() &&
            std::equal(r.name.begin(), r.name.end(), name.begin(), [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); })) {
            p = r.params;
            break;
        }
    Lock lock(mutex_);
    reverb_.set_params(p, ms / 1000.0f);
}

void Audio::set_paused(bool paused, const std::string& except) {
    Lock lock(mutex_);
    paused_ = paused, unpaused_ = paused ? except : std::string();
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

bool Audio::playing(const std::string& name) const {
    Lock lock(mutex_);
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) { return !v.stopping && v.def && (v.def->label == name || v.def->filename == name); });
}

const Audio::Clip::Element* Audio::element(const Voice& v) const {
    const auto& list = v.clip->playlists[size_t(v.state)];
    return v.element < int(list.size()) ? &list[size_t(v.element)] : nullptr;
}

void Audio::next_element(Voice& v) const {
    const Clip::Element* e = element(v);
    if (e && e->loop) {
        v.frame = v.clip->segments[size_t(e->segment)].first;
        return;
    }
    ++v.element;
    e = element(v);
    if (!e) {
        v.done = true;
        return;
    }
    v.frame = v.clip->segments[size_t(e->segment)].first;
}

void Audio::set_state(Voice& v, int state) const {
    const Clip& c = *v.clip;
    if (state < 0 || state >= int(c.playlists.size())) return;
    if (!v.done && v.started) {
        // What plays now fades out by the rule while the new state's playlist starts.
        if (const Clip::Element* e = element(v); e && c.fade_out > 0) {
            v.tail_frame = v.frame, v.tail_end = c.segments[size_t(e->segment)].second;
            v.tail_gain = 1, v.tail_step = 1.0f / float(c.fade_out);
        }
        if (c.fade_in > 0) v.fade = 0, v.fade_target = 1, v.fade_step = 1.0f / float(c.fade_in);
    }
    v.state = state, v.element = 0, v.done = false;
    const Clip::Element* e = element(v);
    if (!e) {
        v.done = true;
        return;
    }
    v.frame = c.segments[size_t(e->segment)].first;
}

void Audio::set_state(int handle, const std::string& state) {
    Lock lock(mutex_);
    for (Voice& v : voices_) {
        if (v.handle != handle || !v.clip->interactive() || v.stopping) continue;
        const auto& names = v.clip->states;
        const auto it = std::find(names.begin(), names.end(), state);
        if (it == names.end()) continue;
        const int to = int(it - names.begin());
        if (to == v.state || !v.clip->transitions[size_t(v.state)][size_t(to)]) continue;   // not a change the file allows
        if (trace) std::printf("[sound] %s: %s -> %s\n", v.def ? v.def->label.c_str() : "?", names[size_t(v.state)].c_str(), state.c_str());
        set_state(v, to);
    }
}

void Audio::set_state(const std::string& name, const std::string& state) {
    std::vector<int> handles;
    {
        Lock lock(mutex_);
        for (const Voice& v : voices_)
            if (v.def && (v.def->label == name || v.def->filename == name)) handles.push_back(v.handle);
    }
    for (int h : handles) set_state(h, state);
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

void Audio::mix(float* final_out, int frames) {
    const float smooth = 1.0f / 256.0f;   // 3D gain changes glide over ~5 ms
    aux_.assign(size_t(frames) * 2, 0.0f);
    for (Voice& v : voices_) {
        float* out = v.aux ? aux_.data() : final_out;
        if (paused_ && !v.ui && !(v.def && !unpaused_.empty() && v.def->label == unpaused_)) continue;   // held
        const std::vector<float>& s = v.clip->samples;
        const bool interactive = v.clip->interactive();
        size_t length = v.loop ? v.clip->loop_end : s.size() / 2;
        if (interactive) length = v.done ? 0 : v.clip->segments[size_t(element(v)->segment)].second;
        const float g = v.gain * v.bus * master;
        for (int f = 0; f < frames; ++f) {
            if (interactive && v.tail_gain > 0 && v.tail_frame < v.tail_end) {   // the old state's segment fading out
                const float k = g * (v.stopping ? v.fade : 1.0f) * v.tail_gain;   // not the new state's fade-in
                out[2 * f] += s[2 * v.tail_frame] * k * v.cur_left;
                out[2 * f + 1] += s[2 * v.tail_frame + 1] * k * v.cur_right;
                ++v.tail_frame, v.tail_gain -= v.tail_step;
            }
            if (interactive) {
                while (!v.done && v.frame >= length) {
                    next_element(v);
                    if (!v.done) length = v.clip->segments[size_t(element(v)->segment)].second;
                }
                if (v.done) break;
            } else if (v.frame >= length) {
                if (!v.loop) break;
                v.frame = v.clip->loop_start;
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
                                     if (v.stopping && v.fade <= 0) return true;
                                     if (v.clip->interactive()) return v.done;
                                     return !v.loop && v.frame >= v.clip->samples.size() / 2;
                                 }),
                  voices_.end());
    reverb_.process(aux_.data(), final_out, frames);   // MiniAuxBus: AUX1 through its DSP
    for (int i = 0; i < frames * 2; ++i) final_out[i] = std::clamp(final_out[i], -1.0f, 1.0f);
}

}  // namespace nova
