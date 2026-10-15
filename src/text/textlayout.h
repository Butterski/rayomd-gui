// SPDX-License-Identifier: Apache-2.0
// Text runs: UTF-8 to positioned glyphs, left to right, without kerning, ligatures or bidi.
#pragma once

#include "text/fontchoice.h"
#include "text/sfnt.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace rgui::text {

struct PositionedGlyph {
    const FontFace* face = nullptr;
    uint16_t glyph = 0;
    float x = 0;            // pen x of the glyph's origin from the run's start, in px
    float advance = 0;      // in px
    uint32_t byteOffset = 0;   // where its code point starts in the UTF-8 text
};

struct GlyphRun {
    std::vector<PositionedGlyph> glyphs;
    float advance = 0;      // the pen x after the last glyph
};

// One glyph per code point through cmap and the fallback chain (FontSet::Resolve), advancing
// by hmtx scaled to `sizePx` per em. Bytes that are not UTF-8 show as U+FFFD, one per byte.
// An unloaded FontSet gives an empty run.
GlyphRun ShapeRun(std::string_view utf8, const FontSet& fonts, float sizePx, bool bold = false);
// The same into `run` (cleared first), reusing its storage.
void ShapeRun(std::string_view utf8, const FontSet& fonts, float sizePx, bool bold, GlyphRun& run);

// A glyph's advance in px at `sizePx` per em.
float GlyphAdvance(const FontFace& face, uint16_t glyph, float sizePx);

} // namespace rgui::text
