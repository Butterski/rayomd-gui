// SPDX-License-Identifier: Apache-2.0
// The exact-area scanline rasterizer, in the manner of FreeType's smooth rasterizer.
#pragma once

#include "gfx/geometry.h"
#include "gfx/path.h"

#include <cstdint>
#include <vector>

namespace rgui::gfx {

struct WorkBudget;

enum class FillRule : uint8_t { NonZero, EvenOdd };

// Receives one row of coverage: alpha[0..width) for pixels x0 .. x0 + width - 1 of row y.
class SpanSink {
public:
    virtual void Span(int y, int x0, int width, const uint8_t* alpha) = 0;
    // Rows y .. y + count - 1 with the same coverage (rectangles' rows): each to Span unless the
    // sink draws them at once.
    virtual void Rows(int y, int count, int x0, int width, const uint8_t* alpha) {
        for (int k = 0; k < count; ++k) Span(y + k, x0, width, alpha);
    }
    // True for a sink whose pixels depend on their own coverage and position alone, not on where
    // their span starts, and for which zero coverage changes nothing: Sweep may then hand a row
    // over in pieces, leaving out the runs of zero coverage between them.
    virtual bool Splittable() const { return false; }

protected:
    ~SpanSink() = default;
};

// Coordinates are 24.8 fixed point (PIXEL_BITS 8), rounded with ExactMath::LlRound(x * 256).
// A segment is clipped to the clip rectangle: the part left of clip.x0 becomes a vertical
// segment at clip.x0 that keeps its cover (area 0), the part right of clip.x1 and rows outside
// [clip.y0, clip.y1) are dropped. Work goes in bands of 16 rows, each cell an interleaved
// int32 {cover, area}; Sweep accumulates each row with the accumulate kernel and hands the
// sink every row that is not all transparent. Each band row also notes which runs of 64 cells
// any segment touched: where the row is long, a Splittable sink gets its touched runs and the
// untouched runs of nonzero coverage between them, and the untouched runs of zero coverage (the
// inside of a stroked box, the gaps between a row's glyph outlines) are neither accumulated nor
// handed over. An outline of vertical segments only (rectangles) is swept apart: a row whose
// segments, and the row above's, cover both rows whole repeats the row above, so it is neither
// rendered nor accumulated, and such rows go to the sink together (SpanSink::Rows). AddPath
// flattens a curve that lies wholly beyond one edge of the clip as its chord (FlattenCull),
// which covers the clip the same.
//
// With a WorkBudget, every segment AddLine takes costs one unit of geometry and every pixel
// Sweep accumulates one of pixels; once the budget is stopped (spent or cancelled, the flag
// polled every band and every 4096 segments) AddLine takes nothing and Sweep ends.
class Rasterizer {
public:
    Rasterizer();
    ~Rasterizer();
    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;

    // Forgets every segment and sets the clip; an empty clip draws nothing.
    void Reset(RectI clip);
    void AddPath(const Path& path, const Matrix& m, float tolerance = kFlattenTolerance);
    // One device-space segment; a closed outline's segments must sum to zero cover on each row.
    void AddLine(PointF from, PointF to);
    void Sweep(FillRule rule, SpanSink& sink);

    // The budget later calls draw on (nullptr: unlimited); it must outlive them.
    void SetBudget(WorkBudget* budget);
    // Frees buffers grown beyond about `bytes` (after an unusually large path).
    void Trim(size_t bytes);

    // The device rows and columns any segment touched, within the clip (empty if none).
    RectI Bounds() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace rgui::gfx
