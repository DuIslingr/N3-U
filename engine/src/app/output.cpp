#include "app/output.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include <SDL3/SDL.h>

namespace nova {

bool save_wav(const std::vector<float>& samples, int rate, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t data = uint32_t(samples.size() * 2);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f), u32(36 + data), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(2), u32(uint32_t(rate)), u32(uint32_t(rate) * 4), u16(4), u16(16);
    std::fwrite("data", 1, 4, f), u32(data);
    for (float x : samples) u16(uint16_t(int16_t(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f))));
    return std::fclose(f) == 0;
}

bool save_bmp(const Image& img, const std::string& path) {
    // Opaque, as the window shows it: the framebuffer's alpha (the HUD writes its own) is not
    // part of the picture.
    SDL_Surface* s = SDL_CreateSurfaceFrom(img.width, img.height, SDL_PIXELFORMAT_XBGR8888,
                                           const_cast<uint8_t*>(img.rgba.data()), img.width * 4);
    const bool ok = s && SDL_SaveBMP(s, path.c_str());
    SDL_DestroySurface(s);
    return ok;
}

}  // namespace nova
