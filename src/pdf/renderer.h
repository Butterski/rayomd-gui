// SPDX-License-Identifier: Apache-2.0
// The preview renderer (plan Phase 6 step 2): one page of a PdfFile into an image, by
// interpreting its content stream with gfx::Canvas.
//
// What it draws: paths (fill, stroke, both; nonzero and even-odd), clips (W, W*), the line
// state (w J j M d), colours in DeviceGray, DeviceRGB and DeviceCMYK (CMYK -> RGB as
// (1 - c)(1 - k), ...), ExtGState /ca /CA /LW /LC /LJ /ML /D, text in the fonts of pdf/fonts.h
// (render modes 0-3; 4-7 as 0-3), image XObjects (pdf/images.h), form XObjects nested at most
// 16 deep, and axial shadings (ShadingType 2, FunctionType 2 or 3 of type-2 functions). Marked
// content, compatibility sections and inline images are skipped. Whatever else a page uses is
// left out and named in RenderStats::unknown.
//
// Thread safety: any number of threads may render pages of the same PdfFile at once. Each
// thread keeps its own glyph cache and canvas (thread_local); fonts and images come from the
// thread-safe caches of the environment.
#pragma once

#include "gfx/surface.h"
#include "pdf/fonts.h"
#include "pdf/images.h"
#include "pdf/reader.h"
#include "text/fontindex.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::pdf {

struct RenderStats {
    // What the page uses that the renderer does not draw, each once, in the order met:
    // "op:<operator>", "font:<what>", "image:<reason>", "shading:<what>", "shading-exponent",
    // "colorspace:<name>", "pattern", "filter:<name>", "contents", "form-depth", "operators",
    // "gs:<key>".
    std::vector<std::string> unknown;
    uint32_t operators = 0;     // operators run, form contents included
    uint32_t glyphs = 0;        // glyphs drawn: from the glyph cache ...
    uint32_t glyphOutlines = 0; // ... and of those, filled or stroked as outlines
    uint32_t images = 0;        // images drawn

    void AddUnknown(std::string_view what);
};

// What rendering uses from outside the PDF. Shared by every document and thread; each pointer
// must outlive the renders that use it.
struct RenderEnvironment {
    // System faces for the standard fonts; nullptr draws no standard-font text
    // ("font:no-substitute:...").
    const text::FontIndex* fontIndex = nullptr;
    // Decoded images; nullptr decodes them on every use.
    ImageCache* images = nullptr;
    // Read fonts; nullptr reads them for each render.
    FontCache* fonts = nullptr;
};

// The process's environment: a font index of the default directories (started on first use),
// SharedImageCache() and SharedFontCache().
const RenderEnvironment& DefaultRenderEnvironment();

// Page sizes the renderer refuses: wider or taller than this, or more pixels than kMaxPixels.
inline constexpr int kMaxRenderSide = 16384;
inline constexpr uint64_t kMaxRenderPixels = 1ull << 26;

// Renders page `page` (0-based) at `widthPx` pixels across its MediaBox, the height in
// proportion (rounded, at least 1), onto white: `out` is opaque, `out.id` and `out.version` are
// left as they were. The base transform is Matrix(s, 0, 0, -s, -x0 * s, y1 * s), s = widthPx /
// MediaBox width. False, with `out` unchanged, for a page that does not exist or a size out of
// range; a page whose content cannot be read is white (true, "contents" or "filter:..." in
// `stats`). `stats` may be nullptr; it is cleared first.
bool RenderPage(const PdfFile& file, int page, int widthPx, gfx::Image& out, RenderStats* stats,
                const RenderEnvironment& environment);
// The same with DefaultRenderEnvironment().
bool RenderPage(const PdfFile& file, int page, int widthPx, gfx::Image& out, RenderStats* stats);

} // namespace rgui::pdf
