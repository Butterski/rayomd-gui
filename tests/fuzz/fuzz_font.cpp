// SPDX-License-Identifier: Apache-2.0
// Font programs: sfnt files and collections (FaceCount, ReadFaceInfo, FontFace::Open, cmap,
// hmtx, GlyphOutline for TrueType and OpenType CFF), else a bare CFF program as a PDF's
// FontFile3 holds it (CffOutlines with its charset and encoding). Outlines are rasterized at
// 32 px, as the glyph cache does.
#include "harness.h"

#include "gfx/geometry.h"
#include "gfx/path.h"
#include "gfx/raster.h"
#include "text/cff_outlines.h"
#include "text/sfnt.h"

namespace rgui::fuzz {
namespace {

class NullSink final : public gfx::SpanSink {
public:
    void Span(int, int, int width, const uint8_t* alpha) override {
        for (int i = 0; i < width; ++i) sum += alpha[i];
    }
    uint64_t sum = 0;
};

void Rasterize(const gfx::Path& path, const gfx::Matrix& unitsToPx) {
    static gfx::Rasterizer rasterizer;
    rasterizer.Reset(gfx::RectI{-8, -40, 40, 8});
    rasterizer.AddPath(path, unitsToPx);
    NullSink sink;
    rasterizer.Sweep(gfx::FillRule::NonZero, sink);
}

constexpr uint16_t kMaxGlyphs = 48;

void FuzzSfnt(const uint8_t* data, size_t size) {
    const uint32_t faces = text::FaceCount(data, size);
    for (uint32_t index = 0; index < faces && index < 4; ++index) {
        text::FaceInfo info;
        text::ReadFaceInfo(data, size, index, info);
        text::FontFace face;
        if (!face.Open(data, size, index)) continue;
        static const uint32_t kCodePoints[] = {0x20, 'A', 'g', 0xe9, 0x4e2d, 0x1f600, 0x10ffff};
        for (uint32_t cp : kCodePoints) (void)face.AdvanceWidth(face.GlyphForCodePoint(cp));
        const double em = face.UnitsPerEm() ? face.UnitsPerEm() : 1000;
        const gfx::Matrix toPx{32 / em, 0, 0, -32 / em, 0, 0};
        gfx::Path path;
        for (uint16_t glyph = 0; glyph < face.GlyphCount() && glyph < kMaxGlyphs; ++glyph) {
            if (text::GlyphOutline(face, glyph, path)) Rasterize(path, toPx);
        }
    }
}

void FuzzBareCff(const uint8_t* data, size_t size) {
    text::CffOutlines cff;
    if (!cff.Open(data, size)) return;
    for (uint16_t id : {0, 1, 34, 100, 1000}) (void)cff.Charset().GlyphFor(id);
    for (int code = 0; code < 256; code += 7) (void)cff.Encoding().GlyphFor(static_cast<uint8_t>(code));
    gfx::Path path;
    for (uint16_t glyph = 0; glyph < cff.GlyphCount() && glyph < kMaxGlyphs; ++glyph) {
        double width = 0;
        cff.Advance(glyph, width);
        const gfx::Matrix m = cff.GlyphMatrix(glyph);
        if (cff.GlyphPath(glyph, path)) Rasterize(path, m.Multiply(gfx::Matrix{32, 0, 0, -32, 0, 0}));
    }
}

} // namespace

int FuzzFont(const uint8_t* data, size_t size) {
    if (text::FaceCount(data, size) > 0) {
        FuzzSfnt(data, size);
    } else {
        FuzzBareCff(data, size);
    }
    return 0;
}

} // namespace rgui::fuzz
