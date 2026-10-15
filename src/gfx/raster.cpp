// SPDX-License-Identifier: Apache-2.0
// The exact-area scanline rasterizer (plan Phase 4 step 3), written from the algorithm of
// FreeType's smooth rasterizer: every cell a segment crosses gets the signed height it covers
// (cover) and that height times twice the mean x of its part within the cell (area); a row's
// coverage at a pixel is then (sum of the covers to its left) * 512 - its own area, over 512.
//
// Coordinates are 24.8 fixed point relative to the clip's corner. A segment is clipped in double
// precision before it is rounded, so any finite input stays in range: rows outside the clip are
// dropped, the part left of the clip becomes a vertical segment on its left edge (keeping its
// cover, area 0), the part right of it is dropped. Segments are bucketed by band of 16 rows;
// each band is rendered into a (clip width x 16) buffer of {cover, area} cells, which the
// accumulate kernel reads and clears row by row.
#include "gfx/raster.h"

#include "common/exact_math.h"
#include "gfx/kernels.h"

#include <algorithm>
#include <cstring>

namespace rgui::gfx {

namespace {

constexpr int kPixelBits = 8;
constexpr int32_t kOne = 1 << kPixelBits;
constexpr int kBandRows = 16;
constexpr int kBandShift = kPixelBits + 4;  // subpixel y to band
constexpr int kMaxClipSize = 1 << 20;       // pixels per side; keeps 24.8 products in int64

struct Segment {
    int32_t x0, y0, x1, y1;  // y0 < y1
    int32_t dir;             // +1 when the original ran downwards, else -1
};

struct RowStat {
    int32_t minCell;
    int32_t maxCell;
    int64_t coverSum;
};

// floor(a / b) for b > 0.
inline int64_t FloorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

inline int32_t WrapAdd(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

} // namespace

struct Rasterizer::Impl {
    RectI clip{};
    int width = 0;
    int height = 0;
    std::vector<Segment> segments;
    int32_t minX = 0, minY = 0, maxX = -1, maxY = -1;  // subpixels; empty while maxX < minX
    bool rightDropped = false;

    std::vector<int32_t> cells;   // kBandRows rows of `width` {cover, area}; all zero between bands
    std::vector<uint8_t> alpha;
    std::vector<uint32_t> order;  // segment indices sorted by first band
    std::vector<uint32_t> bandStart;
    std::vector<uint32_t> active;
    RowStat rows[kBandRows];

    void Push(double x0, double y0, double x1, double y1);
    void ClipAndPush(double x0, double y0, double x1, double y1);
    void Cell(int32_t* row, RowStat& st, int32_t c, int32_t cover, int32_t area) const;
    void RowPiece(int32_t* row, RowStat& st, int32_t sx, int32_t sy, int32_t ex, int32_t ey, int32_t dir) const;
    void RenderBand(const Segment& s, int bandTop);
};

inline void Rasterizer::Impl::Cell(int32_t* row, RowStat& st, int32_t c, int32_t cover, int32_t area) const {
    int32_t* cell = row + 2 * static_cast<ptrdiff_t>(c);
    cell[0] = WrapAdd(cell[0], cover);
    cell[1] = WrapAdd(cell[1], area);
    st.minCell = std::min(st.minCell, c);
    st.maxCell = std::max(st.maxCell, c);
    st.coverSum += cover;
}

// The part of a segment within one row, from (sx, sy) to (ex, ey) with sy < ey (only their
// difference matters), split at every vertical cell boundary.
void Rasterizer::Impl::RowPiece(int32_t* row, RowStat& st, int32_t sx, int32_t sy, int32_t ex, int32_t ey,
                                int32_t dir) const {
    const int32_t dy = ey - sy;
    if (sx == ex) {
        const int32_t c = sx >> kPixelBits;
        if (c >= width) return;  // on the clip's right edge: covers nothing inside
        const int32_t fx = sx - (c << kPixelBits);
        Cell(row, st, c, dir * dy, dir * dy * 2 * fx);
        return;
    }
    int32_t lx, rx;
    int64_t yl, yr;
    if (sx < ex) {
        lx = sx, rx = ex, yl = sy, yr = ey;
    } else {
        lx = ex, rx = sx, yl = ey, yr = sy;
    }
    const int32_t ca = lx >> kPixelBits;
    const int32_t cb = (rx - 1) >> kPixelBits;
    if (ca == cb) {
        Cell(row, st, ca, dir * dy, dir * dy * (lx + rx - 2 * (ca << kPixelBits)));
        return;
    }
    const int64_t dxt = rx - lx;
    const int64_t dyt = yr - yl;
    int64_t yPrev = yl;
    int32_t xPrev = lx;
    for (int32_t c = ca; c <= cb && c < width; ++c) {
        const int32_t xNext = c == cb ? rx : (c + 1) << kPixelBits;
        const int64_t yNext = c == cb ? yr : yl + FloorDiv(dyt * (xNext - lx), dxt);
        const int32_t dyp = static_cast<int32_t>(yNext > yPrev ? yNext - yPrev : yPrev - yNext);
        const int32_t base = c << kPixelBits;
        Cell(row, st, c, dir * dyp, dir * dyp * ((xPrev - base) + (xNext - base)));
        yPrev = yNext;
        xPrev = xNext;
    }
}

void Rasterizer::Impl::RenderBand(const Segment& s, int bandTop) {
    const int32_t top = bandTop << kPixelBits;
    const int32_t bottom = (bandTop + kBandRows) << kPixelBits;
    const int32_t ya = std::max(s.y0, top);
    const int32_t yb = std::min(s.y1, bottom);
    if (ya >= yb) return;
    const int64_t dxs = static_cast<int64_t>(s.x1) - s.x0;
    const int64_t dys = static_cast<int64_t>(s.y1) - s.y0;
    auto xAt = [&](int32_t y) -> int32_t {
        if (y == s.y0) return s.x0;
        if (y == s.y1) return s.x1;
        return static_cast<int32_t>(s.x0 + FloorDiv(dxs * (y - s.y0), dys));
    };
    const int32_t r0 = ya >> kPixelBits;
    const int32_t r1 = (yb - 1) >> kPixelBits;
    int32_t yPrev = ya;
    int32_t xPrev = xAt(ya);
    // x at the row boundaries (r + 1) * 256 below the first row, floor(dxs * (y - y0) / dys), by
    // an exact DDA: quotient and remainder stepped by those of dxs * 256 / dys.
    int64_t q = 0, rem = 0, stepQ = 0, stepR = 0;
    if (r0 < r1 && dxs != 0) {
        const int64_t num = dxs * ((static_cast<int64_t>(r0) + 1) * kOne - s.y0);
        q = FloorDiv(num, dys);
        rem = num - q * dys;
        stepQ = FloorDiv(dxs * kOne, dys);
        stepR = dxs * kOne - stepQ * dys;
    }
    for (int32_t r = r0; r <= r1; ++r) {
        int32_t yNext, xNext;
        if (r == r1) {
            yNext = yb;
            xNext = xAt(yb);
        } else {
            yNext = (r + 1) << kPixelBits;
            xNext = static_cast<int32_t>(s.x0 + q);
            q += stepQ;
            rem += stepR;
            if (rem >= dys) {
                rem -= dys;
                ++q;
            }
        }
        const int local = r - bandTop;
        int32_t* row = cells.data() + 2 * static_cast<ptrdiff_t>(local) * width;
        RowPiece(row, rows[local], xPrev, yPrev, xNext, yNext, s.dir);
        yPrev = yNext;
        xPrev = xNext;
    }
}

// A segment in device pixels, already within the clip's rows and columns, to 24.8.
void Rasterizer::Impl::Push(double x0, double y0, double x1, double y1) {
    const double cx = clip.x0, cy = clip.y0;
    const int64_t maxXs = static_cast<int64_t>(width) << kPixelBits;
    const int64_t maxYs = static_cast<int64_t>(height) << kPixelBits;
    auto fx = [&](double x) {
        return static_cast<int32_t>(std::clamp<int64_t>(RayoMd::ExactMath::LlRound((x - cx) * kOne), 0, maxXs));
    };
    auto fy = [&](double y) {
        return static_cast<int32_t>(std::clamp<int64_t>(RayoMd::ExactMath::LlRound((y - cy) * kOne), 0, maxYs));
    };
    Segment s{fx(x0), fy(y0), fx(x1), fy(y1), 1};
    if (s.y0 == s.y1) return;
    if (s.y0 > s.y1) {
        std::swap(s.x0, s.x1);
        std::swap(s.y0, s.y1);
        s.dir = -1;
    }
    segments.push_back(s);
    if (maxX < minX) {
        minX = std::min(s.x0, s.x1);
        maxX = std::max(s.x0, s.x1);
        minY = s.y0;
        maxY = s.y1;
    } else {
        minX = std::min(minX, std::min(s.x0, s.x1));
        maxX = std::max(maxX, std::max(s.x0, s.x1));
        minY = std::min(minY, s.y0);
        maxY = std::max(maxY, s.y1);
    }
}

void Rasterizer::Impl::ClipAndPush(double x0, double y0, double x1, double y1) {
    const double cx0 = clip.x0, cy0 = clip.y0, cx1 = clip.x1, cy1 = clip.y1;
    // Rows: only the part within [cy0, cy1] covers anything.
    if (y0 == y1) return;
    if ((y0 <= cy0 && y1 <= cy0) || (y0 >= cy1 && y1 >= cy1)) return;
    auto xAtY = [&](double y) { return x0 + (x1 - x0) * ((y - y0) / (y1 - y0)); };
    double ax = x0, ay = y0, bx = x1, by = y1;
    if (ay < cy0) ax = xAtY(cy0), ay = cy0;
    else if (ay > cy1) ax = xAtY(cy1), ay = cy1;
    if (by < cy0) bx = xAtY(cy0), by = cy0;
    else if (by > cy1) bx = xAtY(cy1), by = cy1;
    // Columns.
    if (ax >= cx1 && bx >= cx1) {
        rightDropped = true;
        return;
    }
    if (ax <= cx0 && bx <= cx0) {
        Push(cx0, ay, cx0, by);
        return;
    }
    auto yAtX = [&](double x) {
        const double y = ay + (by - ay) * ((x - ax) / (bx - ax));
        const double lo = ay < by ? ay : by, hi = ay < by ? by : ay;
        return y < lo ? lo : (y > hi ? hi : y);
    };
    // Cut at the left edge: the outside part stands on it.
    if (ax < cx0 || bx < cx0) {
        const double ys = yAtX(cx0);
        if (ax < cx0) {
            Push(cx0, ay, cx0, ys);
            ax = cx0, ay = ys;
        } else {
            Push(cx0, ys, cx0, by);
            bx = cx0, by = ys;
        }
    }
    // Cut at the right edge: the outside part is dropped.
    if (ax > cx1 || bx > cx1) {
        rightDropped = true;
        const double ys = yAtX(cx1);
        if (ax > cx1) {
            ax = cx1, ay = ys;
        } else {
            bx = cx1, by = ys;
        }
    }
    Push(ax, ay, bx, by);
}

Rasterizer::Rasterizer() : impl_(new Impl) {}

Rasterizer::~Rasterizer() {
    delete impl_;
}

void Rasterizer::Reset(RectI clip) {
    Impl& r = *impl_;
    if (clip.Width() > kMaxClipSize) clip.x1 = clip.x0 + kMaxClipSize;
    if (clip.Height() > kMaxClipSize) clip.y1 = clip.y0 + kMaxClipSize;
    if (clip.Empty()) clip = {0, 0, 0, 0};
    r.clip = clip;
    r.width = clip.Width();
    r.height = clip.Height();
    r.segments.clear();
    r.minX = r.minY = 0;
    r.maxX = r.maxY = -1;
    r.rightDropped = false;
}

void Rasterizer::AddLine(PointF from, PointF to) {
    Impl& r = *impl_;
    if (r.width <= 0) return;
    const double x0 = from.x, y0 = from.y, x1 = to.x, y1 = to.y;
    // NaN and infinities fail these comparisons; floats are finite below 3.5e38.
    if (!(x0 > -1e300 && x0 < 1e300 && y0 > -1e300 && y0 < 1e300 && x1 > -1e300 && x1 < 1e300 &&
          y1 > -1e300 && y1 < 1e300)) {
        return;
    }
    r.ClipAndPush(x0, y0, x1, y1);
}

namespace {

struct RasterLineSink final : LineSink {
    Rasterizer& raster;
    explicit RasterLineSink(Rasterizer& r) : raster(r) {}
    void Line(PointF from, PointF to) override { raster.AddLine(from, to); }
};

} // namespace

void Rasterizer::AddPath(const Path& path, const Matrix& m, float tolerance) {
    if (impl_->width <= 0) return;
    RasterLineSink sink(*this);
    Flatten(path, m, tolerance, sink);
}

RectI Rasterizer::Bounds() const {
    const Impl& r = *impl_;
    if (r.maxX < r.minX || r.maxY <= r.minY) return {};
    RectI b{r.clip.x0 + (r.minX >> kPixelBits), r.clip.y0 + (r.minY >> kPixelBits),
            r.clip.x0 + ((r.maxX + kOne - 1) >> kPixelBits), r.clip.y0 + ((r.maxY + kOne - 1) >> kPixelBits)};
    if (r.rightDropped) b.x1 = r.clip.x1;
    if (b.x1 <= b.x0) b.x1 = std::min(b.x0 + 1, r.clip.x1);  // a vertical edge covers the pixel right of it
    return b.Intersect(r.clip);
}

void Rasterizer::Sweep(FillRule rule, SpanSink& sink) {
    Impl& r = *impl_;
    if (r.segments.empty() || r.width <= 0) return;
    const size_t w = static_cast<size_t>(r.width);
    if (r.cells.size() < w * 2 * kBandRows) r.cells.resize(w * 2 * kBandRows, 0);
    if (r.alpha.size() < w) r.alpha.resize(w);

    // Bucket the segments by their first band (counting sort).
    const int bandCount = (r.height + kBandRows - 1) / kBandRows;
    r.bandStart.assign(static_cast<size_t>(bandCount) + 1, 0);
    for (const Segment& s : r.segments) r.bandStart[static_cast<size_t>(s.y0 >> kBandShift) + 1]++;
    for (int b = 0; b < bandCount; ++b) r.bandStart[b + 1] += r.bandStart[b];
    r.order.resize(r.segments.size());
    {
        // bandStart[b] is consumed as an insertion cursor, then rebuilt below.
        std::vector<uint32_t>& cursor = r.bandStart;
        for (uint32_t i = 0; i < r.segments.size(); ++i) {
            r.order[cursor[static_cast<size_t>(r.segments[i].y0 >> kBandShift)]++] = i;
        }
        // cursor[b] now holds the end of band b, which is where band b + 1 starts.
        for (int b = bandCount; b > 0; --b) cursor[b] = cursor[b - 1];
        cursor[0] = 0;
    }

    const uint32_t evenOdd = rule == FillRule::EvenOdd ? 1u : 0u;
    const auto accumulateRow = ActiveKernels().accumulateRow;
    r.active.clear();
    const int firstBand = r.minY >> kBandShift;
    const int lastBand = (r.maxY - 1) >> kBandShift;
    for (int band = firstBand; band <= lastBand; ++band) {
        for (uint32_t i = r.bandStart[band]; i < r.bandStart[band + 1]; ++i) r.active.push_back(r.order[i]);
        if (r.active.empty()) continue;
        const int bandTop = band * kBandRows;
        const int rowsInBand = std::min(kBandRows, r.height - bandTop);
        for (int i = 0; i < rowsInBand; ++i) r.rows[i] = {INT32_MAX, -1, 0};
        for (size_t i = 0; i < r.active.size();) {
            const Segment& s = r.segments[r.active[i]];
            r.RenderBand(s, bandTop);
            if (((s.y1 - 1) >> kBandShift) <= band) {
                r.active[i] = r.active.back();
                r.active.pop_back();
            } else {
                ++i;
            }
        }
        for (int i = 0; i < rowsInBand; ++i) {
            const RowStat& st = r.rows[i];
            if (st.maxCell < st.minCell) continue;
            // Past the last touched cell the coverage stays at the row's total cover: zero when
            // the outline closes within the clip, so the row ends there.
            const int32_t end = st.coverSum == 0 ? st.maxCell + 1 : r.width;
            int32_t* row = r.cells.data() + 2 * static_cast<ptrdiff_t>(i) * r.width;
            const size_t n = static_cast<size_t>(end - st.minCell);
            if (accumulateRow(row + 2 * static_cast<ptrdiff_t>(st.minCell), r.alpha.data(), n, evenOdd) != 0) {
                sink.Span(r.clip.y0 + bandTop + i, r.clip.x0 + st.minCell, static_cast<int>(n), r.alpha.data());
            }
        }
    }
}

} // namespace rgui::gfx
