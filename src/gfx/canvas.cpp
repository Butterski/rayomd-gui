// SPDX-License-Identifier: Apache-2.0
// The canvas (plan Phase 4 step 5): fills, gradients, strokes, images and masks under a stack of
// rectangle and path clips. Every span is limited to the clip rectangle, which a path clip
// shrinks to its mask's bounds, and its coverage is multiplied by the mask row where there is one.
#include "gfx/canvas.h"

#include "common/exact_math.h"
#include "gfx/kernels.h"

#include <algorithm>
#include <cstring>

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

} // namespace

struct Canvas::Impl {
    Surface target{};
    std::vector<ClipState> clips{{RectI{}, -1}};
    std::vector<MaskBuffer> masks;
    size_t maskDepth = 0;
    Rasterizer raster;
    std::vector<uint8_t> cov;
    std::vector<uint32_t> src;
    std::vector<uint8_t> full;
    uint32_t lut[256] = {};
    Path outline;
    Path unitSquare;
    const Kernels& k = ActiveKernels();

    Impl() {
        unitSquare.AddRect(0, 0, 1, 1);
    }

    const ClipState& Clip() const { return clips.back(); }

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
        for (int i = 0; i < w; ++i) out[i] = static_cast<uint8_t>(Mul255(alpha[i], row[i]));
        return out;
    }

    uint32_t* Dst(int y, int x) const { return target.Row(y) + x; }

    // One row of `color` under coverage c[0..w): runs of 16 or more zeros are skipped and, for an
    // opaque colour, runs as long of full coverage are stored; the rest is blended. Stores and
    // skips give the bytes blend_solid gives there.
    void SolidRow(int y, int x0, int w, const uint8_t* c, uint32_t color) const {
        constexpr int kMinRun = 16;
        uint32_t* dst = Dst(y, x0);
        const bool opaque = (color >> 24) == 255;
        int start = 0;
        int i = 0;
        while (i < w) {
            const uint8_t v = c[i];
            if (v != 0 && !(v == 255 && opaque)) {
                ++i;
                continue;
            }
            int end = i + 1;
            while (end < w && c[end] == v) ++end;
            if (end - i >= kMinRun) {
                if (i > start) k.blendSolid(dst + start, c + start, static_cast<size_t>(i - start), color);
                if (v == 255) std::fill_n(dst + i, end - i, color);
                start = end;
            }
            i = end;
        }
        if (w > start) k.blendSolid(dst + start, c + start, static_cast<size_t>(w - start), color);
    }

    // One row of premultiplied src[0..w) under coverage c[0..w), split as SolidRow: when every
    // src pixel is opaque, runs of full coverage are copied.
    void SpanRow(int y, int x0, int w, const uint32_t* s, const uint8_t* c, bool opaqueSrc) const {
        constexpr int kMinRun = 16;
        uint32_t* dst = Dst(y, x0);
        int start = 0;
        int i = 0;
        while (i < w) {
            const uint8_t v = c[i];
            if (v != 0 && !(v == 255 && opaqueSrc)) {
                ++i;
                continue;
            }
            int end = i + 1;
            while (end < w && c[end] == v) ++end;
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
};

struct Canvas::Impl::SolidSink final : SpanSink {
    Impl& c;
    uint32_t color;
    SolidSink(Impl& canvas, uint32_t col) : c(canvas), color(col) {}
    void Span(int y, int x0, int w, const uint8_t* alpha) override {
        c.SolidRow(y, x0, w, c.Masked(alpha, y, x0, w), color);
    }
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

struct Canvas::Impl::ImageSink final : SpanSink {
    Impl& c;
    const Image& image;
    Matrix inv;  // device to image pixels
    uint8_t opacity;
    bool smooth;
    ImageSink(Impl& canvas, const Image& img, const Matrix& inverse, uint8_t op, bool sm)
        : c(canvas), image(img), inv(inverse), opacity(op), smooth(sm) {}

    // 16.16 fixed point, clamped to +-2^24 so that 2^20 steps of a clamped step stay in int64;
    // NaN goes to the low end. Indices are clamped into the image per pixel.
    static int64_t Fixed(double v) {
        constexpr double kLimit = 16777216.0;
        if (!(v > -kLimit)) v = -kLimit;
        if (v > kLimit) v = kLimit;
        return static_cast<int64_t>(RayoMd::ExactMath::Floor(v * 65536.0));
    }

    // The 0x00XX00XX channels of a pixel spread into two 32-bit lanes of a 64-bit word.
    static uint64_t Lanes(uint32_t p) { return (p & 0xFFu) | (static_cast<uint64_t>(p & 0xFF0000u) << 16); }

    void Span(int y, int x0, int w, const uint8_t* alpha) override {
        const uint8_t* a = c.Masked(alpha, y, x0, w);
        uint8_t* cov = c.cov.data();
        if (opacity == 255) {
            if (a != cov) std::memcpy(cov, a, static_cast<size_t>(w));
        } else {
            for (int i = 0; i < w; ++i) cov[i] = static_cast<uint8_t>(Mul255(a[i], opacity));
        }
        uint32_t* src = c.src.data();
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
                if (cov[i] == 0) continue;
                const int64_t xi = std::clamp<int64_t>(sx >> 16, 0, iw - 1);
                const int64_t yi = std::clamp<int64_t>(sy >> 16, 0, ih - 1);
                src[i] = pixels[yi * iw + xi];
            }
        } else {
            // Bilinear between the four pixels around the centre, weights in 1/256 (edges
            // clamped), two channels at a time in 32-bit lanes: at most 255 * 256 * 256 + 32768.
            constexpr uint64_t kHalf = 32768u | (uint64_t(32768u) << 32);
            constexpr uint64_t kMask = 0xFFu | (uint64_t(0xFFu) << 32);
            int64_t sx = Fixed(ix - 0.5), sy = Fixed(iy - 0.5);
            for (int i = 0; i < w; ++i, sx += dx, sy += dy) {
                if (cov[i] == 0) continue;
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
        c.SpanRow(y, x0, w, src, cov, image.opaque && opacity == 255);
    }
};

namespace {

struct MaskSink final : SpanSink {
    MaskBuffer& m;
    explicit MaskSink(MaskBuffer& mask) : m(mask) {}
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

void Canvas::SetTarget(const Surface& target) {
    Impl& c = *impl_;
    c.target = target;
    const bool valid = target.pixels != nullptr && target.width > 0 && target.height > 0 && target.stride >= target.width;
    c.clips.assign(1, ClipState{valid ? RectI{0, 0, target.width, target.height} : RectI{}, -1});
    c.maskDepth = 0;
    if (valid) c.Reserve(static_cast<size_t>(target.width));
}

const Surface& Canvas::Target() const {
    return impl_->target;
}

RectI Canvas::ClipBounds() const {
    return impl_->Clip().rect;
}

void Canvas::FillRect(RectI rect, uint32_t color) {
    Impl& c = *impl_;
    const RectI r = rect.Intersect(c.Clip().rect);
    if (r.Empty()) return;
    const size_t w = static_cast<size_t>(r.Width());
    if (c.Clip().mask >= 0) {
        for (int y = r.y0; y < r.y1; ++y) {
            c.SolidRow(y, r.x0, r.Width(), c.Masked(c.full.data(), y, r.x0, r.Width()), color);
        }
    } else if ((color >> 24) == 255) {
        for (int y = r.y0; y < r.y1; ++y) std::fill_n(c.Dst(y, r.x0), w, color);
    } else {
        for (int y = r.y0; y < r.y1; ++y) c.k.blendSolid(c.Dst(y, r.x0), c.full.data(), w, color);
    }
}

void Canvas::FillPath(const Path& path, const Matrix& m, FillRule rule, const Paint& paint) {
    Impl& c = *impl_;
    if (c.Clip().rect.Empty() || path.Empty()) return;
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
    if (c.Clip().rect.Empty() || path.Empty()) return;
    StrokeOutline(path, m, style, c.outline);
    FillPath(c.outline, m, FillRule::NonZero, paint);
}

void Canvas::DrawImage(const Image& image, const Matrix& unitSquareToDevice, uint8_t opacity, bool smooth) {
    Impl& c = *impl_;
    const RectI clip = c.Clip().rect;
    if (clip.Empty() || opacity == 0 || image.width <= 0 || image.height <= 0) return;
    if (image.pixels.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) return;
    const Matrix& m = unitSquareToDevice;
    const double w = image.width, h = image.height;
    // An opaque image at an integer translation and scale 1: rows copied as they are.
    if (image.opaque && opacity == 255 && c.Clip().mask < 0 && m.b == 0 && m.c == 0 && m.a == w && m.d == h &&
        m.e > -1e9 && m.e < 1e9 && m.f > -1e9 && m.f < 1e9 && m.e == RayoMd::ExactMath::Floor(m.e) &&
        m.f == RayoMd::ExactMath::Floor(m.f)) {
        const int ex = static_cast<int>(m.e), fy = static_cast<int>(m.f);
        const RectI r = RectI{ex, fy, ex + image.width, fy + image.height}.Intersect(clip);
        if (r.Empty()) return;
        for (int y = r.y0; y < r.y1; ++y) {
            std::memcpy(c.Dst(y, r.x0), image.pixels.data() + static_cast<size_t>(y - fy) * image.width + (r.x0 - ex),
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

void Canvas::DrawMask(const uint8_t* alpha, int stride, RectI dst, uint32_t color) {
    Impl& c = *impl_;
    if (alpha == nullptr) return;
    const RectI r = dst.Intersect(c.Clip().rect);
    if (r.Empty()) return;
    for (int y = r.y0; y < r.y1; ++y) {
        const uint8_t* row = alpha + static_cast<ptrdiff_t>(y - dst.y0) * stride + (r.x0 - dst.x0);
        c.k.blendSolid(c.Dst(y, r.x0), c.Masked(row, y, r.x0, r.Width()), static_cast<size_t>(r.Width()), color);
    }
}

void Canvas::PushClipRect(RectI rect) {
    Impl& c = *impl_;
    const ClipState top = c.Clip();
    c.clips.push_back({rect.Intersect(top.rect), top.mask});
}

void Canvas::PushClipPath(const Path& path, const Matrix& m, FillRule rule) {
    Impl& c = *impl_;
    const ClipState top = c.Clip();
    c.raster.Reset(top.rect);
    c.raster.AddPath(path, m);
    const RectI bounds = c.raster.Bounds().Intersect(top.rect);
    if (bounds.Empty()) {
        c.clips.push_back({RectI{}, top.mask});
        return;
    }
    if (c.maskDepth == c.masks.size()) c.masks.emplace_back();
    MaskBuffer& mask = c.masks[c.maskDepth];
    mask.bounds = bounds;
    mask.alpha.assign(static_cast<size_t>(bounds.Width()) * static_cast<size_t>(bounds.Height()), 0);
    MaskSink sink(mask);
    c.raster.Sweep(rule, sink);
    if (top.mask >= 0) {
        const MaskBuffer& old = c.masks[static_cast<size_t>(top.mask)];
        for (int y = bounds.y0; y < bounds.y1; ++y) {
            uint8_t* row = mask.alpha.data() + static_cast<ptrdiff_t>(y - bounds.y0) * bounds.Width();
            const uint8_t* o = old.alpha.data() + static_cast<ptrdiff_t>(y - old.bounds.y0) * old.bounds.Width() +
                               (bounds.x0 - old.bounds.x0);
            for (int x = 0; x < bounds.Width(); ++x) row[x] = static_cast<uint8_t>(Mul255(row[x], o[x]));
        }
    }
    c.clips.push_back({bounds, static_cast<int>(c.maskDepth)});
    ++c.maskDepth;
}

void Canvas::PopClip() {
    Impl& c = *impl_;
    if (c.clips.size() <= 1) return;
    const int mask = c.clips.back().mask;
    c.clips.pop_back();
    if (mask != c.clips.back().mask) --c.maskDepth;
}

} // namespace rgui::gfx
