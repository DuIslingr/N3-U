#include "game/settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

namespace nova {

std::string Settings::default_path() {
    char* dir = SDL_GetPrefPath("nova3", "nova3");
    if (!dir) return {};
    std::string path = std::string(dir) + "settings.ini";
    SDL_free(dir);
    return path;
}

Settings Settings::load(const std::string& path) {
    Settings s;
    if (path.empty()) return s;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        const bool on = value == "1";
        const float f = std::strtof(value.c_str(), nullptr);
        const int i = std::atoi(value.c_str());
        if (key == "sound") s.sound = on;
        else if (key == "sfx_volume") s.sfx_volume = std::clamp(f, 0.0f, 1.0f);
        else if (key == "music_volume") s.music_volume = std::clamp(f, 0.0f, 1.0f);
        else if (key == "sensitivity") s.sensitivity = std::clamp(f, 0.25f, 2.5f);
        else if (key == "invert_y") s.invert_y = on;
        else if (key == "left_handed") s.left_handed = on;
        else if (key == "aim_toggle") s.aim_toggle = on;
        else if (key == "aim_assist") s.aim_assist = on;
        else if (key == "auto_switch") s.auto_switch = on;
        else if (key == "auto_leveling") s.auto_leveling = on;
        else if (key == "metric") s.metric = on;
        else if (key == "width") s.width = std::clamp(i, 640, 3840);
        else if (key == "height") s.height = std::clamp(i, 360, 2160);
        else if (key == "display") s.display = DisplayMode(std::clamp(i, 0, 2));
        else if (key == "vsync") s.vsync = on;
        else if (key == "fps_limit") s.fps_limit = std::clamp(i, 0, 1000);
        else if (key == "chapters") s.chapters = std::max(i, 1);
    }
    return s;
}

bool Settings::save(const std::string& path) const {
    if (path.empty()) return false;
    std::ofstream out(path);
    out << "sound=" << sound << "\nsfx_volume=" << sfx_volume << "\nmusic_volume=" << music_volume << "\nsensitivity=" << sensitivity
        << "\ninvert_y=" << invert_y << "\nleft_handed=" << left_handed << "\naim_toggle=" << aim_toggle << "\naim_assist=" << aim_assist
        << "\nauto_switch=" << auto_switch << "\nauto_leveling=" << auto_leveling << "\nmetric=" << metric << "\nwidth=" << width
        << "\nheight=" << height << "\ndisplay=" << int(display) << "\nvsync=" << vsync << "\nfps_limit=" << fps_limit << "\nchapters=" << chapters << "\n";
    return bool(out);
}

}  // namespace nova
