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
#include "gfx/budget.h"
#include "gfx/kernels.h"

#include <algorithm>
#include <cstring>

#include <emmintrin.h>

namespace rgui::gfx {

namespace {

constexpr int kPixelBits = 8;
constexpr int32_t kOne = 1 << kPixelBits;
constexpr int kBandRows = 16;
constexpr int kBandShift = kPixelBits + 4;  // subpixel y to band
constexpr int kMaxClipSize = 1 << 20;       // pixels per side; keeps 24.8 products in int64
constexpr int kChunkShift = 6;              // cells per touched-run bit: 64
constexpr int kWordShift = kChunkShift + 6; // cells per word of bits: 4096
// A row is handed over in pieces (SpanSink::Splittable) only when at least this long.
constexpr int32_t kSplitCells = 256;

struct Segment {
    int32_t x0, y0, x1, y1;  // y0 < y1
    int32_t dir;             // +1 when the original ran downwards, else -1
};

struct RowStat {
    int32_t minCell;
    int32_t maxCell;
    int64_t coverSum;
    uint64_t* touched;       // bit k of word w: some cell in [64 (64 w + k), + 64) was touched
};

// accumulate_row's alpha of a cell after `acc`, the sum of the covers to its left and its own,
// when the cell's area is 0 (kernels.h).
inline uint8_t AlphaAfter(int32_t acc, uint32_t evenOdd) {
    const int32_t a = static_cast<int32_t>(static_cast<uint32_t>(acc) << 9) >> 9;
    int32_t m = static_cast<int32_t>(static_cast<uint32_t>(a ^ (a >> 31)) - static_cast<uint32_t>(a >> 31));
    if (evenOdd) {
        m &= 511;
        if (m > 256) m = 512 - m;
    }
    return static_cast<uint8_t>(m > 255 ? 255 : m);
}

// The wrapping sum of the covers of cells[0..n) (SSE2).
inline int32_t CoverSum(const int32_t* cells, size_t n) {
    const __m128i even = _mm_set_epi32(0, -1, 0, -1);
    __m128i sum = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        sum = _mm_add_epi32(sum, _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(cells + 2 * i)), even));
    }
    uint32_t total = static_cast<uint32_t>(_mm_cvtsi128_si32(sum)) +
                     static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_srli_si128(sum, 8)));
    if (i < n) total += static_cast<uint32_t>(cells[2 * i]);
    return static_cast<int32_t>(total);
}

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
    bool upright = true;  // every segment taken is vertical (x0 == x1): an outline of rectangles
    WorkBudget* budget = nullptr;
    uint32_t added = 0;  // segments taken since the start, to poll the cancel flag now and then

    std::vector<int32_t> cells;   // kBandRows rows of `width` {cover, area}; all zero between bands
    std::vector<uint64_t> touched; // kBandRows rows of `words` bits (RowStat::touched); zero between bands
    int words = 0;
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
    void SweepUpright(uint32_t evenOdd, SpanSink& sink);
};

inline void Rasterizer::Impl::Cell(int32_t* row, RowStat& st, int32_t c, int32_t cover, int32_t area) const {
    int32_t* cell = row + 2 * static_cast<ptrdiff_t>(c);
    cell[0] = WrapAdd(cell[0], cover);
    cell[1] = WrapAdd(cell[1], area);
    st.minCell = std::min(st.minCell, c);
    st.maxCell = std::max(st.maxCell, c);
    st.coverSum += cover;
    st.touched[c >> kWordShift] |= uint64_t(1) << ((c >> kChunkShift) & 63);
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
    upright = upright && s.x0 == s.x1;
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
    r.upright = true;
}

void Rasterizer::AddLine(PointF from, PointF to) {
    Impl& r = *impl_;
    if (r.width <= 0) return;
    if (r.budget) {
        if (!r.budget->Take(r.budget->geometry, 1, WorkBudget::Stop::Geometry)) return;
        if ((++r.added & 4095) == 0 && !r.budget->Poll()) return;
    }
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
    Impl& r = *impl_;
    if (r.width <= 0) return;
    RasterLineSink sink(*this);
    FlattenCull cull;
    cull.x0 = r.clip.x0 - 1.0;
    cull.y0 = r.clip.y0 - 1.0;
    cull.x1 = r.clip.x1 + 1.0;
    cull.y1 = r.clip.y1 + 1.0;
    Flatten(path, m, tolerance, sink, &cull);
}

// Sweep for outlines of vertical segments only (rectangles: fills, stroked boxes and lines,
// images' squares). A segment crosses a row at one cell, so a row's cells follow from the
// segments that touch it; a row that every segment touching it or the row above covers whole,
// in both rows, has the cells of the row above, so its coverage is that row's: it is neither
// rendered nor accumulated, and such runs of rows go to the sink at once (SpanSink::Rows). The
// bands, their budget and the cells of the other rows are those of the general sweep.
void Rasterizer::Impl::SweepUpright(uint32_t evenOdd, SpanSink& sink) {
    const auto accumulateRow = ActiveKernels().accumulateRow;
    const size_t w = static_cast<size_t>(width);
    const size_t wordsPerRow = static_cast<size_t>(words);
    uint8_t* const out = alpha.data();
    // The rows sharing the coverage in `out`: from groupY, groupCount of them, the span groupX0,
    // groupN; handed over when the next row differs.
    int groupY = 0, groupCount = 0;
    int32_t groupX0 = 0, groupN = 0;
    const auto flush = [&] {
        if (groupCount > 0) sink.Rows(clip.y0 + groupY, groupCount, clip.x0 + groupX0, groupN, out);
        groupCount = 0;
    };
    bool repeat[kBandRows];
    active.clear();
    const int firstBand = minY >> kBandShift;
    const int lastBand = (maxY - 1) >> kBandShift;
    for (int band = firstBand; band <= lastBand; ++band) {
        for (uint32_t i = bandStart[band]; i < bandStart[band + 1]; ++i) active.push_back(order[i]);
        if (active.empty()) continue;
        if (budget && !budget->Poll()) {
            flush();
            return;
        }
        const int bandTop = band * kBandRows;
        const int rowsInBand = std::min(kBandRows, height - bandTop);
        for (int i = 0; i < rowsInBand; ++i) {
            RowStat& st = rows[i];
            st = {INT32_MAX, -1, 0, touched.data() + static_cast<size_t>(i) * wordsPerRow};
            const int32_t top = (bandTop + i) << kPixelBits, bottom = top + kOne;
            // The first row of a band is rendered: segments that ended in the band above are gone.
            bool same = i > 0;
            for (size_t k = 0; k < active.size() && same; ++k) {
                const Segment& s = segments[active[k]];
                if (s.y1 <= top || s.y0 >= bottom) {
                    same = !(s.y0 < top && s.y1 > top - kOne);  // touched the row above only
                } else {
                    same = s.y0 <= top - kOne && s.y1 >= bottom;  // covers both rows whole
                }
            }
            repeat[i] = same;
            if (same) {
                st.minCell = rows[i - 1].minCell;
                st.maxCell = rows[i - 1].maxCell;
                st.coverSum = rows[i - 1].coverSum;
                continue;
            }
            int32_t* row = cells.data() + 2 * static_cast<ptrdiff_t>(i) * width;
            for (const uint32_t index : active) {
                const Segment& s = segments[index];
                if (s.y1 <= top || s.y0 >= bottom) continue;
                // RowPiece's vertical case.
                const int32_t dy = std::min(s.y1, bottom) - std::max(s.y0, top);
                const int32_t c = s.x0 >> kPixelBits;
                if (c >= width) continue;
                const int32_t fx = s.x0 - (c << kPixelBits);
                Cell(row, st, c, s.dir * dy, s.dir * dy * 2 * fx);
            }
        }
        for (size_t i = 0; i < active.size();) {
            if (((segments[active[i]].y1 - 1) >> kBandShift) <= band) {
                active[i] = active.back();
                active.pop_back();
            } else {
                ++i;
            }
        }
        if (budget) {
            uint64_t pixels = 0;
            for (int i = 0; i < rowsInBand; ++i) {
                const RowStat& st = rows[i];
                if (st.maxCell >= st.minCell) pixels += static_cast<uint64_t>((st.coverSum == 0 ? st.maxCell + 1 : width) - st.minCell);
            }
            if (!budget->Take(budget->pixels, pixels, WorkBudget::Stop::Pixels)) {
                flush();
                std::fill_n(cells.data(), 2 * static_cast<size_t>(rowsInBand) * w, 0);
                std::fill_n(touched.data(), static_cast<size_t>(rowsInBand) * wordsPerRow, 0);
                return;
            }
        }
        for (int i = 0; i < rowsInBand; ++i) {
            const RowStat& st = rows[i];
            if (repeat[i]) {
                if (groupCount > 0) groupCount++;
                continue;
            }
            flush();
            if (st.maxCell < st.minCell) continue;
            const int32_t end = st.coverSum == 0 ? st.maxCell + 1 : width;
            int32_t* row = cells.data() + 2 * static_cast<ptrdiff_t>(i) * width;
            if (accumulateRow(row + 2 * static_cast<ptrdiff_t>(st.minCell), out, static_cast<size_t>(end - st.minCell),
                              evenOdd) != 0) {
                groupY = bandTop + i;
                groupCount = 1;
                groupX0 = st.minCell;
                groupN = end - st.minCell;
            }
            std::fill(st.touched + (st.minCell >> kWordShift), st.touched + (st.maxCell >> kWordShift) + 1, 0);
        }
    }
    flush();
}

void Rasterizer::SetBudget(WorkBudget* budget) {
    impl_->budget = budget;
}

template <typename T>
static void TrimVector(std::vector<T>& v, size_t bytes) {
    if (v.capacity() * sizeof(T) > bytes) std::vector<T>().swap(v);
}

void Rasterizer::Trim(size_t bytes) {
    Impl& r = *impl_;
    TrimVector(r.segments, bytes);
    TrimVector(r.order, bytes);
    TrimVector(r.bandStart, bytes);
    TrimVector(r.active, bytes);
    TrimVector(r.cells, bytes);    // all zero between sweeps: dropping it keeps that
    TrimVector(r.touched, bytes);  // likewise
    TrimVector(r.alpha, bytes);
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
    r.words = (r.width + (1 << kWordShift) - 1) >> kWordShift;
    const size_t words = static_cast<size_t>(r.words);
    if (r.touched.size() < words * kBandRows) r.touched.resize(words * kBandRows, 0);
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
    if (r.upright) {
        r.SweepUpright(evenOdd, sink);
        return;
    }
    const auto accumulateRow = ActiveKernels().accumulateRow;
    const bool split = sink.Splittable();
    uint8_t* const alpha = r.alpha.data();
    r.active.clear();
    const int firstBand = r.minY >> kBandShift;
    const int lastBand = (r.maxY - 1) >> kBandShift;
    for (int band = firstBand; band <= lastBand; ++band) {
        for (uint32_t i = r.bandStart[band]; i < r.bandStart[band + 1]; ++i) r.active.push_back(r.order[i]);
        if (r.active.empty()) continue;
        if (r.budget && !r.budget->Poll()) return;
        const int bandTop = band * kBandRows;
        const int rowsInBand = std::min(kBandRows, r.height - bandTop);
        for (int i = 0; i < rowsInBand; ++i) r.rows[i] = {INT32_MAX, -1, 0, r.touched.data() + static_cast<size_t>(i) * words};
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
        if (r.budget) {
            // The band's pixels are paid before any row is accumulated; when they cannot be, the
            // band's cells are cleared for the next sweep and the sweep ends.
            uint64_t pixels = 0;
            for (int i = 0; i < rowsInBand; ++i) {
                const RowStat& st = r.rows[i];
                if (st.maxCell >= st.minCell) pixels += static_cast<uint64_t>((st.coverSum == 0 ? st.maxCell + 1 : r.width) - st.minCell);
            }
            if (!r.budget->Take(r.budget->pixels, pixels, WorkBudget::Stop::Pixels)) {
                std::fill_n(r.cells.data(), 2 * static_cast<size_t>(rowsInBand) * w, 0);
                std::fill_n(r.touched.data(), static_cast<size_t>(rowsInBand) * words, 0);
                return;
            }
        }
        for (int i = 0; i < rowsInBand; ++i) {
            const RowStat& st = r.rows[i];
            if (st.maxCell < st.minCell) continue;
            // Past the last touched cell the coverage stays at the row's total cover: zero when
            // the outline closes within the clip, so the row ends there.
            const int32_t end = st.coverSum == 0 ? st.maxCell + 1 : r.width;
            int32_t* row = r.cells.data() + 2 * static_cast<ptrdiff_t>(i) * r.width;
            const int y = r.clip.y0 + bandTop + i;
            uint64_t* bits = st.touched;
            if (!split || end - st.minCell < kSplitCells) {
                const size_t n = static_cast<size_t>(end - st.minCell);
                if (accumulateRow(row + 2 * static_cast<ptrdiff_t>(st.minCell), alpha, n, evenOdd) != 0) {
                    sink.Span(y, r.clip.x0 + st.minCell, static_cast<int>(n), alpha);
                }
            } else {
                // Runs of 64-cell chunks, touched or not. A touched run is accumulated from the
                // cover summed so far (added to its first cell); an untouched run's cells are all
                // zero, so its coverage is that of the sum alone: a run of zeros ends a piece.
                const auto isTouched = [bits](int32_t c) {
                    return ((bits[c >> kWordShift] >> ((c >> kChunkShift) & 63)) & 1) != 0;
                };
                int32_t acc = 0;
                int32_t start = -1;
                uint32_t any = 0;
                const auto flush = [&](int32_t at) {
                    if (start >= 0 && any != 0) sink.Span(y, r.clip.x0 + start, at - start, alpha + start);
                    start = -1;
                    any = 0;
                };
                for (int32_t x = st.minCell; x < end;) {
                    const bool touched = isTouched(x);
                    int32_t next = std::min(end, ((x >> kChunkShift) + 1) << kChunkShift);
                    while (next < end && isTouched(next) == touched) next = std::min(end, next + (1 << kChunkShift));
                    const size_t n = static_cast<size_t>(next - x);
                    if (touched) {
                        int32_t* cells = row + 2 * static_cast<ptrdiff_t>(x);
                        const int32_t sum = CoverSum(cells, n);
                        cells[0] = WrapAdd(cells[0], acc);
                        any |= accumulateRow(cells, alpha + x, n, evenOdd);
                        acc = WrapAdd(acc, sum);
                        if (start < 0) start = x;
                    } else if (const uint8_t v = AlphaAfter(acc, evenOdd)) {
                        std::memset(alpha + x, v, n);
                        any |= v;
                        if (start < 0) start = x;
                    } else {
                        flush(x);
                    }
                    x = next;
                }
                flush(end);
            }
            std::fill(bits + (st.minCell >> kWordShift), bits + (st.maxCell >> kWordShift) + 1, 0);
        }
    }
}

} // namespace rgui::gfx
