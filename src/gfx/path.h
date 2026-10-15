// SPDX-License-Identifier: Apache-2.0
// Paths of lines and Bézier curves, and their flattening into device-space line segments.
#pragma once

#include "gfx/geometry.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rgui::gfx {

enum class Verb : uint8_t {
    MoveTo,   // 1 point
    LineTo,   // 1 point
    QuadTo,   // 2 points: control, end
    CubicTo,  // 3 points: control 1, control 2, end
    Close,    // 0 points
};

class Path {
public:
    void MoveTo(float x, float y);
    void LineTo(float x, float y);
    void QuadTo(float cx, float cy, float x, float y);
    void CubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    void Close();

    // A closed rectangle, and one with corners of radius r (cubics with 0.5522847498).
    void AddRect(float x, float y, float w, float h);
    void AddRoundRect(float x, float y, float w, float h, float r);

    void Clear();
    bool Empty() const { return verbs_.empty(); }
    const std::vector<Verb>& Verbs() const { return verbs_; }
    const std::vector<PointF>& Points() const { return points_; }

    // The bounds of every point, control points included; {0,0,0,0} for an empty path.
    RectF Bounds() const;
    Path Transformed(const Matrix& m) const;

private:
    std::vector<Verb> verbs_;
    std::vector<PointF> points_;
};

// Receives device-space line segments.
class LineSink {
public:
    virtual void Line(PointF from, PointF to) = 0;

protected:
    ~LineSink() = default;
};

// The default flattening tolerance, in device pixels.
inline constexpr float kFlattenTolerance = 0.2f;

// Emits `path` transformed by `m` as line segments, closing every subpath (a fill's implicit
// close). A quadratic is cut into n = ceil(sqrt(|p0 - 2p1 + p2| / (4 tol))) pieces, a cubic into
// n = ceil(sqrt(0.75 * max(|p0 - 2p1 + p2|, |p1 - 2p2 + p3|) / tol)), n clamped to [1, 256], the
// control points measured after the transform.
void Flatten(const Path& path, const Matrix& m, float tolerance, LineSink& sink);

// Receives flattened subpaths whole: `count` device-space points, `closed` when the subpath ended
// with Close. A lone MoveTo arrives as one open point.
class PolylineSink {
public:
    virtual void Polyline(const PointF* points, size_t count, bool closed) = 0;

protected:
    ~PolylineSink() = default;
};

// As Flatten, but each subpath is handed over as a polyline and open ones stay open (for
// stroking). `scratch` holds the current subpath's points; pass the same vector to reuse it.
void FlattenPolylines(const Path& path, const Matrix& m, float tolerance, std::vector<PointF>& scratch,
                      PolylineSink& sink);

} // namespace rgui::gfx
