// SPDX-License-Identifier: Apache-2.0
// gfx.*: paths, the rasterizer, the stroker, the canvas, the PNG writer and icon paths.
#include "runner.h"

#include "gfx/path.h"
#include "gfx/raster.h"

#include <cstdint>
#include <limits>
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
