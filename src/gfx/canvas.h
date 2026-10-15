// SPDX-License-Identifier: Apache-2.0
// Drawing into a Surface: rectangles, paths, strokes, images and alpha masks, under a stack of
// clips. Colours are premultiplied 0xAARRGGBB (surface.h).
#pragma once

#include "gfx/geometry.h"
#include "gfx/path.h"
#include "gfx/raster.h"
#include "gfx/stroke.h"
#include "gfx/surface.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace rgui::gfx {

struct GradientStop {
    float offset = 0;  // 0..1, ascending
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// An axial gradient in the space of the path it fills (before the path's matrix): a pixel takes
// t = ((p - p0) . (p1 - p0)) / |p1 - p0|^2, p its centre mapped back through the inverse matrix.
// Below 0 or above 1, t is clamped where extendStart / extendEnd is set and the pixel is left
// unpainted where it is not. The colour is interpolated in RGB, then premultiplied with alpha.
struct LinearGradient {
    PointF p0;
    PointF p1;
    std::vector<GradientStop> stops;
    bool extendStart = true;
    bool extendEnd = true;
    uint8_t alpha = 255;
};

// A solid premultiplied colour, or a gradient when `gradient` is set.
struct Paint {
    uint32_t color = 0xFF000000u;
    std::shared_ptr<const LinearGradient> gradient;

    static Paint Solid(uint32_t premultiplied) { return Paint{premultiplied, nullptr}; }
    static Paint Gradient(std::shared_ptr<const LinearGradient> g) { return Paint{0, std::move(g)}; }
};

class Canvas {
public:
    Canvas();
    explicit Canvas(const Surface& target);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // Draws into `target` from now on, the clip stack reset to the whole surface.
    void SetTarget(const Surface& target);
    const Surface& Target() const;

    // An opaque colour is stored; otherwise blended with full coverage.
    void FillRect(RectI rect, uint32_t color);
    void FillPath(const Path& path, const Matrix& m, FillRule rule, const Paint& paint);
    void StrokePath(const Path& path, const Matrix& m, const StrokeStyle& style, const Paint& paint);

    // The image fills the unit square mapped by `unitSquareToDevice`: the square's point (u, v)
    // is image pixel position (u * width, v * height), v = 0 at the image's first row. Each
    // covered pixel samples the image at its centre mapped back (bilinear with edge clamping when
    // `smooth`, else nearest), times `opacity`. An opaque image at an integer translation and
    // scale 1, with no clip mask, is copied row by row.
    void DrawImage(const Image& image, const Matrix& unitSquareToDevice, uint8_t opacity, bool smooth);

    // An 8-bit coverage mask (glyphs, icons) in `color`: alpha row r starts at alpha + r * stride
    // and covers dst.y0 + r; dst's size is the mask's.
    void DrawMask(const uint8_t* alpha, int stride, RectI dst, uint32_t color);

    // Clips: a rectangle intersects the current clip; a path is rasterized into an 8-bit mask
    // over its bounds within the current clip, times any mask already there. PopClip restores
    // what was before the matching push.
    void PushClipRect(RectI rect);
    void PushClipPath(const Path& path, const Matrix& m, FillRule rule);
    void PopClip();
    RectI ClipBounds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rgui::gfx
