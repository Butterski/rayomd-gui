// SPDX-License-Identifier: Apache-2.0
// Stroking: a path's outline as closed polygons, to be filled NonZero.
#pragma once

#include "gfx/geometry.h"
#include "gfx/path.h"

#include <cstdint>
#include <vector>

namespace rgui::gfx {

enum class LineCap : uint8_t { Butt, Round, Square };
enum class LineJoin : uint8_t { Miter, Round, Bevel };

struct StrokeStyle {
    float width = 1;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    float miterLimit = 10;
    std::vector<float> dash;   // empty or all zero: solid
    float dashPhase = 0;
};

// The outline of `path` stroked with `style`, in the path's own (user) space: fill it with `m`
// and FillRule::NonZero. `m` sets the flattening tolerance (0.2 / s, s = sqrt|det m|, at least
// 1e-6), the steps of round joins and caps, and the width of a zero-width stroke (1 device
// pixel). Every segment, join and cap is its own closed polygon; a miter join stays a miter
// while 1 / sin(theta / 2) <= miterLimit, else it is bevelled. A zero-length subpath with a
// round or square cap is a dot or a square.
void StrokeOutline(const Path& path, const Matrix& m, const StrokeStyle& style, Path& outline);

} // namespace rgui::gfx
