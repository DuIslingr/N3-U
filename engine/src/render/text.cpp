#include "render/text.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstdio>
#include <cstring>

#include "assets/texture.h"
#include "core/vfs.h"

namespace nova {

TextRenderer::TextRenderer(const Vfs& vfs, Renderer& renderer, const std::string& font_file, float size) : renderer_(renderer) {
    if (!vfs.exists(font_file)) {
        std::fprintf(stderr, "text: no font %s\n", font_file.c_str());
        return;
    }
    if (!TTF_WasInit() && !TTF_Init()) {
        std::fprintf(stderr, "text: %s\n", SDL_GetError());
        return;
    }
    font_data_ = vfs.read(font_file);
    font_ = TTF_OpenFontIO(SDL_IOFromConstMem(font_data_.data(), font_data_.size()), true, size);
    if (!font_) std::fprintf(stderr, "text: %s: %s\n", font_file.c_str(), SDL_GetError());
}

TextRenderer::~TextRenderer() {
    if (font_) TTF_CloseFont(font_);
}

const TextRenderer::Line& TextRenderer::line(const std::string& text, int wrap) {
    const std::string key = std::to_string(wrap) + "|" + text;
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    Line l;
    if (SDL_Surface* s = TTF_RenderText_Blended_Wrapped(font_, text.c_str(), 0, SDL_Color{255, 255, 255, 255}, wrap)) {
        if (SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32)) {
            Image img;
            img.width = rgba->w, img.height = rgba->h;
            img.rgba.resize(size_t(rgba->w) * size_t(rgba->h) * 4);
            for (int y = 0; y < rgba->h; ++y)
                std::memcpy(img.rgba.data() + size_t(y) * size_t(rgba->w) * 4,
                            static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch), size_t(rgba->w) * 4);
            l.texture = renderer_.upload_texture(img);
            l.width = rgba->w, l.height = rgba->h;
            SDL_DestroySurface(rgba);
        }
        SDL_DestroySurface(s);
    }
    return cache_[key] = l;
}

glm::vec2 TextRenderer::measure(const std::string& text, int wrap) {
    if (!font_ || text.empty()) return glm::vec2(0);
    const Line& l = line(text, wrap);
    return glm::vec2(float(l.width), float(l.height));
}

float TextRenderer::draw(ParticleList& out, const std::string& text, glm::vec2 at, int wrap, const glm::vec4& color,
                         glm::vec2 anchor) {
    if (!font_ || text.empty()) return 0;
    const Line& l = line(text, wrap);
    if (!l.texture) return 0;
    auto quad = [&](glm::vec2 p, const glm::vec4& c) {
        const uint32_t rgba = pack_rgba(c);
        const float x0 = std::floor(p.x - float(l.width) * anchor.x), y0 = std::floor(p.y - float(l.height) * anchor.y);
        const float x1 = x0 + float(l.width), y1 = y0 + float(l.height);
        const ParticleVertex v[4] = {{{x0, y0, 0}, {0, 0}, rgba}, {{x1, y0, 0}, {1, 0}, rgba},
                                     {{x1, y1, 0}, {1, 1}, rgba}, {{x0, y1, 0}, {0, 1}, rgba}};
        const uint32_t first = uint32_t(out.vertices.size());
        out.vertices.insert(out.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        out.batches.push_back({l.texture, false, first, 6});
    };
    quad(at + glm::vec2(2, 2), glm::vec4(0, 0, 0, color.a * 0.8f));   // shadow
    quad(at, color);
    return float(l.height);
}

}  // namespace nova
