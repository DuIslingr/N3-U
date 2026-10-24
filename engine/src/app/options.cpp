#include "app/options.h"

#include <cstdio>
#include <stdexcept>

namespace nova {

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--data") o.data = next(), o.data_given = true;
        else if (a == "--extra-data") o.extra_data.push_back(next());
        else if (a == "--level") o.level = next(), o.level_given = true;
        else if (a == "--main-menu") o.main_menu_keys = next();
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--walk-test") o.walk_test = std::stof(next());
        else if (a == "--time") o.time = std::stof(next());
        else if (a == "--focus") o.focus = next();
        else if (a == "--builtin-shaders") o.builtin_shaders = true;
        else if (a == "--bench") o.bench = std::stoi(next());
        else if (a == "--skybox") o.skybox = std::stoi(next());
        else if (a == "--no-scripts") o.scripts = false;
        else if (a == "--trace-scripts") o.trace_scripts = true;
        else if (a == "--skip-intro") o.skip_intro = true;
        else if (a == "--run") o.run = std::stof(next());
        else if (a == "--every") o.every = std::stof(next());
        else if (a == "--respawn-at") o.respawn_at = std::stof(next());
        else if (a == "--use-at") o.use_at = std::stof(next());
        else if (a == "--spawn-at") o.spawn_at = next();
        else if (a == "--exec") o.exec.push_back(next());
        else if (a == "--effect") o.effects.push_back(next());
        else if (a == "--sound-check") o.sound_check = true;
        else if (a == "--touch") o.touch = true;
        else if (a == "--probe-model") o.probe_model = next();
        else if (a == "--probe") {
            float v[3];
            if (std::sscanf(next().c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) throw std::runtime_error("--probe needs X,Y,Z");
            o.probe = {v[0], v[1], v[2]}, o.have_probe = true;
        }
        else if (a == "--mute") o.mute = true;
        else if (a == "--sound-render") o.sound_render = next();
        else if (a == "--hide-models") o.hide_models.push_back(next());
        else if (a == "--no-zones") o.zones = false;
        else if (a == "--section") o.section = std::stoi(next());
        else if (a == "--trace-zones") o.trace_zones = true;
        else if (a == "--log-player") o.log_player = true;
        else if (a == "--combat-test") o.combat_test = std::stof(next());
        else if (a == "--grenade-test") o.grenade_test = std::stof(next());
        else if (a == "--mine-test") o.mine_test = std::stof(next());
        else if (a == "--power-test") o.power_test = std::stof(next());
        else if (a == "--shoot-at") o.shoot_at = next();
        else if (a == "--checkpoint-at") o.checkpoint_at = std::stof(next());
        else if (a == "--sprite-sheet") o.sprite_sheet = next();
        else if (a == "--menu") o.menu_keys = next();
        else if (a == "--credits") o.credits = std::stoi(next());
        else if (a == "--difficulty") o.difficulty = std::stoi(next());
        else if (a == "--save") o.save_file = next();
        else if (a == "--continue") o.continue_file = next();
        else if (a == "--loading-screen") o.loading_screen = std::stoi(next());
        else if (a == "--death-screen") o.death_screen = std::stof(next());
        else if (a == "--end-screen") o.end_screen = std::stof(next());
        else if (a == "--touch-test") o.touch_test = std::stof(next()), o.touch = true;
        else if (a == "--god") o.god = true;
        else if (a == "--list-actors") o.list_actors = true;
        else if (a == "--audit") o.audit = std::stoi(next());
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &o.width, &o.height), o.size_given = true;
        else if (a == "--eye") {
            float v[5] = {0, 0, 0, 0, 0};
            const int n = std::sscanf(next().c_str(), "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
            if (n < 4) throw std::runtime_error("--eye needs X,Y,Z,YAW[,PITCH]");
            o.eye = {v[0], v[1], v[2], v[3]};
            o.pitch = v[4];
            o.have_eye = true;
        } else {
            throw std::runtime_error("unknown option " + a);
        }
    }
    return o;
}

}  // namespace nova
