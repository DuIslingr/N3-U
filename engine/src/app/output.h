// Files the app writes: screenshots and rendered sound.
#pragma once

#include <string>
#include <vector>

#include "assets/texture.h"

namespace nova {

// 16-bit stereo WAV from interleaved float samples.
bool save_wav(const std::vector<float>& samples, int rate, const std::string& path);
bool save_bmp(const Image& img, const std::string& path);

}  // namespace nova
