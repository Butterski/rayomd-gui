// SPDX-License-Identifier: Apache-2.0
// The preview renderer (plan Phase 6 step 2): one page of a PdfFile, or a band of its rows, into
// an image, by interpreting its content stream with gfx::Canvas.
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

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::pdf {

struct RenderStats {
    // What the page uses that the renderer does not draw, each once, in the order met:
    // "op:<operator>", "font:<what>", "image:<reason>", "shading:<what>", "shading-exponent",
    // "colorspace:<name>", "pattern", "filter:<name>", "contents", "form-depth", "operators",
    // "gs:<key>", "clip-depth"; and where the page's limits stopped it: "budget" (glyphs,
    // pixels, dashes or decoded image bytes), "geometry" (segments and outline points).
    std::vector<std::string> unknown;
    uint32_t operators = 0;      // operators run, form contents included
    uint32_t glyphs = 0;         // glyphs shown: from the glyph cache ...
    uint32_t glyphOutlines = 0;  // ... and of those, filled or stroked as outlines
    uint32_t glyphsCulled = 0;   // ... and of those, left out wholly outside the clip
    uint32_t images = 0;         // images drawn ...
    uint32_t imagesReused = 0;   // ... and of those, from samples an earlier render took
    // What the page spent of its limits (RenderLimits).
    uint64_t geometry = 0;       // flattened segments and stroke outline points
    uint64_t pixels = 0;         // pixels swept or written
    uint64_t dashes = 0;
    uint64_t imageBytes = 0;     // decode cost of the images decoded for it (ImageDecodeCost)
    uint64_t maskBytes = 0;      // the most bytes of clip masks held at once
    uint64_t glyphCacheBytes = 0;  // the rendering thread's glyph cache after the page
    bool cancelled = false;      // RenderEnvironment::cancel was set: the render was abandoned

    void AddUnknown(std::string_view what);
};

// What rendering uses from outside the PDF. Shared by every document and thread; each pointer
// must outlive the renders that use it.
struct RenderEnvironment {
    // System faces for the standard fonts; nullptr draws no standard-font text
    // ("font:no-substitute:..."). On Windows Arial, Courier New and Times New Roman are opened by
    // file name first, so a render never waits for the index's scan when they are there.
    const text::FontIndex* fontIndex = nullptr;
    // Decoded images; nullptr decodes them on every use.
    ImageCache* images = nullptr;
    // Read fonts; nullptr reads them for each render.
    FontCache* fonts = nullptr;
    // Set to true by another thread to abandon renders that use this environment: they stop
    // within about a millisecond (the flag is polled every 64 operators, every glyph, dash and
    // band of 16 rows, every 4096 segments) and return false. nullptr: never.
    const std::atomic<bool>* cancel = nullptr;
};

// The process's environment: a font index of the default directories (started on first use),
// SharedImageCache() and SharedFontCache().
const RenderEnvironment& DefaultRenderEnvironment();

// Page sizes the renderer refuses: wider or taller than this, or more pixels than kMaxPixels.
inline constexpr int kMaxRenderSide = 16384;
inline constexpr uint64_t kMaxRenderPixels = 1ull << 26;
// The tallest page RenderPageRows draws rows of.
inline constexpr int kMaxPageRows = 1 << 20;

// What one page may cost. A page that reaches a limit stops there: what it drew so far is the
// image (true), with "budget" or "geometry" in RenderStats::unknown. Every limit is far above
// what a page of the engine's needs (tester.md's pages: under 50,000 segments, 3 x their pixels,
// a few hundred glyphs).
struct RenderLimits {
    static constexpr uint32_t kOperators = 1u << 21;     // "operators"
    static constexpr uint32_t kGlyphs = 1u << 18;        // glyphs shown
    static constexpr uint64_t kGeometry = 1u << 21;      // segments rasterized + stroke outline points
    static constexpr uint64_t kDashes = 1u << 18;
    // Pixels swept or written: this many times the page's pixels, plus kPixelsBase.
    static constexpr uint64_t kPixelsPerPagePixel = 64;
    static constexpr uint64_t kPixelsBase = 1u << 24;
    // Decode cost of the images decoded (ImageDecodeCost: 4 bytes a pixel, 1 a mask pixel).
    static constexpr uint64_t kImageBytes = uint64_t(512) << 20;
    // Decoded images the page keeps for its later uses beyond those the image cache keeps;
    // past this, an image is decoded again (and paid again) at each use.
    static constexpr uint64_t kHeldImageBytes = uint64_t(64) << 20;
    // Clips deeper than this, or clip masks past kMaskPerPagePixel times the page's pixels plus
    // kMaskBase bytes, are intersected into the top clip ("clip-depth"); a clip set again
    // before a q always is, so masks follow the q nesting (at most 64 deep), not the clip count.
    static constexpr size_t kClipDepth = 64;
    static constexpr uint64_t kMaskPerPagePixel = 4;
    static constexpr uint64_t kMaskBase = 16u << 20;
    // Glyphs bigger than this many pixels per em are filled as outlines, not cached.
    static constexpr double kMaxCachedGlyphSize = 256;
};

// The height in pixels of a page whose MediaBox is `boxWidth` x `boxHeight` drawn `widthPx`
// across: round(boxHeight * widthPx / boxWidth), at least 1. 0 when it has none: a box side
// that is not positive and finite, a width outside [1, kMaxRenderSide], over kMaxPageRows.
int RenderHeight(double boxWidth, double boxHeight, int widthPx);

// Renders page `page` (0-based) at `widthPx` pixels across its MediaBox, the height in
// proportion (RenderHeight), onto white: `out` is opaque, `out.id` and `out.version` are
// left as they were. The base transform is Matrix(s, 0, 0, -s, -x0 * s, y1 * s), s = widthPx /
// MediaBox width. False, with `out` unchanged, for a page that does not exist or a size out of
// range, or when the environment's cancel flag was set (`stats->cancelled`); a page whose
// content cannot be read is white (true, "contents" or "filter:..." in `stats`). `stats` may be
// nullptr; it is cleared first. Glyphs whose em box (grown to [-2, 3] em and by 32 px) lies
// outside the clip are not drawn, so a page of text far off the page costs no rasterization;
// nor are images wholly outside it decoded.
bool RenderPage(const PdfFile& file, int page, int widthPx, gfx::Image& out, RenderStats* stats,
                const RenderEnvironment& environment);
// The same with DefaultRenderEnvironment().
bool RenderPage(const PdfFile& file, int page, int widthPx, gfx::Image& out, RenderStats* stats);

// A band of a page of any height up to kMaxPageRows: rows [top, top + rows) of the image
// RenderPage would give at `widthPx` (`out` is widthPx x rows), drawn with the same base
// transform into a surface whose first row is device row `top` (gfx::Canvas::SetTarget). The
// pixels are RenderPage's, but in the rows next to the band's edges, where a path crossing one
// is cut at it, by up to 1 per channel. The content is interpreted whole for every band, and
// what lies outside the band is culled as outside any clip; the band spends the page's limits
// (RenderLimits), its own pixels in place of the page's. False, with `out` unchanged, when `top`
// and `rows` do not lie within the page's height, rows > kMaxRenderSide or widthPx * rows >
// kMaxRenderPixels; otherwise as RenderPage.
bool RenderPageRows(const PdfFile& file, int page, int widthPx, int top, int rows, gfx::Image& out,
                    RenderStats* stats, const RenderEnvironment& environment);

} // namespace rgui::pdf
