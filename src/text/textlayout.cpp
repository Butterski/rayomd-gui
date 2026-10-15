// SPDX-License-Identifier: Apache-2.0
#include "text/textlayout.h"

#include "common/text_utils.h"

namespace rgui::text {

float GlyphAdvance(const FontFace& face, uint16_t glyph, float sizePx) {
    if (face.UnitsPerEm() == 0) return 0;
    return static_cast<float>(face.AdvanceWidth(glyph)) * sizePx / static_cast<float>(face.UnitsPerEm());
}

void ShapeRun(std::string_view utf8, const FontSet& fonts, float sizePx, bool bold, GlyphRun& run) {
    run.glyphs.clear();
    run.advance = 0;
    if (!fonts.IsLoaded()) return;
    run.glyphs.reserve(utf8.size());
    float x = 0;
    for (size_t at = 0; at < utf8.size();) {
        uint32_t cp = static_cast<unsigned char>(utf8[at]);
        size_t length = 1;
        if (cp >= 0x80 && !RayoMd::Text::DecodeUtf8(utf8, at, cp, length)) {
            cp = 0xFFFD;
            length = 1;
        }
        const FontSet::Resolved resolved = fonts.Resolve(cp, bold);
        PositionedGlyph g;
        g.face = resolved.face;
        g.glyph = resolved.glyph;
        g.x = x;
        g.advance = GlyphAdvance(*resolved.face, resolved.glyph, sizePx);
        g.byteOffset = static_cast<uint32_t>(at);
        run.glyphs.push_back(g);
        x += g.advance;
        at += length;
    }
    run.advance = x;
}

GlyphRun ShapeRun(std::string_view utf8, const FontSet& fonts, float sizePx, bool bold) {
    GlyphRun run;
    ShapeRun(utf8, fonts, sizePx, bold, run);
    return run;
}

} // namespace rgui::text
