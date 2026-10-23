// Text for the HUD (subtitles, messages): lines rendered with SDL_ttf from the game's own
// fonts (sprites/*.ttf), cached as textures, drawn as screen-space sprites.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

#include "core/bytes.h"
#include "render/renderer.h"

struct TTF_Font;

namespace nova {

class Vfs;

class TextRenderer {
public:
    TextRenderer(const Vfs& vfs, Renderer& renderer, const std::string& font_file, float size);
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;
    bool ok() const { return font_ != nullptr; }

    // Draw `text` (UTF-8, wrapped at `wrap` pixels) into `out`, `at` (pixels) being the point
    // `anchor` of its box (0,0 top left, 0.5,0 top centre, 0.5,0.5 centre); returns its height.
    float draw(ParticleList& out, const std::string& text, glm::vec2 at, int wrap, const glm::vec4& color,
               glm::vec2 anchor = {0.5f, 0.0f});

private:
    struct Line {
        const GpuTexture* texture = nullptr;
        int width = 0, height = 0;
    };
    const Line& line(const std::string& text, int wrap);

    Renderer& renderer_;
    Bytes font_data_;
    TTF_Font* font_ = nullptr;
    std::unordered_map<std::string, Line> cache_;
};

}  // namespace nova
