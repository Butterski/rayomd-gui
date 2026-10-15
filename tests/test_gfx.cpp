// SPDX-License-Identifier: Apache-2.0
// gfx.*: paths, the rasterizer, the stroker, the canvas, the PNG writer and icon paths.
#include "runner.h"

#include "common/file_io.h"
#include "core/inflate.h"
#include "gfx/budget.h"
#include "gfx/canvas.h"
#include "gfx/crc32.h"
#include "gfx/kernels.h"
#include "gfx/path.h"
#include "gfx/png_write.h"
#include "gfx/raster.h"
#include "gfx/stroke.h"
#include "gfx/svgpath.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace rgui::gfx;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kKappa = 0.5522847498f;

// Collects the rasterizer's rows into a grid over its clip.
struct Grid final : SpanSink {
    RectI clip;
    std::vector<uint8_t> a;
    bool outside = false;  // a span reached past the clip

    explicit Grid(RectI c) : clip(c), a(static_cast<size_t>(c.Width() * c.Height()), 0) {}
    void Span(int y, int x0, int width, const uint8_t* alpha) override {
        if (y < clip.y0 || y >= clip.y1 || x0 < clip.x0 || x0 + width > clip.x1) {
            outside = true;
            return;
        }
        for (int i = 0; i < width; ++i) a[(y - clip.y0) * clip.Width() + (x0 - clip.x0 + i)] = alpha[i];
    }
    int At(int x, int y) const { return a[(y - clip.y0) * clip.Width() + (x - clip.x0)]; }
    double Area() const {
        double s = 0;
        for (uint8_t v : a) s += v;
        return s / 255.0;
    }
};

Grid Fill(const Path& path, RectI clip, FillRule rule, const Matrix& m = {}, float tolerance = kFlattenTolerance) {
    Rasterizer r;
    r.Reset(clip);
    r.AddPath(path, m, tolerance);
    Grid g(clip);
    r.Sweep(rule, g);
    return g;
}

void AddCircle(Path& p, float cx, float cy, float r) {
    const float k = r * kKappa;
    p.MoveTo(cx + r, cy);
    p.CubicTo(cx + r, cy + k, cx + k, cy + r, cx, cy + r);
    p.CubicTo(cx - k, cy + r, cx - r, cy + k, cx - r, cy);
    p.CubicTo(cx - r, cy - k, cx - k, cy - r, cx, cy - r);
    p.CubicTo(cx + k, cy - r, cx + r, cy - k, cx + r, cy);
    p.Close();
}

} // namespace

RGUI_TEST("gfx.raster_rect_edges") {
    Path p;
    p.AddRect(10.25f, 10.5f, 10.5f, 19.5f);  // (10.25, 10.5) - (20.75, 30)
    const Grid g = Fill(p, {0, 0, 32, 32}, FillRule::NonZero);
    CHECK(!g.outside);
    bool ok = true;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            int want = 0;
            if (y >= 11 && y <= 29) want = (x == 10 || x == 20) ? 192 : (x > 10 && x < 20 ? 255 : 0);
            if (y == 10) want = (x == 10 || x == 20) ? 96 : (x > 10 && x < 20 ? 128 : 0);
            if (g.At(x, y) != want) {
                if (ok) CHECK_EQ(g.At(x, y), want);
                ok = false;
            }
        }
    }
    CHECK(ok);
    // The same rectangle with a clip whose corner is not the origin.
    const Grid h = Fill(p, {8, 9, 24, 31}, FillRule::NonZero);
    CHECK_EQ(h.At(10, 12), 192);
    CHECK_EQ(h.At(15, 10), 128);
    CHECK_EQ(h.At(20, 10), 96);
    CHECK_EQ(h.At(15, 29), 255);
    CHECK_EQ(h.At(21, 20), 0);
    CHECK_EQ(h.At(15, 30), 0);
}

// The flattened outline's own area (shoelace).
static double PolygonArea(const Path& path, const Matrix& m, float tolerance) {
    struct Shoelace final : LineSink {
        double twice = 0;
        void Line(PointF a, PointF b) override { twice += double(a.x) * b.y - double(b.x) * a.y; }
    } s;
    Flatten(path, m, tolerance, s);
    return s.twice < 0 ? -s.twice / 2 : s.twice / 2;
}

RGUI_TEST("gfx.raster_circle_area") {
    Path p;
    AddCircle(p, 64.3f, 63.7f, 50);
    const double want = kPi * 50 * 50;
    // The rasterizer covers exactly the area of the polygon it is given (edge pixels at
    // 255 / 256 of full make the sum read up to 0.4 % of the edge pixels high).
    const Grid g = Fill(p, {0, 0, 128, 128}, FillRule::NonZero);
    CHECK_NEAR(g.Area(), PolygonArea(p, {}, kFlattenTolerance), want * 0.0005);
    CHECK_EQ(g.At(64, 64), 255);
    CHECK_EQ(g.At(2, 2), 0);
    // At 0.2 px the four cubics flatten to a 40-gon, 0.41 % short of the circle; at 0.1 px to a
    // 56-gon, 0.21 % short.
    CHECK_NEAR(g.Area(), want, want * 0.005);
    const Grid fine = Fill(p, {0, 0, 128, 128}, FillRule::NonZero, {}, 0.1f);
    CHECK_NEAR(fine.Area(), want, want * 0.003);
    // Scaled by a matrix: radius 10 * 5 flattens as radius 50 does.
    Path q;
    AddCircle(q, 12.86f, 12.74f, 10);
    const Grid h = Fill(q, {0, 0, 128, 128}, FillRule::NonZero, Matrix::Scale(5, 5));
    CHECK_NEAR(h.Area(), g.Area(), want * 0.0005);
}

RGUI_TEST("gfx.raster_fill_rules") {
    Path ring;
    AddCircle(ring, 50, 50, 40);
    AddCircle(ring, 50, 50, 20);  // same direction
    const Grid eo = Fill(ring, {0, 0, 100, 100}, FillRule::EvenOdd);
    const Grid nz = Fill(ring, {0, 0, 100, 100}, FillRule::NonZero);
    CHECK_EQ(eo.At(50, 50), 0);
    CHECK_EQ(nz.At(50, 50), 255);
    CHECK_EQ(eo.At(50, 20), 255);  // in the ring
    CHECK_EQ(nz.At(50, 20), 255);
    CHECK_NEAR(eo.Area(), kPi * (1600 - 400), kPi * 1200 * 0.005);

    // A five-point star drawn as a pentagram: its centre is wound twice.
    const float px[5] = {0.0f, 0.95105652f, 0.58778525f, -0.58778525f, -0.95105652f};
    const float py[5] = {-1.0f, -0.30901699f, 0.80901699f, 0.80901699f, -0.30901699f};
    Path star;
    const int order[5] = {0, 2, 4, 1, 3};
    for (int i = 0; i < 5; ++i) {
        const float x = 50 + 45 * px[order[i]], y = 50 + 45 * py[order[i]];
        if (i == 0) star.MoveTo(x, y);
        else star.LineTo(x, y);
    }
    star.Close();
    const Grid seo = Fill(star, {0, 0, 100, 100}, FillRule::EvenOdd);
    const Grid snz = Fill(star, {0, 0, 100, 100}, FillRule::NonZero);
    CHECK_EQ(seo.At(50, 50), 0);
    CHECK_EQ(snz.At(50, 50), 255);
    CHECK_EQ(seo.At(50, 12), 255);  // inside the top point
    CHECK_EQ(snz.At(50, 12), 255);
}

RGUI_TEST("gfx.raster_clipping") {
    // Left of the clip: the outside part still covers the clip's first columns.
    Path a;
    a.AddRect(-10, 2, 15.5f, 4);  // x -10 .. 5.5
    const Grid ga = Fill(a, {0, 0, 8, 8}, FillRule::NonZero);
    CHECK(!ga.outside);
    CHECK_EQ(ga.At(0, 3), 255);
    CHECK_EQ(ga.At(4, 3), 255);
    CHECK_EQ(ga.At(5, 3), 128);
    CHECK_EQ(ga.At(6, 3), 0);
    CHECK_EQ(ga.At(0, 1), 0);
    // A slanted edge cut by the left side of the clip keeps its slope inside.
    Path t;
    t.MoveTo(-8, 0);
    t.LineTo(8, 16);
    t.LineTo(-8, 16);
    t.Close();
    const Grid gt = Fill(t, {0, 0, 16, 16}, FillRule::NonZero);
    CHECK_EQ(gt.At(0, 10), 255);
    CHECK_EQ(gt.At(0, 4), 0);
    CHECK_EQ(gt.At(4, 12), 128);  // on the diagonal x = y - 8
    CHECK_EQ(gt.At(5, 12), 0);
    // Right of the clip and above and below it.
    Path b;
    b.AddRect(3, -50, 1000, 52.25f);  // y -50 .. 2.25
    const Grid gb = Fill(b, {0, 0, 8, 8}, FillRule::NonZero);
    CHECK(!gb.outside);
    CHECK_EQ(gb.At(3, 0), 255);
    CHECK_EQ(gb.At(7, 1), 255);
    CHECK_EQ(gb.At(7, 2), 64);
    CHECK_EQ(gb.At(2, 1), 0);
    // Huge coordinates fill exactly; NaN and infinite ones are dropped without harm.
    Path h;
    h.AddRect(-1e20f, -1e20f, 2e20f, 2e20f);
    const Grid gh = Fill(h, {0, 0, 8, 8}, FillRule::NonZero);
    CHECK_EQ(gh.Area(), 64.0);
    Path n;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    n.MoveTo(1, 1);
    n.LineTo(nan, 4);
    n.LineTo(inf, -inf);
    n.CubicTo(nan, nan, 3e38f, -3e38f, 6, 6);
    n.Close();
    Rasterizer r;
    r.Reset({0, 0, 8, 8});
    r.AddPath(n, Matrix{1e30, 0, 0, 1e30, 0, 0});
    Grid gn({0, 0, 8, 8});
    r.Sweep(FillRule::NonZero, gn);
    CHECK(!gn.outside);
}

RGUI_TEST("gfx.raster_bounds") {
    Rasterizer r;
    r.Reset({0, 0, 100, 100});
    CHECK(r.Bounds().Empty());
    Path p;
    p.AddRect(10.5f, 20, 5, 30.25f);
    r.AddPath(p, Matrix::Translate(1, 2));
    CHECK(r.Bounds() == (RectI{11, 22, 17, 53}));
    r.Reset({0, 0, 0, 10});
    r.AddPath(p, {});
    CHECK(r.Bounds().Empty());
    Grid g({0, 0, 1, 1});
    r.Sweep(FillRule::NonZero, g);
    CHECK_EQ(g.Area(), 0.0);
}

RGUI_TEST("gfx.path_flatten") {
    // A quadratic with |p0 - 2p1 + p2| = 80 at tolerance 0.2 makes ceil(sqrt(100)) = 10 lines,
    // plus the closing line.
    struct Count final : LineSink {
        int lines = 0;
        PointF last{};
        void Line(PointF, PointF to) override {
            ++lines;
            last = to;
        }
    } count;
    Path q;
    q.MoveTo(0, 0);
    q.QuadTo(20, 40, 40, 0);
    Flatten(q, {}, 0.2f, count);
    CHECK_EQ(count.lines, 11);
    CHECK_EQ(count.last.x, 0.0f);
    // The same quadratic scaled 4x: 4 * 80 / 0.8 = 400, sqrt 20.
    count.lines = 0;
    Flatten(q, Matrix::Scale(4, 4), 0.2f, count);
    CHECK_EQ(count.lines, 21);
    // A cubic: 0.75 * 120 / 0.2 = 450, sqrt -> 22 pieces; huge ones stop at 256.
    count.lines = 0;
    Path c;
    c.MoveTo(0, 0);
    c.CubicTo(0, 60, 60, 60, 60, 0);  // |p0 - 2p1 + p2| = |(60, -60)| = 84.85; |p1 - 2p2 + p3| the same
    Flatten(c, {}, 0.2f, count);
    CHECK_EQ(count.lines, 1 + 18);  // sqrt(0.75 * 84.85 / 0.2) = 17.84
    count.lines = 0;
    Flatten(c, Matrix::Scale(1e6, 1e6), 0.2f, count);
    CHECK_EQ(count.lines, 1 + 256);
    // Bounds and polylines: an open subpath stays open, a lone MoveTo is one point.
    CHECK_EQ(c.Bounds().w, 60.0f);
    struct Polys final : PolylineSink {
        std::vector<size_t> counts;
        std::vector<bool> closed;
        void Polyline(const PointF*, size_t n, bool isClosed) override {
            counts.push_back(n);
            closed.push_back(isClosed);
        }
    } polys;
    Path o;
    o.MoveTo(0, 0);
    o.LineTo(10, 0);
    o.LineTo(10, 10);
    o.MoveTo(5, 5);
    o.AddRect(0, 0, 1, 1);
    o.LineTo(3, 3);  // after Close: a new subpath from the closed one's start
    std::vector<PointF> scratch;
    FlattenPolylines(o, {}, 0.2f, scratch, polys);
    CHECK_EQ(polys.counts.size(), size_t(4));
    if (polys.counts.size() == 4) {
        CHECK_EQ(polys.counts[0], size_t(3));
        CHECK(!polys.closed[0]);
        CHECK_EQ(polys.counts[1], size_t(1));
        CHECK_EQ(polys.counts[2], size_t(4));
        CHECK(polys.closed[2]);
        CHECK_EQ(polys.counts[3], size_t(2));
    }
}

namespace {

Grid Stroke(const Path& path, const StrokeStyle& style, RectI clip, const Matrix& m = {}) {
    Path outline;
    StrokeOutline(path, m, style, outline);
    return Fill(outline, clip, FillRule::NonZero, m);
}

StrokeStyle Style(float width, LineCap cap, LineJoin join = LineJoin::Miter, float miterLimit = 10) {
    StrokeStyle s;
    s.width = width;
    s.cap = cap;
    s.join = join;
    s.miterLimit = miterLimit;
    return s;
}

Path Line(float x0, float y0, float x1, float y1) {
    Path p;
    p.MoveTo(x0, y0);
    p.LineTo(x1, y1);
    return p;
}

} // namespace

RGUI_TEST("gfx.stroke_caps") {
    const RectI clip{0, 0, 256, 128};
    const Path line = Line(50, 50, 150, 50);
    CHECK_NEAR(Stroke(line, Style(10, LineCap::Butt), clip).Area(), 1000, 1);
    CHECK_NEAR(Stroke(line, Style(10, LineCap::Square), clip).Area(), 1100, 1);
    // Round caps: a half disc of radius 5 at each end, pi * 25 together.
    const Grid round = Stroke(line, Style(10, LineCap::Round), clip);
    CHECK_NEAR(round.Area() - 1000, kPi * 25, kPi * 25 * 0.01);
    CHECK_EQ(round.At(153, 50), 255);  // within the cap
    CHECK_EQ(round.At(154, 45), 0);    // outside the disc, inside the square cap's corner
    // A diagonal line covers the same area.
    const Path diagonal = Line(40, 30, 40 + 60, 30 + 80);
    CHECK_NEAR(Stroke(diagonal, Style(10, LineCap::Butt), clip).Area(), 1000, 1);
    // Zero-length subpaths: a dot, a square, nothing; a lone MoveTo is nothing.
    const Path point = Line(60, 60, 60, 60);
    CHECK_NEAR(Stroke(point, Style(10, LineCap::Round), clip).Area(), kPi * 25, kPi * 25 * 0.01);
    CHECK_NEAR(Stroke(point, Style(10, LineCap::Square), clip).Area(), 100, 1);
    CHECK_EQ(Stroke(point, Style(10, LineCap::Butt), clip).Area(), 0.0);
    Path lone;
    lone.MoveTo(60, 60);
    CHECK_EQ(Stroke(lone, Style(10, LineCap::Round), clip).Area(), 0.0);
}

RGUI_TEST("gfx.stroke_joins") {
    // A closed square: the outer square less the inner one, corners mitred.
    Path square;
    square.AddRect(20, 20, 40, 40);
    CHECK_NEAR(Stroke(square, Style(4, LineCap::Butt), {0, 0, 128, 128}).Area(), 44.0 * 44 - 36.0 * 36, 1);
    // Bevelled, each of the four corners loses a right triangle of legs 2.
    CHECK_NEAR(Stroke(square, Style(4, LineCap::Butt, LineJoin::Bevel), {0, 0, 128, 128}).Area(),
               44.0 * 44 - 36.0 * 36 - 4 * 2, 1);

    // A 10 degree corner at (150, 100): 1 / sin(5 deg) = 11.5 exceeds a miter limit of 4.
    Path corner;
    corner.MoveTo(50, 100);
    corner.LineTo(150, 100);
    corner.LineTo(150 - 98.4807753f, 100 + 17.3648178f);
    const RectI clip{0, 0, 256, 160};
    const Grid limited = Stroke(corner, Style(10, LineCap::Butt, LineJoin::Miter, 4), clip);
    const Grid mitred = Stroke(corner, Style(10, LineCap::Butt, LineJoin::Miter, 12), clip);
    const Grid bevel = Stroke(corner, Style(10, LineCap::Butt, LineJoin::Bevel), clip);
    const Grid rounded = Stroke(corner, Style(10, LineCap::Butt, LineJoin::Round), clip);
    // 30 px out along the bisector: only inside the 57 px miter.
    CHECK_EQ(limited.At(179, 97), 0);
    CHECK_EQ(mitred.At(179, 97), 255);
    CHECK_NEAR(limited.Area(), bevel.Area(), 0.01);
    CHECK(mitred.Area() > bevel.Area() + 100);
    // 3.5 px out: within the round join's radius of 5, past the bevel.
    CHECK_EQ(rounded.At(153, 99), 255);
    CHECK_EQ(bevel.At(153, 99), 0);
    CHECK_EQ(rounded.At(157, 99), 0);
}

RGUI_TEST("gfx.stroke_dash") {
    const RectI clip{0, 0, 160, 100};
    const Path line = Line(20, 50, 120, 50);
    auto runs = [](const Grid& g, int y) {
        int count = 0;
        bool in = false;
        for (int x = g.clip.x0; x < g.clip.x1; ++x) {
            const bool on = g.At(x, y) > 127;
            if (on && !in) ++count;
            in = on;
        }
        return count;
    };
    StrokeStyle s = Style(10, LineCap::Butt);
    s.dash = {10, 5};
    const Grid dashed = Stroke(line, s, clip);
    CHECK_EQ(runs(dashed, 50), 7);
    CHECK_NEAR(dashed.Area(), 700, 14);
    CHECK_EQ(dashed.At(32, 50), 0);  // the first gap, 10 .. 15
    // The phase starts the pattern 5 units in: dashes 0-5, 10-20, 25-35, ... 85-95.
    s.dashPhase = 5;
    const Grid shifted = Stroke(line, s, clip);
    CHECK_EQ(runs(shifted, 50), 7);
    CHECK_NEAR(shifted.Area(), 650, 14);
    CHECK_EQ(shifted.At(27, 50), 0);
    // An odd count repeats: [10] is 10 on, 10 off.
    s.dash = {10};
    s.dashPhase = 0;
    CHECK_EQ(runs(Stroke(line, s, clip), 50), 5);
    // All zero is solid; zero-length dashes with round caps are dots.
    s.dash = {0, 0};
    CHECK_NEAR(Stroke(line, s, clip).Area(), 1000, 1);
    s.dash = {0, 20};
    s.cap = LineCap::Round;
    const Grid dots = Stroke(line, s, clip);
    CHECK_EQ(runs(dots, 50), 5);  // at 0, 20, ... 80: no dash starts at the very end
    CHECK_NEAR(dots.Area(), 5 * kPi * 25, 5 * kPi * 25 * 0.01);
    // A pattern far too fine to draw dash by dash is solid.
    s.dash = {1e-6f, 1e-6f};
    s.cap = LineCap::Butt;
    CHECK_NEAR(Stroke(line, s, clip).Area(), 1000, 1);
}

RGUI_TEST("gfx.stroke_transform") {
    const RectI clip{0, 0, 256, 256};
    // Width 0 is one device pixel whatever the scale.
    CHECK_NEAR(Stroke(Line(10, 20, 110, 20), Style(0, LineCap::Butt), clip, Matrix::Scale(2, 2)).Area(), 200, 1);
    // Non-uniform scale: the pen is stretched with the path.
    const Matrix m = Matrix::Scale(1, 3);
    CHECK_NEAR(Stroke(Line(20, 30, 120, 30), Style(10, LineCap::Butt), clip, m).Area(), 3000, 1);
    CHECK_NEAR(Stroke(Line(50, 5, 50, 80), Style(10, LineCap::Butt), clip, m).Area(), 10 * 225, 1);
    // NaN, infinite and absurd input draws nothing or something finite, never crashes.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(Stroke(Line(10, 10, 50, 50), Style(nan, LineCap::Round), clip).Area(), 0.0);
    CHECK_EQ(Stroke(Line(nan, 10, 50, 50), Style(4, LineCap::Round), clip).Area(), 0.0);
    Stroke(Line(-1e30f, 10, 1e30f, 50), Style(1e20f, LineCap::Round, LineJoin::Round), clip);
    Stroke(Line(10, 10, 50, 50), Style(4, LineCap::Round), clip, Matrix{nan, 0, 0, 1, 0, 0});
    StrokeStyle s = Style(2, LineCap::Butt);
    s.dash = {nan, 1};
    CHECK_NEAR(Stroke(Line(10, 10, 110, 10), s, clip).Area(), 200, 1);
}

namespace {

struct Pixels {
    std::vector<uint32_t> data;
    Surface surface;
    Pixels(int w, int h, uint32_t fill) : data(static_cast<size_t>(w) * h, fill), surface{data.data(), w, h, w} {}
    uint32_t At(int x, int y) const { return data[static_cast<size_t>(y) * surface.width + x]; }
};

} // namespace

RGUI_TEST("gfx.canvas_rect_and_mask") {
    Pixels px(16, 8, 0xFF0000FFu);  // opaque blue
    Canvas canvas(px.surface);
    canvas.FillRect({2, 1, 5, 3}, 0xFFFFFFFFu);
    CHECK_EQ(px.At(2, 1), 0xFFFFFFFFu);
    CHECK_EQ(px.At(5, 1), 0xFF0000FFu);
    canvas.FillRect({-5, -5, 1, 1}, 0x80000000u);  // clipped to (0, 0); half-transparent black
    CHECK_EQ(px.At(0, 0), 0xFF00007Fu);
    // A mask in premultiplied half red: coverage 0, 128, 255; mul255 as the kernels define it.
    const uint8_t mask[2 * 4] = {0, 128, 255, 99, /* row 1 */ 255, 255, 255, 255};
    canvas.DrawMask(mask, 4, {8, 4, 11, 6}, 0x80800000u);
    CHECK_EQ(px.At(8, 4), 0xFF0000FFu);
    CHECK_EQ(px.At(9, 4), 0xFF4000BFu);
    CHECK_EQ(px.At(10, 4), 0xFF80007Fu);
    CHECK_EQ(px.At(11, 4), 0xFF0000FFu);  // past dst: the 99 is not drawn
    CHECK_EQ(px.At(8, 5), 0xFF80007Fu);
    // Clipped by a rectangle: only the part inside is drawn, from the right mask column.
    canvas.PushClipRect({13, 0, 16, 8});
    canvas.DrawMask(mask, 4, {12, 0, 15, 1}, 0xFF00FF00u);
    CHECK_EQ(px.At(12, 0), 0xFF0000FFu);
    CHECK_EQ(px.At(13, 0), Premultiply(0, 128, 127, 255));
    CHECK_EQ(px.At(14, 0), 0xFF00FF00u);
    canvas.PopClip();
    CHECK(canvas.ClipBounds() == (RectI{0, 0, 16, 8}));
}

RGUI_TEST("gfx.canvas_gradient") {
    Pixels px(128, 2, 0xFF102030u);
    Canvas canvas(px.surface);
    auto g = std::make_shared<LinearGradient>();
    g->p0 = {10, 0};
    g->p1 = {110, 0};
    g->stops = {{0, 0, 0, 0}, {1, 255, 255, 255}};
    g->extendStart = false;
    g->extendEnd = false;
    Path rect;
    rect.AddRect(0, 0, 128, 1);
    canvas.FillPath(rect, {}, FillRule::NonZero, Paint::Gradient(g));
    CHECK_EQ(px.At(9, 0), 0xFF102030u);    // t < 0, not extended: unpainted
    CHECK_EQ(px.At(10, 0), 0xFF010101u);   // t = 0.005
    CHECK_EQ(px.At(59, 0), 0xFF7E7E7Eu);   // t = 0.495 -> 126
    CHECK_EQ(px.At(109, 0), 0xFFFEFEFEu);  // t = 0.995
    CHECK_EQ(px.At(110, 0), 0xFF102030u);  // t > 1, not extended
    // Extended, three stops, half alpha, in a scaled space: x' = 2x.
    g->extendStart = g->extendEnd = true;
    g->stops = {{0, 255, 0, 0}, {0.5f, 0, 255, 0}, {1, 0, 0, 255}};
    g->p0 = {5, 0};
    g->p1 = {55, 0};  // device 10 .. 110
    g->alpha = 128;
    Path row1;
    row1.AddRect(0, 1, 64, 1);
    canvas.FillPath(row1, Matrix::Scale(2, 1), FillRule::NonZero, Paint::Gradient(g));
    // x = 0: t clamped to 0, red at alpha 128 over 0xFF102030.
    const uint32_t keep = 127;
    auto over = [&](uint32_t r, uint32_t gg, uint32_t b) {
        return PackArgb(255, static_cast<uint8_t>(Mul255(r, 128) + Mul255(0x10, keep)),
                        static_cast<uint8_t>(Mul255(gg, 128) + Mul255(0x20, keep)),
                        static_cast<uint8_t>(Mul255(b, 128) + Mul255(0x30, keep)));
    };
    CHECK_EQ(px.At(0, 1), over(255, 0, 0));
    CHECK_EQ(px.At(120, 1), over(0, 0, 255));
    // Device 60.5 / 2 = 30.25 user: t = 0.505 -> index 129 of 255: (0, 252, 3) between green and blue.
    CHECK_EQ(px.At(60, 1), over(0, 252, 3));
    // A gradient of no length paints nothing.
    g->p1 = g->p0;
    Pixels px2(4, 4, 0xFF000000u);
    Canvas c2(px2.surface);
    Path all;
    all.AddRect(0, 0, 4, 4);
    c2.FillPath(all, {}, FillRule::NonZero, Paint::Gradient(g));
    CHECK_EQ(px2.At(1, 1), 0xFF000000u);
}

RGUI_TEST("gfx.canvas_clip_path") {
    Pixels px(32, 16, 0xFF000000u);
    Canvas canvas(px.surface);
    Path clip;
    clip.AddRect(10.5f, 0, 9.5f, 10);  // x 10.5 .. 20
    canvas.PushClipPath(clip, {}, FillRule::NonZero);
    CHECK(canvas.ClipBounds() == (RectI{10, 0, 20, 10}));
    Path big;
    big.AddRect(0, 0, 32, 16);
    canvas.FillPath(big, {}, FillRule::NonZero, Paint::Solid(0xFFFFFFFFu));
    CHECK_EQ(px.At(9, 5), 0xFF000000u);
    CHECK_EQ(px.At(10, 5), 0xFF808080u);  // half covered by the clip
    CHECK_EQ(px.At(15, 5), 0xFFFFFFFFu);
    CHECK_EQ(px.At(20, 5), 0xFF000000u);
    CHECK_EQ(px.At(15, 12), 0xFF000000u);
    // A second clip path multiplies: column 10 at 0.5 * 0.75.
    Path clip2;
    clip2.AddRect(0, 0, 10.75f, 16);
    canvas.PushClipPath(clip2, {}, FillRule::NonZero);
    CHECK(canvas.ClipBounds() == (RectI{10, 0, 11, 10}));
    canvas.FillRect({0, 0, 32, 16}, 0xFFFFFFFFu);
    CHECK_EQ(px.At(10, 1), 0xFFB0B0B0u);  // 0x80 grey under white at 96: 96 + mul255(128, 159)
    canvas.PopClip();
    // Back to the first clip; FillRect honours its mask too.
    canvas.FillRect({0, 12, 32, 16}, 0xFFFFFFFFu);  // outside its rows
    CHECK_EQ(px.At(15, 13), 0xFF000000u);
    canvas.PopClip();
    canvas.FillRect({0, 12, 32, 16}, 0xFFFFFFFFu);
    CHECK_EQ(px.At(15, 13), 0xFFFFFFFFu);
    // A clip path that covers nothing hides everything until popped.
    Path empty;
    canvas.PushClipPath(empty, {}, FillRule::NonZero);
    CHECK(canvas.ClipBounds().Empty());
    canvas.FillRect({0, 0, 32, 16}, 0xFF00FF00u);
    CHECK_EQ(px.At(0, 0), 0xFF000000u);
    canvas.PopClip();
    canvas.PopClip();  // the base clip stays
    CHECK(canvas.ClipBounds() == (RectI{0, 0, 32, 16}));
}

RGUI_TEST("gfx.canvas_image") {
    Image image;
    image.width = 13;
    image.height = 7;
    uint32_t seed = 12345;
    for (int i = 0; i < 13 * 7; ++i) {
        seed = seed * 1103515245u + 12345u;
        image.pixels.push_back(0xFF000000u | (seed >> 8));
    }
    const Matrix at{13, 0, 0, 7, 5, 3};
    for (const bool smooth : {false, true}) {
        Pixels fast(32, 16, 0xFF203040u), general(32, 16, 0xFF203040u);
        Canvas a(fast.surface), b(general.surface);
        a.DrawImage(image, at, 255, smooth);
        Path everything;
        everything.AddRect(0, 0, 32, 16);
        b.PushClipPath(everything, {}, FillRule::NonZero);  // a mask: the general path
        b.DrawImage(image, at, 255, smooth);
        CHECK(fast.data == general.data);
        CHECK_EQ(fast.At(5, 3), image.pixels[0]);
        CHECK_EQ(fast.At(17, 9), image.pixels[6 * 13 + 12]);
        CHECK_EQ(fast.At(4, 3), 0xFF203040u);
        CHECK_EQ(fast.At(18, 9), 0xFF203040u);
    }
    // Scaled 4x: nearest repeats pixels; bilinear blends them, clamping at the edges.
    Image two;
    two.width = 2;
    two.height = 1;
    two.pixels = {0xFF000000u, 0xFFFFFFFFu};
    Pixels px(8, 4, 0xFF00FF00u);
    Canvas c(px.surface);
    c.DrawImage(two, Matrix{8, 0, 0, 4, 0, 0}, 255, false);
    CHECK_EQ(px.At(3, 2), 0xFF000000u);
    CHECK_EQ(px.At(4, 2), 0xFFFFFFFFu);
    c.DrawImage(two, Matrix{8, 0, 0, 4, 0, 0}, 255, true);
    CHECK_EQ(px.At(0, 0), 0xFF000000u);  // x 0.0625 - 0.5 < 0: clamped to the first pixel
    CHECK_EQ(px.At(2, 1), 0xFF202020u);  // x 2.5 / 4 - 0.5 = 0.125: 1/8 of the white pixel
    CHECK_EQ(px.At(7, 3), 0xFFFFFFFFu);
    // Scaled 3x at an offset, nearest: every 3x3 block is its pixel (long full-coverage runs).
    {
        Pixels big(48, 30, 0xFF808080u);
        Canvas cb(big.surface);
        cb.DrawImage(image, Matrix{39, 0, 0, 21, 4, 2}, 255, false);
        bool blocks = true;
        for (int y = 0; y < 21; ++y) {
            for (int x = 0; x < 39; ++x) {
                if (big.At(4 + x, 2 + y) != image.pixels[static_cast<size_t>(y / 3) * 13 + x / 3]) blocks = false;
            }
        }
        CHECK(blocks);
        CHECK_EQ(big.At(3, 2), 0xFF808080u);
        CHECK_EQ(big.At(43, 2), 0xFF808080u);
    }
    // Half opacity over green.
    Pixels half(2, 1, 0xFF00FF00u);
    Canvas h(half.surface);
    Image white;
    white.width = white.height = 1;
    white.pixels = {0xFFFFFFFFu};
    h.DrawImage(white, Matrix{2, 0, 0, 1, 0, 0}, 128, false);
    CHECK_EQ(half.At(1, 0), PackArgb(255, 128, static_cast<uint8_t>(128 + Mul255(255, 127)), 128));
    // Rotated by 90 degrees: (u, v) -> (16 - 16v, 16u)... the image's first row lands on the right.
    Pixels rot(16, 16, 0);
    Canvas r(rot.surface);
    Image col;
    col.width = 2;
    col.height = 2;
    col.pixels = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    r.DrawImage(col, Matrix{0, 16, -16, 0, 16, 0}, 255, false);
    CHECK_EQ(rot.At(12, 3), 0xFFFF0000u);   // u small, v small
    CHECK_EQ(rot.At(12, 12), 0xFF00FF00u);  // u large, v small
    CHECK_EQ(rot.At(3, 3), 0xFF0000FFu);
    // Degenerate and broken input draws nothing and does not crash.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    r.DrawImage(col, Matrix{nan, 0, 0, 16, 0, 0}, 255, true);
    r.DrawImage(col, Matrix{0, 0, 0, 0, 3, 3}, 255, true);
    r.DrawImage(col, Matrix{1e300, 0, 0, 1e300, -1e300, 5}, 255, true);
    Image broken;
    broken.width = 100;
    broken.height = 100;  // no pixels
    r.DrawImage(broken, Matrix{16, 0, 0, 16, 0, 0}, 255, true);
    CHECK_EQ(rot.At(12, 3), 0xFFFF0000u);
}

RGUI_TEST("gfx.canvas_robust") {
    Pixels px(64, 64, 0xFF000000u);
    Canvas canvas(px.surface);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Path p;
    p.MoveTo(nan, 0);
    p.LineTo(inf, 1e38f);
    p.CubicTo(-1e38f, 3e38f, nan, nan, 10, 10);
    p.QuadTo(1e30f, -1e30f, 20, 20);
    p.Close();
    canvas.FillPath(p, {}, FillRule::EvenOdd, Paint::Solid(0xFFFFFFFFu));
    canvas.FillPath(p, Matrix{1e300, 0, 0, 1e300, 0, 0}, FillRule::NonZero, Paint::Solid(0xFFFFFFFFu));
    canvas.StrokePath(p, {}, Style(3, LineCap::Round, LineJoin::Round), Paint::Solid(0xFFFFFFFFu));
    canvas.PushClipPath(p, {}, FillRule::NonZero);
    canvas.FillRect({0, 0, 64, 64}, 0xFFFFFFFFu);
    canvas.PopClip();
    // A triangle with one vertex a billion pixels away still fills the right pixels.
    Path t;
    t.MoveTo(0, 0);
    t.LineTo(1e9f, 1e9f);
    t.LineTo(0, 1e9f);
    t.Close();
    Pixels tri(64, 64, 0xFF000000u);
    Canvas tc(tri.surface);
    tc.FillPath(t, {}, FillRule::NonZero, Paint::Solid(0xFFFFFFFFu));
    CHECK_EQ(tri.At(10, 40), 0xFFFFFFFFu);
    CHECK_EQ(tri.At(40, 10), 0xFF000000u);
    CHECK_EQ(tri.At(20, 20), 0xFF808080u);  // on the diagonal
    // A canvas without a target draws nothing.
    Canvas none;
    none.FillRect({0, 0, 10, 10}, 0xFFFFFFFFu);
    none.FillPath(t, {}, FillRule::NonZero, Paint::Solid(0xFFFFFFFFu));
    CHECK(none.ClipBounds().Empty());
}

RGUI_TEST("gfx.png_roundtrip") {
    Pixels px(7, 5, 0);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 7; ++x) {
            px.data[static_cast<size_t>(y) * 7 + x] =
                PackArgb(255, static_cast<uint8_t>(x * 36), static_cast<uint8_t>(y * 60), static_cast<uint8_t>((x + y) * 20));
        }
    }
    const std::filesystem::path file = std::filesystem::path(rgui::test::BuildDir()) / "test-gfx-roundtrip.png";
    CHECK(WritePng(file, px.surface));
    std::vector<uint8_t> bytes;
    CHECK(RayoMd::File::ReadAll(file, 1 << 20, bytes));
    std::filesystem::remove(file);
    CHECK(bytes.size() > 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
    auto u32 = [&](size_t at) {
        return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
    };
    std::vector<std::string> types;
    std::vector<uint8_t> idat;
    size_t at = 8;
    while (at + 12 <= bytes.size()) {
        const uint32_t len = u32(at);
        if (at + 12 + len > bytes.size()) break;
        const std::string type(reinterpret_cast<const char*>(&bytes[at + 4]), 4);
        types.push_back(type);
        CHECK_EQ(u32(at + 8 + len), rgui_crc32(0, &bytes[at + 4], 4 + len));
        if (type == "IHDR") {
            CHECK_EQ(len, 13u);
            CHECK_EQ(u32(at + 8), 7u);
            CHECK_EQ(u32(at + 12), 5u);
            CHECK_EQ(int(bytes[at + 16]), 8);  // bit depth
            CHECK_EQ(int(bytes[at + 17]), 2);  // RGB
            CHECK_EQ(int(bytes[at + 20]), 0);  // not interlaced
        }
        if (type == "IDAT") idat.insert(idat.end(), &bytes[at + 8], &bytes[at + 8] + len);
        at += 12 + len;
    }
    CHECK_EQ(at, bytes.size());
    CHECK(types == (std::vector<std::string>{"IHDR", "IDAT", "IEND"}));
    RayoMd::Inflate::ZlibReader reader;
    const size_t rowBytes = 1 + 3 * 7;
    CHECK(reader.Start(idat.data(), idat.size(), rowBytes * 5, rowBytes * 5));
    const uint8_t* raw = reader.Take(rowBytes * 5);
    CHECK(raw != nullptr);
    if (raw == nullptr) return;
    bool same = true;
    for (int y = 0; y < 5; ++y) {
        const uint8_t* row = raw + y * rowBytes;
        if (row[0] != 0) same = false;
        for (int x = 0; x < 7; ++x) {
            const uint32_t p = px.At(x, y);
            if (row[1 + 3 * x] != ((p >> 16) & 255) || row[2 + 3 * x] != ((p >> 8) & 255) || row[3 + 3 * x] != (p & 255)) {
                same = false;
            }
        }
    }
    CHECK(same);
    CHECK(reader.Finish());
    // The CRC itself: the standard check value of "123456789", and continuing over two calls.
    CHECK_EQ(rgui_crc32(0, "123456789", 9), 0xCBF43926u);
    CHECK_EQ(rgui_crc32(rgui_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
}

RGUI_TEST("gfx.svg_path") {
    Path p;
    CHECK(ParseSvgPath("M2.5 13V3.5H6.5L8.5 5.5H13.5V13Z", p));
    const std::vector<PointF> want = {{2.5f, 13}, {2.5f, 3.5f}, {6.5f, 3.5f}, {8.5f, 5.5f}, {13.5f, 5.5f}, {13.5f, 13}};
    CHECK_EQ(p.Points().size(), want.size());
    bool same = p.Points().size() == want.size();
    for (size_t i = 0; same && i < want.size(); ++i) {
        same = p.Points()[i].x == want[i].x && p.Points()[i].y == want[i].y;
    }
    CHECK(same);
    CHECK(p.Verbs() == (std::vector<Verb>{Verb::MoveTo, Verb::LineTo, Verb::LineTo, Verb::LineTo, Verb::LineTo,
                                          Verb::LineTo, Verb::Close}));
    // Implicit repeats, commas, exponents, signs, a new subpath after Z from its start.
    Path q;
    CHECK(ParseSvgPath("M0,0 10,0 10-10 Z H5 C1 2 3 4 5 6 7 8 9 10 11 12 M1e1 .5.5.5", q));
    CHECK_EQ(q.Verbs().size(), size_t(9));
    CHECK_EQ(q.Points().size(), size_t(1 + 2 + 1 + 6 + 2));
    if (q.Points().size() == 12) {
        CHECK_EQ(q.Points()[2].y, -10.0f);
        CHECK_EQ(q.Points()[3].x, 5.0f);  // H5 from the closed subpath's start (0, 0)
        CHECK_EQ(q.Points()[3].y, 0.0f);
        CHECK_EQ(q.Points()[10].x, 10.0f);
        CHECK_EQ(q.Points()[11].x, 0.5f);
        CHECK_EQ(q.Points()[11].y, 0.5f);
    }
    // Every icon path of the plan parses.
    for (const char* icon : {"M2.5 2.5H10.5L13.5 5.5V13.5H2.5Z M5.5 2.5V6H10V2.5 M4.5 13.5V9.5H11.5V13.5",
                             "M12 3V15 M7 10L12 15L17 10 M4 15V21H20V15",
                             "M3 6H7 M11 6H21 M7 3V9H11V3Z M3 18H13 M17 18H21 M13 15V21H17V15Z", "M6 9L12 15L18 9",
                             "M5 12.5L10 17.5L19 7", "M6 6L18 18 M18 6L6 18", "M5 5H19V19H5Z", "M5 12H19"}) {
        Path icon_path;
        CHECK(ParseSvgPath(icon, icon_path));
    }
    // Refused: arcs, relative and other commands, missing or bad numbers, not starting with M.
    for (const char* bad : {"M0 0A1 1 0 0 1 2 2", "m0 0l1 1", "M0 0l1 1", "M0 0h1", "M0 0Q1 1 2 2", "L1 1",
                            "M0", "M0 0L", "M0 0L1 x", "M0 0Z 5", "M1e999 0", "M0 0 L1 1 -", ""}) {
        Path b;
        CHECK(!ParseSvgPath(bad, b));
    }
}

RGUI_TEST("gfx.canvas_fill_matches_kernel") {
    // The canvas stores full runs and skips empty ones; the bytes must be blend_solid's.
    Path shapes;
    AddCircle(shapes, 60.3f, 50.2f, 45);
    shapes.AddRect(2.25f, 70.5f, 120, 20);
    shapes.AddRoundRect(100.5f, 5.5f, 25, 60, 8);
    for (const uint32_t color : {0xFF3366CCu, 0x80402010u}) {
        for (const FillRule rule : {FillRule::NonZero, FillRule::EvenOdd}) {
            Pixels got(128, 100, 0), want(128, 100, 0);
            for (size_t i = 0; i < got.data.size(); ++i) got.data[i] = want.data[i] = 0xFF000000u | uint32_t(i * 977);
            Canvas canvas(got.surface);
            canvas.FillPath(shapes, {}, rule, Paint::Solid(color));
            struct Reference final : SpanSink {
                Surface s;
                uint32_t color;
                void Span(int y, int x0, int w, const uint8_t* alpha) override {
                    rgui_blend_solid_scalar(s.Row(y) + x0, alpha, static_cast<size_t>(w), color);
                }
            } ref;
            ref.s = want.surface;
            ref.color = color;
            Rasterizer r;
            r.Reset({0, 0, 128, 100});
            r.AddPath(shapes, {});
            r.Sweep(rule, ref);
            CHECK(got.data == want.data);
        }
    }
}

RGUI_TEST("gfx.raster_random_triangles") {
    // Coverage sums to the area of any triangle, at any slope, within the rounding of its edge
    // pixels; a triangle and its mirror image cover the same.
    uint32_t seed = 777;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    bool areas = true, mirrors = true;
    for (int i = 0; i < 300; ++i) {
        float x[3], y[3];
        for (int k = 0; k < 3; ++k) {
            x[k] = 1 + next() * 126;
            y[k] = 1 + next() * (i % 3 == 0 ? 6 : 126);  // some flat ones
        }
        Path t, m;
        t.MoveTo(x[0], y[0]);
        t.LineTo(x[1], y[1]);
        t.LineTo(x[2], y[2]);
        t.Close();
        m.MoveTo(128 - x[0], y[0]);
        m.LineTo(128 - x[1], y[1]);
        m.LineTo(128 - x[2], y[2]);
        m.Close();
        double area = (double(x[1]) - x[0]) * (double(y[2]) - y[0]) - (double(x[2]) - x[0]) * (double(y[1]) - y[0]);
        area = (area < 0 ? -area : area) / 2;
        double perimeter = 0;
        for (int k = 0; k < 3; ++k) {
            const double dx = double(x[(k + 1) % 3]) - x[k], dy = double(y[(k + 1) % 3]) - y[k];
            perimeter += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        }
        const FillRule rule = i % 2 ? FillRule::NonZero : FillRule::EvenOdd;
        const Grid g = Fill(t, {0, 0, 128, 128}, rule);
        const Grid h = Fill(m, {0, 0, 128, 128}, rule);
        const double got = g.Area();
        if (!(got - area < 0.01 * perimeter + 0.1 && area - got < 0.01 * perimeter + 0.1)) {
            if (areas) CHECK_NEAR(got, area, 0.01 * perimeter + 0.1);
            areas = false;
        }
        if (got - h.Area() > 0.01 * perimeter + 0.1 || h.Area() - got > 0.01 * perimeter + 0.1) mirrors = false;
    }
    CHECK(areas);
    CHECK(mirrors);
}

namespace {

// Segments of a flattening, and the rasterizer fed segment by segment: no culling.
struct CountLines final : LineSink {
    Rasterizer* raster = nullptr;
    size_t lines = 0;
    void Line(PointF from, PointF to) override {
        lines++;
        if (raster) raster->AddLine(from, to);
    }
};

} // namespace

// Curves beyond one edge of the clip are flattened as chords: the same coverage, a fraction of
// the segments.
RGUI_TEST("gfx.flatten_cull") {
    Path p;
    AddCircle(p, -30, 32, 40);  // its left half lies wholly left of the clip, its cover still counts
    for (int i = 0; i < 1000; ++i) {
        // Far right, above and below: wiggly curves that would take 256 pieces each.
        const float o = static_cast<float>(i % 50);
        p.MoveTo(100 + o, 10);
        p.CubicTo(5000, -3000, 3000, 9000, 100 + o, 50);
        p.Close();
        p.MoveTo(10 + o, -100);
        p.CubicTo(4000, -5000, -4000, -6000, 20 + o, -120);
        p.Close();
    }
    const RectI clip{0, 0, 64, 64};
    WorkBudget budget;
    Rasterizer culled;
    culled.SetBudget(&budget);
    culled.Reset(clip);
    culled.AddPath(p, {});
    Grid a(clip);
    culled.Sweep(FillRule::NonZero, a);
    const uint64_t used = WorkBudget::kUnlimited - budget.geometry;

    Rasterizer plain;
    plain.Reset(clip);
    CountLines all;
    all.raster = &plain;
    Flatten(p, {}, kFlattenTolerance, all);
    Grid b(clip);
    plain.Sweep(FillRule::NonZero, b);

    int worst = 0;
    for (size_t i = 0; i < a.a.size(); ++i) worst = std::max(worst, std::abs(int(a.a[i]) - int(b.a[i])));
    CHECK(worst <= 1);
    CHECK(a.At(5, 32) == 255);  // inside the circle, right of the clip's edge
    CHECK(all.lines > 400000);
    CHECK(used < 5000);
}

// A stroke leaves out what lies outside the clip: polygons, curves and dashes there, with the
// same pixels inside.
RGUI_TEST("gfx.stroke_cull") {
    Path p;
    p.MoveTo(10, 50);
    p.LineTo(50000, 50);
    p.LineTo(50000, 60);
    p.CubicTo(90000, 60, 90000, 90, 50000, 90);
    p.LineTo(10, 90);
    p.LineTo(10, 70);
    StrokeStyle style;
    style.width = 4;
    style.dash = {3, 2};
    style.dashPhase = 1;
    style.join = LineJoin::Round;
    style.cap = LineCap::Round;

    Pixels culled(100, 100, 0xFFFFFFFFu);
    WorkBudget budget;
    {
        Canvas canvas(culled.surface);
        canvas.SetBudget(&budget);
        canvas.StrokePath(p, {}, style, Paint::Solid(0xFF000000u));
    }
    Pixels whole(100, 100, 0xFFFFFFFFu);
    {
        Path outline;
        StrokeOutline(p, {}, style, outline);
        Canvas canvas(whole.surface);
        canvas.FillPath(outline, {}, FillRule::NonZero, Paint::Solid(0xFF000000u));
    }
    int worst = 0;
    size_t dark = 0;
    for (size_t i = 0; i < culled.data.size(); ++i) {
        worst = std::max(worst, std::abs(int(culled.data[i] & 255) - int(whole.data[i] & 255)));
        dark += (culled.data[i] & 255) < 128;
    }
    CHECK(worst <= 2);
    CHECK(dark > 500);
    // The two long runs make about 20,000 dashes each; only those near the clip are made.
    CHECK(WorkBudget::kUnlimited - budget.dashes < 200);
    CHECK(WorkBudget::kUnlimited - budget.geometry < 20000);
}

// A clip set again in place multiplies into the top clip's own mask; the result is the
// pushed one's.
RGUI_TEST("gfx.canvas_clip_in_place") {
    Path a, b, square;
    a.AddRect(4.5f, 2.25f, 20, 20);
    b.AddRect(10.25f, 6.5f, 18, 20);
    square.AddRect(0, 0, 32, 32);
    Pixels pushed(32, 32, 0xFF000000u), merged(32, 32, 0xFF000000u);
    {
        Canvas canvas(pushed.surface);
        canvas.PushClipRect({1, 1, 31, 31});
        canvas.PushClipPath(a, {}, FillRule::NonZero);
        canvas.PushClipPath(b, {}, FillRule::NonZero);
        canvas.FillPath(square, {}, FillRule::NonZero, Paint::Solid(0xFFFFFFFFu));
    }
    {
        Canvas canvas(merged.surface);
        canvas.PushClipRect({1, 1, 31, 31});
        canvas.PushClipPath(a, {}, FillRule::NonZero);
        const size_t one = canvas.MaskBytes();
        for (int i = 0; i < 100; ++i) canvas.IntersectClipPath(b, {}, FillRule::NonZero);  // b * b * ... rounds alike
        CHECK_EQ(canvas.ClipDepth(), size_t(2));
        CHECK(canvas.MaskBytes() <= one);
        canvas.IntersectClipRect({0, 0, 32, 32});
        canvas.FillPath(square, {}, FillRule::NonZero, Paint::Solid(0xFFFFFFFFu));
        canvas.PopClip();
        CHECK(canvas.ClipBounds() == (RectI{1, 1, 31, 31}));
    }
    // Full coverage of b multiplies to itself; the partial edge rows of b compound, so compare
    // where b covers fully or not at all.
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            if (y == 6 || y == 26 || x == 10 || x == 28) continue;
            CHECK_EQ(pushed.At(x, y), merged.At(x, y));
        }
    }
}

// Once the budget is spent the canvas draws nothing more.
RGUI_TEST("gfx.canvas_budget") {
    Pixels px(64, 64, 0xFF000000u);
    Canvas canvas(px.surface);
    WorkBudget budget;
    budget.pixels = 64 * 64 + 10;
    canvas.SetBudget(&budget);
    canvas.FillRect({0, 0, 64, 64}, 0xFF0000FFu);
    CHECK(!budget.Stopped());
    canvas.FillRect({0, 0, 64, 64}, 0xFFFF0000u);
    CHECK(budget.stop == WorkBudget::Stop::Pixels);
    CHECK_EQ(px.At(5, 5), 0xFF0000FFu);
    Path p;
    AddCircle(p, 32, 32, 20);
    canvas.FillPath(p, {}, FillRule::NonZero, Paint::Solid(0xFF00FF00u));
    CHECK_EQ(px.At(32, 32), 0xFF0000FFu);
    // A cancel flag stops it too.
    std::atomic<bool> cancel{true};
    WorkBudget cancelled;
    cancelled.cancel = &cancel;
    CHECK(!cancelled.Poll());
    CHECK(cancelled.stop == WorkBudget::Stop::Cancelled);
    // The rasterizer's cells stay clean after a sweep cut short: the next one is exact.
    Rasterizer r;
    WorkBudget tight;
    tight.pixels = 100;
    r.SetBudget(&tight);
    r.Reset({0, 0, 64, 64});
    r.AddPath(p, {});
    Grid cut({0, 0, 64, 64});
    r.Sweep(FillRule::NonZero, cut);
    CHECK(tight.Stopped());
    r.SetBudget(nullptr);
    r.Reset({0, 0, 64, 64});
    r.AddPath(p, {});
    Grid again({0, 0, 64, 64});
    r.Sweep(FillRule::NonZero, again);
    CHECK(again.a == Fill(p, {0, 0, 64, 64}, FillRule::NonZero).a);
}
