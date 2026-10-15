// SPDX-License-Identifier: Apache-2.0
// Points, rectangles and affine matrices.
#pragma once

#include <algorithm>

namespace rgui::gfx {

struct PointF {
    float x = 0;
    float y = 0;
};

// Integer device rectangle, half-open: [x0, x1) x [y0, y1).
struct RectI {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    int Width() const { return x1 - x0; }
    int Height() const { return y1 - y0; }
    bool Empty() const { return x1 <= x0 || y1 <= y0; }
    bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    RectI Intersect(const RectI& o) const {
        return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    }
    // The smallest rectangle holding both; an empty one counts as nothing.
    RectI Union(const RectI& o) const {
        if (Empty()) return o;
        if (o.Empty()) return *this;
        return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
    }
    bool operator==(const RectI& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
    bool operator!=(const RectI& o) const { return !(*this == o); }
};

struct RectF {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;

    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// An affine transform in PDF's order: x' = a*x + c*y + e, y' = b*x + d*y + f.
struct Matrix {
    double a = 1;
    double b = 0;
    double c = 0;
    double d = 1;
    double e = 0;
    double f = 0;

    static Matrix Translate(double tx, double ty) { return {1, 0, 0, 1, tx, ty}; }
    static Matrix Scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }

    // This transform followed by `then` (PDF's "this x then"; `cm` makes M.Multiply(ctm)).
    Matrix Multiply(const Matrix& then) const {
        return {a * then.a + b * then.c, a * then.b + b * then.d,
                c * then.a + d * then.c, c * then.b + d * then.d,
                e * then.a + f * then.c + then.e, e * then.b + f * then.d + then.f};
    }
    // False, and `out` unchanged, when |det| < 1e-12.
    bool Invert(Matrix& out) const {
        const double det = a * d - b * c;
        if (!(det > 1e-12 || det < -1e-12)) return false;
        const double inv = 1.0 / det;
        out = {d * inv, -b * inv, -c * inv, a * inv, (c * f - d * e) * inv, (b * e - a * f) * inv};
        return true;
    }
    PointF Apply(PointF p) const {
        return {static_cast<float>(a * p.x + c * p.y + e), static_cast<float>(b * p.x + d * p.y + f)};
    }
    void Apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    bool IsIdentity() const { return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0; }
};

} // namespace rgui::gfx
