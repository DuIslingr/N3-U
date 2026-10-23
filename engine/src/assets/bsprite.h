// Gameloft sprite sheets (.bsprite, CSprite::LoadSpriteData): rectangles ("modules") of
// one or more images, placed in frames ("frame modules" with offsets and flip flags).
// See tools/bsprite.py.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/bytes.h"

namespace nova {

struct Sprite {
    struct Module {
        int image = 0;
        int x = 0, y = 0, w = 0, h = 0;   // pixels of the image
    };
    struct FModule {
        int module = 0;
        int flags = 0;                    // 1: flip horizontally, 2: flip vertically
        int x = 0, y = 0;                 // offset from the frame's anchor
    };
    struct Frame {
        int first = 0, count = 0;         // into fmodules
    };
    // Animations: a run of animation frames, each a frame shown for `time` ticks at an offset.
    struct AFrame {
        int frame = 0, time = 1, x = 0, y = 0, flags = 0;
    };
    struct Anim {
        int first = 0, count = 0;         // into aframes
    };
    std::vector<std::string> images;
    std::vector<Module> modules;
    std::vector<FModule> fmodules;
    std::vector<Frame> frames;
    std::vector<AFrame> aframes;
    std::vector<Anim> anims;
};

// Throws FormatError on a malformed file.
Sprite load_sprite(const Bytes& data);

}  // namespace nova
