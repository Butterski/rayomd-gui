// SPDX-License-Identifier: Apache-2.0
// The canvas (plan Phase 4 step 5): fills, gradients, strokes, images and masks under a stack of
// rectangle and path clips. Every span is limited to the clip rectangle, which a path clip
// shrinks to its mask's bounds, and its coverage is multiplied by the mask row where there is one.
#include "gfx/canvas.h"

#include "common/exact_math.h"
#include "gfx/budget.h"
#include "gfx/kernels.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <emmintrin.h>

namespace rgui::gfx {

namespace {

struct ClipState {
    RectI rect;
    int mask;  // index into Impl::masks, -1 for none
};

struct MaskBuffer {
    RectI bounds;
    std::vector<uint8_t> alpha;  // bounds.Width() per row
};

// The columns of an upright bilinear span (bilinear_row's xs and ws): every row of an image drawn
// upright has the same ones, kept while the span's start, step, width and image width stay.
struct ColumnTable {
    int64_t sx = 0;
    int64_t dx = 0;
    int64_t imageWidth = 0;
    int width = -1;
    std::vector<int32_t> xs;
    std::vector<uint32_t> ws;
};

// The gradient's colour at t in [0, 1], interpolated in RGB, then premultiplied.
uint32_t GradientColor(const LinearGradient& g, double t) {
    const std::vector<GradientStop>& s = g.stops;
    const GradientStop* a = &s.front();
    const GradientStop* b = &s.front();
    if (t <= s.front().offset) {
        b = a;
    } else if (t >= s.back().offset) {
        a = b = &s.back();
    } else {
        for (size_t i = 0; i + 1 < s.size(); ++i) {
            if (t < s[i + 1].offset) {
                a = &s[i];
                b = &s[i + 1];
                break;
            }
        }
    }
    const double span = static_cast<double>(b->offset) - a->offset;
    const double f = span > 0 ? (t - a->offset) / span : 1.0;
    auto mix = [f](uint8_t x, uint8_t y) {
        const double v = x + (static_cast<double>(y) - x) * f;
        const int i = static_cast<int>(v + 0.5);
        return static_cast<uint8_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    return Premultiply(mix(a->r, b->r), mix(a->g, b->g), mix(a->b, b->b), g.alpha);
}

// SSE2, the x86-64 baseline, 16 coverage bytes at a time.

// The first index in [i, n) whose byte is 0, or 255 when `full` counts too; n when none.
inline int NextStop(const uint8_t* c, int i, int n, bool full) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i other = full ? _mm_set1_epi8(-1) : zero;
    for (; i + 16 <= n; i += 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(c + i));
        const int hits = _mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v, zero), _mm_cmpeq_epi8(v, other)));
        if (hits != 0) return i + __builtin_ctz(static_cast<unsigned>(hits));
    }
    for (; i < n; ++i) {
        if (c[i] == 0 || (full && c[i] == 255)) return i;
    }
    return n;
}

// The first index in [i, n) whose byte is not v; n when none.
inline int RunEnd(const uint8_t* c, int i, int n, uint8_t v) {
    const __m128i want = _mm_set1_epi8(static_cast<char>(v));
    for (; i + 16 <= n; i += 16) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(c + i));
        const int misses = _mm_movemask_epi8(_mm_cmpeq_epi8(bytes, want)) ^ 0xFFFF;
        if (misses != 0) return i + __builtin_ctz(static_cast<unsigned>(misses));
    }
    while (i < n && c[i] == v) ++i;
    return i;
}

// Mul255 of 16 byte pairs: t = a b + 128 is at most 65,153 and t + (t >> 8) at most 65,407,
// both exact in 16-bit lanes.
inline __m128i Mul255x16(__m128i a, __m128i b) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i half = _mm_set1_epi16(128);
    __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero)), half);
    __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero)), half);
    lo = _mm_srli_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), 8);
    hi = _mm_srli_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), 8);
    return _mm_packus_epi16(lo, hi);
}

// out[i] = Mul255(a[i], b[i]) for i in [0, n); out may be a.
inline void MulRow(const uint8_t* a, const uint8_t* b, uint8_t* out, int n) {
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), Mul255x16(x, y));
    }
    for (; i < n; ++i) out[i] = static_cast<uint8_t>(Mul255(a[i], b[i]));
}

// out[i] = Mul255(a[i], k) for i in [0, n).
inline void MulRowBy(const uint8_t* a, uint8_t k, uint8_t* out, int n) {
    const __m128i y = _mm_set1_epi8(static_cast<char>(k));
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), Mul255x16(x, y));
    }
    for (; i < n; ++i) out[i] = static_cast<uint8_t>(Mul255(a[i], k));
}

// p[0..n) = v.
inline void FillRun(uint32_t* p, size_t n, uint32_t v) {
    const __m128i four = _mm_set1_epi32(static_cast<int>(v));
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + i), four);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + i + 4), four);
    }
    for (; i < n; ++i) p[i] = v;
}

} // namespace

struct Canvas::Impl {
    Surface target{};
    int top = 0;                 // the device row of the target's first row
    std::vector<ClipState> clips{{RectI{}, -1}};
    std::vector<MaskBuffer> masks;
    size_t maskDepth = 0;
    MaskBuffer spare;            // IntersectClipPath's new mask, swapped with the one it replaces
    WorkBudget* budget = nullptr;
    Rasterizer raster;
    std::vector<uint8_t> cov;
    std::vector<uint32_t> src;
    std::vector<uint8_t> full;
    uint32_t lut[256] = {};
    Path outline;
    Path unitSquare;
    ColumnTable columns;
    struct Run {
        int at;
        int n;
        bool fill;  // stored, else blended
    };
    std::vector<Run> runs;  // SolidRows'
    const Kernels& k = ActiveKernels();

    Impl() {
        unitSquare.AddRect(0, 0, 1, 1);
    }

    const ClipState& Clip() const { return clips.back(); }

    bool Stopped() const { return budget && budget->Stopped(); }
    // Pays `pixels` written outside the rasterizer; false once the budget is stopped.
    bool Spend(uint64_t pixels) { return !budget || budget->Take(budget->pixels, pixels, WorkBudget::Stop::Pixels); }
    static uint64_t Area(const RectI& r) { return static_cast<uint64_t>(r.Width()) * static_cast<uint64_t>(r.Height()); }

    // Rasterizes `path` into `mask` over the part of `rect` it touches, times the mask `old`
    // (index into masks, -1 for none) there; false (mask untouched) when it touches nothing.
    bool RasterMask(const Path& path, const Matrix& m, FillRule rule, RectI rect, int old, MaskBuffer& mask);

    void Reserve(size_t n) {
        if (cov.size() < n) {
            cov.resize(n);
            src.resize(n);
            full.resize(n, 255);
        }
    }

    // The coverage of a span in the clip, times the clip mask's row there when there is one.
    const uint8_t* Masked(const uint8_t* alpha, int y, int x0, int w) {
        const ClipState& c = Clip();
        if (c.mask < 0) return alpha;
        const MaskBuffer& m = masks[static_cast<size_t>(c.mask)];
        const uint8_t* row = m.alpha.data() + static_cast<ptrdiff_t>(y - m.bounds.y0) * m.bounds.Width() +
                             (x0 - m.bounds.x0);
        uint8_t* out = cov.data();
        MulRow(alpha, row, out, w);
        return out;
    }

    uint32_t* Dst(int y, int x) const { return target.Row(y - top) + x; }

    // The runs of a row of `color` under coverage c[0..w): runs of 16 or more zeros are skipped
    // and, for an opaque colour, runs as long of full coverage are stored (fill(start, n)); the
    // rest is blended (blend(start, n)). Stores and skips give the bytes blend_solid gives there.
    template <typename Blend, typename Fill>
    static void SolidRuns(int w, const uint8_t* c, bool opaque, Blend&& blend, Fill&& fill) {
        constexpr int kMinRun = 16;
        int start = 0;
        int i = 0;
        for (;;) {
            i = NextStop(c, i, w, opaque);
            if (i >= w) break;
            const uint8_t v = c[i];
            const int end = RunEnd(c, i + 1, w, v);
            if (end - i >= kMinRun) {
                if (i > start) blend(start, i - start);
                if (v == 255) fill(i, end - i);
                start = end;
            }
            i = end;
        }
        if (w > start) blend(start, w - start);
    }

    // One row of `color` under coverage c[0..w).
    void SolidRow(int y, int x0, int w, const uint8_t* c, uint32_t color) const {
        uint32_t* dst = Dst(y, x0);
        SolidRuns(
            w, c, (color >> 24) == 255,
            [&](int at, int n) { k.blendSolid(dst + at, c + at, static_cast<size_t>(n), color); },
            [&](int at, int n) { FillRun(dst + at, static_cast<size_t>(n), color); });
    }

    // Rows y .. y + count - 1 of `color` under the same coverage c[0..w): its runs found once.
    void SolidRows(int y, int count, int x0, int w, const uint8_t* c, uint32_t color) {
        runs.clear();
        SolidRuns(
            w, c, (color >> 24) == 255, [&](int at, int n) { runs.push_back({at, n, false}); },
            [&](int at, int n) { runs.push_back({at, n, true}); });
        for (int row = y; row < y + count; ++row) {
            uint32_t* dst = Dst(row, x0);
            for (const Run& run : runs) {
                if (run.fill) {
                    FillRun(dst + run.at, static_cast<size_t>(run.n), color);
                } else {
                    k.blendSolid(dst + run.at, c + run.at, static_cast<size_t>(run.n), color);
                }
            }
        }
    }

    // One row of premultiplied src[0..w) under coverage c[0..w), split as SolidRow: when every
    // src pixel is opaque, runs of full coverage are copied.
    void SpanRow(int y, int x0, int w, const uint32_t* s, const uint8_t* c, bool opaqueSrc) const {
        constexpr int kMinRun = 16;
        uint32_t* dst = Dst(y, x0);
        int start = 0;
        int i = 0;
        for (;;) {
            i = NextStop(c, i, w, opaqueSrc);
            if (i >= w) break;
            const uint8_t v = c[i];
            const int end = RunEnd(c, i + 1, w, v);
            if (end - i >= kMinRun) {
                if (i > start) k.blendSpan(dst + start, s + start, c + start, static_cast<size_t>(i - start));
                if (v == 255) std::memcpy(dst + i, s + i, static_cast<size_t>(end - i) * sizeof(uint32_t));
                start = end;
            }
            i = end;
        }
        if (w > start) k.blendSpan(dst + start, s + start, c + start, static_cast<size_t>(w - start));
    }

    struct SolidSink;
    struct GradientSink;
    struct ImageSink;
    struct SamplesSink;
};

struct Canvas::Impl::SolidSink final : SpanSink {
    Impl& c;
    uint32_t color;
    SolidSink(Impl& canvas, uint32_t col) : c(canvas), color(col) {}
    void Span(int y, int x0, int w, const uint8_t* alpha) override {
        c.SolidRow(y, x0, w, c.Masked(alpha, y, x0, w), color);
    }
    void Rows(int y, int count, int x0, int w, const uint8_t* alpha) override {
        if (c.Clip().mask >= 0) {
            SpanSink::Rows(y, count, x0, w, alpha);  // the mask differs from row to row
        } else {
            c.SolidRows(y, count, x0, w, alpha, color);
        }
    }
    bool Splittable() const override { return true; }
};

struct Canvas::Impl::GradientSink final : SpanSink {
    Impl& c;
    const LinearGradient& g;
    Matrix inv;
    double gx, gy, g2;
    GradientSink(Impl& canvas, const LinearGradient& gradient, const Matrix& inverse)
        : c(canvas), g(gradient), inv(inverse) {
        gx = static_cast<double>(g.p1.x) - g.p0.x;
        gy = static_cast<double>(g.p1.y) - g.p0.y;
        g2 = gx * gx + gy * gy;
    }
    void Span(int y, int x0, int w, const uint8_t* alpha) override {
        const uint8_t* a = c.Masked(alpha, y, x0, w);
        uint8_t* cov = c.cov.data();
        if (a != cov) std::memcpy(cov, a, static_cast<size_t>(w));
        uint32_t* src = c.src.data();
        const double px = x0 + 0.5, py = y + 0.5;
        const double ux = inv.a * px + inv.c * py + inv.e - g.p0.x;
        const double uy = inv.b * px + inv.d * py + inv.f - g.p0.y;
        const double t0 = (ux * gx + uy * gy) / g2;
        const double dt = (inv.a * gx + inv.b * gy) / g2;
        for (int i = 0; i < w; ++i) {
            double t = t0 + dt * i;
            if (!(t >= 0)) {
                if (!g.extendStart) {
                    cov[i] = 0;
                    src[i] = 0;
                    continue;
                }
                t = 0;
            } else if (t > 1) {
                if (!g.extendEnd) {
                    cov[i] = 0;
                    src[i] = 0;
                    continue;
                }
                t = 1;
            }
            src[i] = c.lut[static_cast<int>(t * 255 + 0.5)];
        }
        c.SpanRow(y, x0, w, src, cov, g.alpha == 255);
    }
};

namespace {

// 16.16 fixed point, clamped to +-2^24 so that 2^20 steps of a clamped step stay in int64;
// NaN goes to the low end. Indices are clamped into the image per pixel.
int64_t Fixed(double v) {
    constexpr double kLimit = 16777216.0;
    if (!(v > -kLimit)) v = -kLimit;
    if (v > kLimit) v = kLimit;
    return static_cast<int64_t>(RayoMd::ExactMath::Floor(v * 65536.0));
}

// The 0x00XX00XX channels of a pixel spread into two 32-bit lanes of a 64-bit word.
inline uint64_t Lanes(uint32_t p) {
    return (p & 0xFFu) | (static_cast<uint64_t>(p & 0xFF0000u) << 16);
}

// Samples `image` for device pixels x0 .. x0 + w - 1 of row y, `inv` mapping device to image
// pixels, into src[0..w); where `cov` is given, only pixels it covers need be (an upright smooth
// image samples them all).
void SampleSpan(const Image& image, const Matrix& inv, bool smooth, int y, int x0, int w, const uint8_t* cov,
                uint32_t* src, ColumnTable& columns, const Kernels& k) {
    const int64_t iw = image.width, ih = image.height;
    const uint32_t* pixels = image.pixels.data();
    // Image position of the first pixel's centre, and the step per pixel.
    const double px = x0 + 0.5, py = y + 0.5;
    const double ix = inv.a * px + inv.c * py + inv.e;
    const double iy = inv.b * px + inv.d * py + inv.f;
    const int64_t dx = Fixed(inv.a), dy = Fixed(inv.b);
    if (!smooth) {
        int64_t sx = Fixed(ix), sy = Fixed(iy);
        for (int i = 0; i < w; ++i, sx += dx, sy += dy) {
            if (cov && cov[i] == 0) continue;
            const int64_t xi = std::clamp<int64_t>(sx >> 16, 0, iw - 1);
            const int64_t yi = std::clamp<int64_t>(sy >> 16, 0, ih - 1);
            src[i] = pixels[yi * iw + xi];
        }
        return;
    }
    // Bilinear between the four pixels around the centre, weights in 1/256 (edges clamped), two
    // channels at a time in 32-bit lanes: at most 255 * 256 * 256 + 32768.
    constexpr uint64_t kHalf = 32768u | (uint64_t(32768u) << 32);
    constexpr uint64_t kMask = 0xFFu | (uint64_t(0xFFu) << 32);
    int64_t sx = Fixed(ix - 0.5), sy = Fixed(iy - 0.5);
    if (inv.b == 0 && inv.c == 0 && iw >= 2) {
        // Upright: the row's y and its weight are the same along it (dy = Fixed(0) = 0), and the
        // columns those of every other row. A clamped column is the pair (0, 1) weighted
        // (256, 0) on the left and (iw - 2, iw - 1) weighted (0, 256) on the right: the same sums
        // as the clamped pixel taken twice.
        if (columns.width != w || columns.sx != sx || columns.dx != dx || columns.imageWidth != iw) {
            columns.sx = sx;
            columns.dx = dx;
            columns.imageWidth = iw;
            columns.width = w;
            columns.xs.resize(static_cast<size_t>(w));
            columns.ws.resize(static_cast<size_t>(w));
            int64_t s = sx;
            for (size_t i = 0; i < static_cast<size_t>(w); ++i, s += dx) {
                const int64_t xi = s >> 16;
                int64_t x = xi;
                uint32_t w1 = static_cast<uint32_t>((s >> 8) & 255);
                if (xi < 0) {
                    x = 0;
                    w1 = 0;
                } else if (xi >= iw - 1) {
                    x = iw - 2;
                    w1 = 256;
                }
                columns.xs[i] = static_cast<int32_t>(x);
                columns.ws[i] = (256 - w1) | (w1 << 16);
            }
        }
        const int64_t yi = sy >> 16;
        const uint32_t* rowA = pixels + std::clamp<int64_t>(yi, 0, ih - 1) * iw;
        const uint32_t* rowB = pixels + std::clamp<int64_t>(yi + 1, 0, ih - 1) * iw;
        k.bilinearRow(rowA, rowB, static_cast<uint32_t>((sy >> 8) & 255), columns.xs.data(), columns.ws.data(),
                      static_cast<size_t>(w), src);
        return;
    }
    for (int i = 0; i < w; ++i, sx += dx, sy += dy) {
        if (cov && cov[i] == 0) continue;
        const int64_t xi = sx >> 16, yi = sy >> 16;
        const uint64_t wx1 = static_cast<uint64_t>((sx >> 8) & 255), wy1 = static_cast<uint64_t>((sy >> 8) & 255);
        const uint64_t wx0 = 256 - wx1, wy0 = 256 - wy1;
        const int64_t xa = std::clamp<int64_t>(xi, 0, iw - 1), xb = std::clamp<int64_t>(xi + 1, 0, iw - 1);
        const int64_t ya = std::clamp<int64_t>(yi, 0, ih - 1) * iw;
        const int64_t yb = std::clamp<int64_t>(yi + 1, 0, ih - 1) * iw;
        const uint32_t p00 = pixels[ya + xa], p01 = pixels[ya + xb], p10 = pixels[yb + xa], p11 = pixels[yb + xb];
        const uint64_t rbTop = Lanes(p00) * wx0 + Lanes(p01) * wx1;
        const uint64_t rbBottom = Lanes(p10) * wx0 + Lanes(p11) * wx1;
        const uint64_t agTop = Lanes(p00 >> 8) * wx0 + Lanes(p01 >> 8) * wx1;
        const uint64_t agBottom = Lanes(p10 >> 8) * wx0 + Lanes(p11 >> 8) * wx1;
        const uint64_t rb = ((rbTop * wy0 + rbBottom * wy1 + kHalf) >> 16) & kMask;
        const uint64_t ag = ((agTop * wy0 + agBottom * wy1 + kHalf) >> 16) & kMask;
        src[i] = static_cast<uint32_t>(rb | (rb >> 16) | (ag << 8) | (ag >> 8));
    }
}

} // namespace

struct Canvas::Impl::ImageSink final : SpanSink {
    Impl& c;
    const Image& image;
    Matrix inv;  // device to image pixels
    uint8_t opacity;
    bool smooth;
    ImageSink(Impl& canvas, const Image& img, const Matrix& inverse, uint8_t op, bool sm)
        : c(canvas), image(img), inv(inverse), opacity(op), smooth(sm) {}

    // The span's coverage times the clip mask and the opacity: `alpha` itself when neither
    // changes it, otherwise in c.cov.
    static const uint8_t* Coverage(Impl& canvas, const uint8_t* alpha, int y, int x0, int w, uint8_t opacity) {
        const uint8_t* a = canvas.Masked(alpha, y, x0, w);
        if (opacity == 255) return a;
        uint8_t* cov = canvas.cov.data();
        MulRowBy(a, opacity, cov, w);
        return cov;
    }

    void Span(int y, int x0, int w, const uint8_t* alpha) override {
        const uint8_t* cov = Coverage(c, alpha, y, x0, w, opacity);
        SampleSpan(image, inv, smooth, y, x0, w, cov, c.src.data(), c.columns, c.k);
        c.SpanRow(y, x0, w, c.src.data(), cov, image.opaque && opacity == 255);
    }
};

// Draws samples taken in advance: the coverage of the image's square, the samples' rows.
struct Canvas::Impl::SamplesSink final : SpanSink {
    Impl& c;
    const ImageSamples& s;
    uint8_t opacity;
    SamplesSink(Impl& canvas, const ImageSamples& samples, uint8_t op) : c(canvas), s(samples), opacity(op) {}

    void Span(int y, int x0, int w, const uint8_t* alpha) override {
        // The samples cover every pixel the square covers within the surface; anything past
        // them (another matrix than they were taken with) is left out.
        if (y < s.rect.y0 || y >= s.rect.y1) return;
        const int a = std::max(x0, s.rect.x0), b = std::min(x0 + w, s.rect.x1);
        if (a >= b) return;
        const uint8_t* cov = ImageSink::Coverage(c, alpha + (a - x0), y, a, b - a, opacity);
        const uint32_t* src = s.pixels.data() + static_cast<size_t>(y - s.rect.y0) * static_cast<size_t>(s.rect.Width()) +
                              static_cast<size_t>(a - s.rect.x0);
        c.SpanRow(y, a, b - a, src, cov, s.opaque && opacity == 255);
    }
    bool Splittable() const override { return true; }
};

namespace {

struct MaskSink final : SpanSink {
    MaskBuffer& m;
    explicit MaskSink(MaskBuffer& mask) : m(mask) {}
    bool Splittable() const override { return true; }
    void Span(int y, int x0, int w, const uint8_t* alpha) override {
        if (y < m.bounds.y0 || y >= m.bounds.y1) return;
        const int a = std::max(x0, m.bounds.x0), b = std::min(x0 + w, m.bounds.x1);
        if (a >= b) return;
        std::memcpy(m.alpha.data() + static_cast<ptrdiff_t>(y - m.bounds.y0) * m.bounds.Width() + (a - m.bounds.x0),
                    alpha + (a - x0), static_cast<size_t>(b - a));
    }
};

} // namespace

Canvas::Canvas() : impl_(new Impl) {}

Canvas::Canvas(const Surface& target) : impl_(new Impl) {
    SetTarget(target);
}

Canvas::~Canvas() = default;

void Canvas::SetTarget(const Surface& target, int top) {
    Impl& c = *impl_;
    c.target = target;
    const bool valid = target.pixels != nullptr && target.width > 0 && target.height > 0 && target.stride >= target.width &&
                       top >= 0 && top <= std::numeric_limits<int>::max() - target.height;
    c.top = valid ? top : 0;
    c.clips.assign(1, ClipState{valid ? RectI{0, top, target.width, top + target.height} : RectI{}, -1});
    c.maskDepth = 0;
    if (valid) c.Reserve(static_cast<size_t>(target.width));
}

const Surface& Canvas::Target() const {
    return impl_->target;
}

int Canvas::TargetTop() const {
    return impl_->top;
}

RectI Canvas::ClipBounds() const {
    return impl_->Clip().rect;
}

void Canvas::FillRect(RectI rect, uint32_t color) {
    Impl& c = *impl_;
    const RectI r = rect.Intersect(c.Clip().rect);
    if (r.Empty() || !c.Spend(Impl::Area(r))) return;
    const size_t w = static_cast<size_t>(r.Width());
    if (c.Clip().mask >= 0) {
        for (int y = r.y0; y < r.y1; ++y) {
            c.SolidRow(y, r.x0, r.Width(), c.Masked(c.full.data(), y, r.x0, r.Width()), color);
        }
    } else if ((color >> 24) == 255) {
        for (int y = r.y0; y < r.y1; ++y) FillRun(c.Dst(y, r.x0), w, color);
    } else {
        // One row of full coverage for every row.
        c.k.blendMask(c.Dst(r.y0, r.x0), static_cast<size_t>(c.target.stride), c.full.data(), 0, w,
                      static_cast<size_t>(r.Height()), color);
    }
}

void Canvas::FillPath(const Path& path, const Matrix& m, FillRule rule, const Paint& paint) {
    Impl& c = *impl_;
    if (c.Clip().rect.Empty() || path.Empty() || c.Stopped()) return;
    if (paint.gradient) {
        const LinearGradient& g = *paint.gradient;
        Matrix inv;
        if (g.stops.empty() || !m.Invert(inv)) return;
        const double gx = static_cast<double>(g.p1.x) - g.p0.x, gy = static_cast<double>(g.p1.y) - g.p0.y;
        if (!(gx * gx + gy * gy > 0)) return;
        for (int i = 0; i < 256; ++i) c.lut[i] = GradientColor(g, i / 255.0);
        c.raster.Reset(c.Clip().rect);
        c.raster.AddPath(path, m);
        Impl::GradientSink sink(c, g, inv);
        c.raster.Sweep(rule, sink);
        return;
    }
    c.raster.Reset(c.Clip().rect);
    c.raster.AddPath(path, m);
    Impl::SolidSink sink(c, paint.color);
    c.raster.Sweep(rule, sink);
}

void Canvas::StrokePath(const Path& path, const Matrix& m, const StrokeStyle& style, const Paint& paint) {
    Impl& c = *impl_;
    if (c.Clip().rect.Empty() || path.Empty() || c.Stopped()) return;
    StrokeLimits limits;
    limits.clip = c.Clip().rect;
    limits.budget = c.budget;
    StrokeOutline(path, m, style, c.outline, &limits);
    FillPath(c.outline, m, FillRule::NonZero, paint);
}

bool Canvas::CopiedImageRect(const Image& image, const Matrix& unitSquareToDevice, uint8_t opacity, RectI& rect) {
    if (!image.opaque || opacity != 255 || image.width <= 0 || image.height <= 0) return false;
    if (image.pixels.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) return false;
    const Matrix& m = unitSquareToDevice;
    if (!(m.b == 0 && m.c == 0 && m.a == image.width && m.d == image.height && m.e > -1e9 && m.e < 1e9 &&
          m.f > -1e9 && m.f < 1e9 && m.e == RayoMd::ExactMath::Floor(m.e) && m.f == RayoMd::ExactMath::Floor(m.f))) {
        return false;
    }
    const int ex = static_cast<int>(m.e), fy = static_cast<int>(m.f);
    rect = RectI{ex, fy, ex + image.width, fy + image.height};
    return true;
}

void Canvas::DrawImage(const Image& image, const Matrix& unitSquareToDevice, uint8_t opacity, bool smooth) {
    Impl& c = *impl_;
    const RectI clip = c.Clip().rect;
    if (clip.Empty() || opacity == 0 || image.width <= 0 || image.height <= 0 || c.Stopped()) return;
    if (image.pixels.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) return;
    const Matrix& m = unitSquareToDevice;
    const double w = image.width, h = image.height;
    // An opaque image at an integer translation and scale 1: rows copied as they are.
    RectI copied;
    if (c.Clip().mask < 0 && CopiedImageRect(image, m, opacity, copied)) {
        const RectI r = copied.Intersect(clip);
        if (r.Empty() || !c.Spend(Impl::Area(r))) return;
        for (int y = r.y0; y < r.y1; ++y) {
            std::memcpy(c.Dst(y, r.x0),
                        image.pixels.data() + static_cast<size_t>(y - copied.y0) * image.width + (r.x0 - copied.x0),
                        static_cast<size_t>(r.Width()) * sizeof(uint32_t));
        }
        return;
    }
    // Image pixels to device, divided rather than multiplied by a reciprocal so that a scale of 1
    // stays exact; its inverse maps each pixel centre back.
    const Matrix imageToDevice{m.a / w, m.b / w, m.c / h, m.d / h, m.e, m.f};
    Matrix inv;
    if (!imageToDevice.Invert(inv)) return;
    c.raster.Reset(clip);
    c.raster.AddPath(c.unitSquare, m);
    Impl::ImageSink sink(c, image, inv, opacity, smooth);
    c.raster.Sweep(FillRule::NonZero, sink);
}

bool Canvas::SampleImage(const Image& image, const Matrix& unitSquareToDevice, bool smooth, ImageSamples& out) {
    Impl& c = *impl_;
    out.rect = RectI{};
    out.pixels.clear();
    const RectI surface{0, c.top, c.target.width, c.top + c.target.height};
    if (c.target.pixels == nullptr || surface.Empty() || c.Stopped() || image.width <= 0 || image.height <= 0) return false;
    if (image.pixels.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) return false;
    const Matrix& m = unitSquareToDevice;
    const double w = image.width, h = image.height;
    const Matrix imageToDevice{m.a / w, m.b / w, m.c / h, m.d / h, m.e, m.f};
    Matrix inv;
    if (!imageToDevice.Invert(inv)) return false;
    c.raster.Reset(surface);
    c.raster.AddPath(c.unitSquare, m);
    const RectI rect = c.raster.Bounds().Intersect(surface);
    c.raster.Reset(RectI{});
    if (rect.Empty() || !c.Spend(Impl::Area(rect))) return false;
    out.rect = rect;
    out.opaque = image.opaque;
    const size_t width = static_cast<size_t>(rect.Width());
    out.pixels.resize(width * static_cast<size_t>(rect.Height()));
    for (int y = rect.y0; y < rect.y1; ++y) {
        SampleSpan(image, inv, smooth, y, rect.x0, rect.Width(), nullptr,
                   out.pixels.data() + static_cast<size_t>(y - rect.y0) * width, c.columns, c.k);
    }
    return true;
}

void Canvas::DrawSamples(const ImageSamples& samples, const Matrix& unitSquareToDevice, uint8_t opacity) {
    Impl& c = *impl_;
    const RectI clip = c.Clip().rect;
    if (clip.Empty() || opacity == 0 || samples.rect.Empty() || c.Stopped()) return;
    if (samples.pixels.size() < static_cast<size_t>(samples.rect.Width()) * static_cast<size_t>(samples.rect.Height())) return;
    c.raster.Reset(clip);
    c.raster.AddPath(c.unitSquare, unitSquareToDevice);
    Impl::SamplesSink sink(c, samples, opacity);
    c.raster.Sweep(FillRule::NonZero, sink);
}

void Canvas::DrawMask(const uint8_t* alpha, int stride, RectI dst, uint32_t color) {
    Impl& c = *impl_;
    if (alpha == nullptr) return;
    const RectI r = dst.Intersect(c.Clip().rect);
    if (r.Empty() || !c.Spend(Impl::Area(r))) return;
    const uint8_t* first = alpha + static_cast<ptrdiff_t>(r.y0 - dst.y0) * stride + (r.x0 - dst.x0);
    if (c.Clip().mask < 0) {
        // The rectangle at once: the kernel's setup once, not per row.
        c.k.blendMask(c.Dst(r.y0, r.x0), static_cast<size_t>(c.target.stride), first, static_cast<size_t>(stride),
                      static_cast<size_t>(r.Width()), static_cast<size_t>(r.Height()), color);
        return;
    }
    for (int y = r.y0; y < r.y1; ++y) {
        const uint8_t* row = first + static_cast<ptrdiff_t>(y - r.y0) * stride;
        c.k.blendSolid(c.Dst(y, r.x0), c.Masked(row, y, r.x0, r.Width()), static_cast<size_t>(r.Width()), color);
    }
}

void Canvas::PushClipRect(RectI rect) {
    Impl& c = *impl_;
    const ClipState top = c.Clip();
    c.clips.push_back({rect.Intersect(top.rect), top.mask});
}

bool Canvas::Impl::RasterMask(const Path& path, const Matrix& m, FillRule rule, RectI rect, int old, MaskBuffer& mask) {
    if (Stopped()) return false;
    raster.Reset(rect);
    raster.AddPath(path, m);
    const RectI bounds = raster.Bounds().Intersect(rect);
    if (bounds.Empty() || !Spend(Area(bounds))) return false;
    mask.bounds = bounds;
    mask.alpha.assign(static_cast<size_t>(bounds.Width()) * static_cast<size_t>(bounds.Height()), 0);
    MaskSink sink(mask);
    raster.Sweep(rule, sink);
    if (old >= 0) {
        const MaskBuffer& o = masks[static_cast<size_t>(old)];
        for (int y = bounds.y0; y < bounds.y1; ++y) {
            uint8_t* row = mask.alpha.data() + static_cast<ptrdiff_t>(y - bounds.y0) * bounds.Width();
            const uint8_t* prev = o.alpha.data() + static_cast<ptrdiff_t>(y - o.bounds.y0) * o.bounds.Width() +
                                  (bounds.x0 - o.bounds.x0);
            MulRow(row, prev, row, bounds.Width());
        }
    }
    return true;
}

void Canvas::PushClipPath(const Path& path, const Matrix& m, FillRule rule) {
    Impl& c = *impl_;
    const ClipState top = c.Clip();
    if (c.maskDepth == c.masks.size()) c.masks.emplace_back();
    if (!c.RasterMask(path, m, rule, top.rect, top.mask, c.masks[c.maskDepth])) {
        c.clips.push_back({RectI{}, top.mask});
        return;
    }
    c.clips.push_back({c.masks[c.maskDepth].bounds, static_cast<int>(c.maskDepth)});
    ++c.maskDepth;
}

void Canvas::PopClip() {
    Impl& c = *impl_;
    if (c.clips.size() <= 1) return;
    const int mask = c.clips.back().mask;
    c.clips.pop_back();
    if (mask != c.clips.back().mask) --c.maskDepth;
}

void Canvas::IntersectClipRect(RectI rect) {
    Impl& c = *impl_;
    if (c.clips.size() <= 1) {
        PushClipRect(rect);
        return;
    }
    ClipState& top = c.clips.back();
    top.rect = top.rect.Intersect(rect);
}

void Canvas::IntersectClipPath(const Path& path, const Matrix& m, FillRule rule) {
    Impl& c = *impl_;
    if (c.clips.size() <= 1) {
        PushClipPath(path, m, rule);
        return;
    }
    const int below = c.clips[c.clips.size() - 2].mask;
    const int current = c.clips.back().mask;
    const bool owns = current >= 0 && current != below;
    if (!owns && c.maskDepth == c.masks.size()) c.masks.emplace_back();
    MaskBuffer& fresh = owns ? c.spare : c.masks[c.maskDepth];
    ClipState& top = c.clips.back();
    if (!c.RasterMask(path, m, rule, top.rect, current, fresh)) {
        top.rect = RectI{};  // keeps its mask, so PopClip still finds it
        return;
    }
    top.rect = fresh.bounds;
    if (owns) {
        std::swap(c.masks[static_cast<size_t>(current)], c.spare);
    } else {
        top.mask = static_cast<int>(c.maskDepth);
        ++c.maskDepth;
    }
}

size_t Canvas::ClipDepth() const {
    return impl_->clips.size() - 1;
}

size_t Canvas::MaskBytes() const {
    const Impl& c = *impl_;
    size_t bytes = 0;
    for (size_t i = 0; i < c.maskDepth; ++i) bytes += c.masks[i].alpha.size();
    return bytes;
}

void Canvas::SetBudget(WorkBudget* budget) {
    impl_->budget = budget;
    impl_->raster.SetBudget(budget);
}

void Canvas::Trim(size_t bytes) {
    Impl& c = *impl_;
    c.raster.Trim(bytes);
    for (size_t i = c.maskDepth; i < c.masks.size(); ++i) {
        if (c.masks[i].alpha.capacity() > bytes) std::vector<uint8_t>().swap(c.masks[i].alpha);
    }
    if (c.spare.alpha.capacity() > bytes) std::vector<uint8_t>().swap(c.spare.alpha);
    if (c.outline.Points().capacity() * sizeof(PointF) > bytes) c.outline = Path();
}

void FillPixels(uint32_t* p, size_t n, uint32_t v) {
    FillRun(p, n, v);
}

} // namespace rgui::gfx
