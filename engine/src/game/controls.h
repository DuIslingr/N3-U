// Input schemes. The original is a touch game: its prompts name on-screen buttons
// (string ids without a suffix, with ^NN^ button glyphs) and it ships gamepad variants
// (<id>_XPLAY, _MOGA, _MOGA_PRO). On desktop this port plays with mouse and keyboard and
// words those prompts for them; on touch devices (or --touch) it keeps the original
// prompts and on-screen controls.
#pragma once

#include <string>

namespace nova {

enum class InputScheme { Desktop, Touch };

// The scheme for this build: touch on mobile platforms, desktop elsewhere.
InputScheme default_input_scheme();

// Desktop wording for a touch prompt (string id without STR_), or "" to keep the original.
std::string desktop_text(const std::string& id);

// The desktop key for a touch button glyph (^NN^), or "" if unknown.
std::string desktop_key(int glyph);

}  // namespace nova
