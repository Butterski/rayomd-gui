// SPDX-License-Identifier: Apache-2.0
// Paths and their flattening into line segments (plan Phase 4 step 2).
#include "gfx/path.h"

#include "common/exact_math.h"

namespace rgui::gfx {

namespace ExactMath = RayoMd::ExactMath;

void Path::MoveTo(float x, float y) {
    verbs_.push_back(Verb::MoveTo);
    points_.push_back({x, y});
}

void Path::LineTo(float x, float y) {
    verbs_.push_back(Verb::LineTo);
    points_.push_back({x, y});
}

void Path::QuadTo(float cx, float cy, float x, float y) {
    verbs_.push_back(Verb::QuadTo);
    points_.push_back({cx, cy});
    points_.push_back({x, y});
}

void Path::CubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    verbs_.push_back(Verb::CubicTo);
    points_.push_back({c1x, c1y});
    points_.push_back({c2x, c2y});
    points_.push_back({x, y});
}

void Path::Close() {
    verbs_.push_back(Verb::Close);
}

void Path::AddRect(float x, float y, float w, float h) {
    MoveTo(x, y);
    LineTo(x + w, y);
    LineTo(x + w, y + h);
    LineTo(x, y + h);
    Close();
}

void Path::AddRoundRect(float x, float y, float w, float h, float r) {
    const float half = (w < h ? w : h) * 0.5f;
    if (r > half) r = half;
    if (!(r > 0)) {
        AddRect(x, y, w, h);
        return;
    }
    const float k = r * (1.0f - 0.5522847498f);  // control points sit r * 0.5523 from the corner's ends
    const float x1 = x + w;
    const float y1 = y + h;
    MoveTo(x + r, y);
    LineTo(x1 - r, y);
    CubicTo(x1 - k, y, x1, y + k, x1, y + r);
    LineTo(x1, y1 - r);
    CubicTo(x1, y1 - k, x1 - k, y1, x1 - r, y1);
    LineTo(x + r, y1);
    CubicTo(x + k, y1, x, y1 - k, x, y1 - r);
    LineTo(x, y + r);
    CubicTo(x, y + k, x + k, y, x + r, y);
    Close();
}

void Path::Clear() {
    verbs_.clear();
    points_.clear();
}

RectF Path::Bounds() const {
    if (points_.empty()) return {};
    float x0 = points_[0].x, y0 = points_[0].y, x1 = x0, y1 = y0;
    for (const PointF& p : points_) {
        x0 = p.x < x0 ? p.x : x0;
        y0 = p.y < y0 ? p.y : y0;
        x1 = p.x > x1 ? p.x : x1;
        y1 = p.y > y1 ? p.y : y1;
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

Path Path::Transformed(const Matrix& m) const {
    Path out;
    out.verbs_ = verbs_;
    out.points_.reserve(points_.size());
    for (const PointF& p : points_) out.points_.push_back(m.Apply(p));
    return out;
}

namespace {

struct Pt {
    double x;
    double y;
};

inline Pt Transform(const Matrix& m, PointF p) {
    return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}

// Device coordinates as floats, kept finite where they can be (NaN stays NaN and is dropped later).
inline float ToFloat(double v) {
    if (v > 1e30) return 1e30f;
    if (v < -1e30) return -1e30f;
    return static_cast<float>(v);
}

inline double Length(double x, double y) {
    return ExactMath::Sqrt(x * x + y * y);
}

// ceil(q) clamped to [1, 256]; NaN gives 1.
inline int Pieces(double q) {
    if (!(q > 1)) return 1;
    if (q >= 256) return 256;
    return static_cast<int>(ExactMath::Ceil(q));
}

// True when the n points, mapped by cull.toDevice, all lie beyond one edge of cull's rectangle.
bool Outside(const FlattenCull& cull, const Pt* p, int n) {
    bool left = true, right = true, above = true, below = true;
    for (int i = 0; i < n; ++i) {
        const Pt d = {cull.toDevice.a * p[i].x + cull.toDevice.c * p[i].y + cull.toDevice.e,
                      cull.toDevice.b * p[i].x + cull.toDevice.d * p[i].y + cull.toDevice.f};
        left = left && d.x < cull.x0;
        right = right && d.x > cull.x1;
        above = above && d.y < cull.y0;
        below = below && d.y > cull.y1;
    }
    return left || right || above || below;
}

// Walks `path` under `m`, handing the visitor device points: Begin(p) starts a subpath, To(p)
// continues it, End(closed) finishes it.
template <typename Visitor>
void Walk(const Path& path, const Matrix& m, float tolerance, Visitor& visitor, const FlattenCull* cull) {
    const double tol = tolerance > 1e-6f ? tolerance : 1e-6;
    const std::vector<Verb>& verbs = path.Verbs();
    const std::vector<PointF>& points = path.Points();
    size_t pi = 0;
    bool open = false;
    Pt start = Transform(m, {0, 0});
    Pt cur = start;
    auto ensureOpen = [&]() {
        if (!open) {
            visitor.Begin(cur);
            start = cur;
            open = true;
        }
    };
    for (const Verb verb : verbs) {
        switch (verb) {
        case Verb::MoveTo:
            if (pi + 1 > points.size()) return;
            if (open) visitor.End(false);
            cur = start = Transform(m, points[pi]);
            pi += 1;
            visitor.Begin(cur);
            open = true;
            break;
        case Verb::LineTo:
            if (pi + 1 > points.size()) return;
            ensureOpen();
            cur = Transform(m, points[pi]);
            pi += 1;
            visitor.To(cur);
            break;
        case Verb::QuadTo: {
            if (pi + 2 > points.size()) return;
            ensureOpen();
            const Pt p0 = cur;
            const Pt p1 = Transform(m, points[pi]);
            const Pt p2 = Transform(m, points[pi + 1]);
            pi += 2;
            const Pt control[3] = {p0, p1, p2};
            if (cull && Outside(*cull, control, 3)) {
                cur = p2;
                visitor.To(cur);
                break;
            }
            const double dd = Length(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y);
            const int n = Pieces(ExactMath::Sqrt(dd / (4 * tol)));
            for (int i = 1; i < n; ++i) {
                const double t = static_cast<double>(i) / n;
                const double u = 1 - t;
                const double w0 = u * u, w1 = 2 * u * t, w2 = t * t;
                visitor.To({w0 * p0.x + w1 * p1.x + w2 * p2.x, w0 * p0.y + w1 * p1.y + w2 * p2.y});
            }
            cur = p2;
            visitor.To(cur);
            break;
        }
        case Verb::CubicTo: {
            if (pi + 3 > points.size()) return;
            ensureOpen();
            const Pt p0 = cur;
            const Pt p1 = Transform(m, points[pi]);
            const Pt p2 = Transform(m, points[pi + 1]);
            const Pt p3 = Transform(m, points[pi + 2]);
            pi += 3;
            const Pt control[4] = {p0, p1, p2, p3};
            if (cull && Outside(*cull, control, 4)) {
                cur = p3;
                visitor.To(cur);
                break;
            }
            const double d1 = Length(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y);
            const double d2 = Length(p1.x - 2 * p2.x + p3.x, p1.y - 2 * p2.y + p3.y);
            const int n = Pieces(ExactMath::Sqrt(0.75 * (d1 > d2 ? d1 : d2) / tol));
            for (int i = 1; i < n; ++i) {
                const double t = static_cast<double>(i) / n;
                const double u = 1 - t;
                const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
                visitor.To({w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                            w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y});
            }
            cur = p3;
            visitor.To(cur);
            break;
        }
        case Verb::Close:
            if (open) {
                visitor.End(true);
                open = false;
            }
            cur = start;
            break;
        }
    }
    if (open) visitor.End(false);
}

struct LineVisitor {
    LineSink& sink;
    PointF first{};
    PointF last{};

    void Begin(Pt p) { first = last = {ToFloat(p.x), ToFloat(p.y)}; }
    void To(Pt p) {
        const PointF q{ToFloat(p.x), ToFloat(p.y)};
        sink.Line(last, q);
        last = q;
    }
    void End(bool) {
        if (last.x != first.x || last.y != first.y) sink.Line(last, first);
    }
};

struct PolylineVisitor {
    PolylineSink& sink;
    std::vector<PointF>& points;

    void Begin(Pt p) {
        points.clear();
        points.push_back({ToFloat(p.x), ToFloat(p.y)});
    }
    void To(Pt p) { points.push_back({ToFloat(p.x), ToFloat(p.y)}); }
    void End(bool closed) { sink.Polyline(points.data(), points.size(), closed); }
};

} // namespace

void Flatten(const Path& path, const Matrix& m, float tolerance, LineSink& sink, const FlattenCull* cull) {
    LineVisitor visitor{sink};
    Walk(path, m, tolerance, visitor, cull);
}

void FlattenPolylines(const Path& path, const Matrix& m, float tolerance, std::vector<PointF>& scratch,
                      PolylineSink& sink, const FlattenCull* cull) {
    PolylineVisitor visitor{sink, scratch};
    Walk(path, m, tolerance, visitor, cull);
}

} // namespace rgui::gfx
