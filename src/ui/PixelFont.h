#pragma once

// PixelFont.h — the single pixel font used by all Nyx UI text (FPS overlay,
// content browser, future panels). One font + one scale = consistent UI text.

#include <cstdint>

namespace Nyx {
namespace PixelFont {

constexpr int   CELL_W  = 5;     // glyph width  (px at scale 1)
constexpr int   CELL_H  = 7;     // glyph height (px at scale 1)
constexpr int   ADVANCE = 6;     // per-glyph advance = CELL_W + 1px gap (× scale)
constexpr float SCALE   = 1.0f;  // unified UI text scale

// Returns the 7 row bitmasks for a glyph (low CELL_W bits, bit CELL_W-1 =
// leftmost column), or nullptr if unsupported. Case-sensitive: covers the full
// printable ASCII range (32..126).
const uint8_t* glyphRows(char c);

} // namespace PixelFont
} // namespace Nyx
