#include "assets/sound_bank.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nova {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string strip_ext(std::string s) {
    for (const char* ext : {".wav", ".vxn"})
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ext) == 0) s.resize(s.size() - 4);
    return s;
}

// Attributes of one tag: name="value" pairs.
std::unordered_map<std::string, std::string> attributes(const std::string& tag) {
    std::unordered_map<std::string, std::string> out;
    size_t p = 0;
    while ((p = tag.find('=', p)) != std::string::npos) {
        size_t k = p;
        while (k > 0 && tag[k - 1] == ' ') --k;
        size_t start = k;
        while (start > 0 && (std::isalnum(static_cast<unsigned char>(tag[start - 1])) || tag[start - 1] == '_')) --start;
        const size_t q1 = tag.find('"', p);
        const size_t q2 = q1 == std::string::npos ? q1 : tag.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        out[tag.substr(start, k - start)] = tag.substr(q1 + 1, q2 - q1 - 1);
        p = q2 + 1;
    }
    return out;
}

float to_float(const std::unordered_map<std::string, std::string>& a, const char* key, float fallback) {
    auto it = a.find(key);
    return it == a.end() ? fallback : std::strtof(it->second.c_str(), nullptr);
}

int to_int(const std::unordered_map<std::string, std::string>& a, const char* key, int fallback) {
    auto it = a.find(key);
    return it == a.end() ? fallback : std::atoi(it->second.c_str());
}

std::string to_str(const std::unordered_map<std::string, std::string>& a, const char* key) {
    auto it = a.find(key);
    return it == a.end() ? std::string() : it->second;
}

}  // namespace

const SoundDef* SoundTable::sound(const std::string& name) const {
    const std::string key = lower(strip_ext(name));
    if (auto it = by_label_.find(key); it != by_label_.end()) return &sounds[size_t(it->second)];
    if (auto it = by_file_.find(key); it != by_file_.end()) return &sounds[size_t(it->second)];
    return nullptr;
}

const SoundEvent* SoundTable::event(const std::string& label) const {
    auto it = event_by_label_.find(lower(label));
    return it == event_by_label_.end() ? nullptr : &events[size_t(it->second)];
}

SoundTable parse_sound_table(const std::string& xml) {
    SoundTable t;
    size_t p = 0;
    while ((p = xml.find('<', p)) != std::string::npos) {
        const size_t end = xml.find('>', p);
        if (end == std::string::npos) break;
        const std::string tag = xml.substr(p + 1, end - p - 1);
        p = end + 1;
        const size_t space = tag.find(' ');
        const std::string kind = tag.substr(0, space);
        if (kind != "sound" && kind != "event" && kind != "group" && kind != "bank") continue;
        const auto a = attributes(tag);
        if (kind == "sound") {
            SoundDef s;
            s.uid = to_int(a, "uid", -1);
            if (s.uid < 0) continue;
            s.label = to_str(a, "label");
            s.filename = to_str(a, "filename");
            if (s.filename.empty()) s.filename = s.label + ".wav";
            s.format = to_str(a, "format");
            s.bank = to_int(a, "bank", 0);
            s.group = to_int(a, "group", 0);
            s.priority = to_int(a, "priority", 1);
            s.loop = to_str(a, "loop") == "yes";
            s.ref_distance = to_float(a, "refdistance", 0);
            s.max_distance = to_float(a, "maxdistance", 0);
            if (size_t(s.uid) >= t.sounds.size()) t.sounds.resize(size_t(s.uid) + 1);
            t.sounds[size_t(s.uid)] = std::move(s);
        } else if (kind == "event") {
            SoundEvent e;
            e.label = to_str(a, "label");
            while (!e.label.empty() && e.label.back() == ' ') e.label.pop_back();   // "env_m_background_MPLevel "
            const std::string v = to_str(a, "value");
            for (size_t q = 0; q < v.size();) {
                const size_t comma = v.find(',', q);
                e.sounds.push_back(std::atoi(v.substr(q, comma - q).c_str()));
                if (comma == std::string::npos) break;
                q = comma + 1;
            }
            t.events.push_back(std::move(e));
        } else if (kind == "group") {
            SoundGroup g;
            g.name = to_str(a, "name");
            g.slider = to_str(a, "volumeslider");
            g.positional = to_str(a, "pos3d") == "yes";
            t.groups[to_int(a, "uid", 0)] = g;
        } else {
            SoundBankLimit b;
            b.name = to_str(a, "name");
            b.max_playbacks = std::max(to_int(a, "maxplaybacks", 8), 1);
            b.steal_lowest_priority = to_str(a, "behaviour").find("priority") != std::string::npos;
            t.banks[to_int(a, "uid", 0)] = b;
        }
    }
    for (size_t i = 0; i < t.sounds.size(); ++i) {
        const SoundDef& s = t.sounds[i];
        if (s.uid < 0) continue;
        t.by_label_.emplace(lower(s.label), int(i));
        t.by_file_.emplace(lower(strip_ext(s.filename)), int(i));
    }
    for (size_t i = 0; i < t.events.size(); ++i) t.event_by_label_.emplace(lower(t.events[i].label), int(i));
    return t;
}

}  // namespace nova
