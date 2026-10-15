// SPDX-License-Identifier: Apache-2.0
// A frame's drawing as a list of commands in device pixels, each with the area it may touch and
// a hash of everything that decides its pixels, so the compositor redraws only what changed.
#pragma once

#include "gfx/geometry.h"
#include "gfx/surface.h"
#include "text/glyphsource.h"
#include "ui/icons.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rgui::ui {

// FNV-1a, 64 bits.
inline constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
inline constexpr uint64_t kFnvPrime = 0x100000001b3ull;
inline uint64_t Fnv1a64(const void* data, size_t size, uint64_t hash = kFnvOffset) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) hash = (hash ^ p[i]) * kFnvPrime;
    return hash;
}
inline uint64_t Fnv1a64(std::string_view text, uint64_t hash = kFnvOffset) {
    return Fnv1a64(text.data(), text.size(), hash);
}

enum class CommandType : uint8_t {
    Rect,             // rect (whole pixels), color
    RectStroke,       // rect (whole pixels), width (whole pixels, inside the rect), color
    RoundRect,        // rect, radius, color
    RoundRectStroke,  // rect, radius, width (inside the rect), color
    Glyphs,           // glyphs [glyphBegin, glyphBegin + glyphCount) of the arena, size, color
    Image,            // image, rect (its destination), opacity; smooth sampling
    Icon,             // icon, rect (a square of whole pixels), color
    ClipPush,         // rect: the new clip is the current one intersected with it
    ClipPop,
};

// One glyph: its font, glyph id, and pen position in device pixels (x may be fractional; the
// glyph cache keys the quarter-pixel phase; y is the baseline, rounded to whole pixels when
// drawn).
struct GlyphInstance {
    const text::GlyphSource* source = nullptr;
    uint16_t glyph = 0;
    float x = 0;
    float y = 0;
};

struct Command {
    CommandType type = CommandType::Rect;
    Icon icon = Icon::Open;
    uint8_t opacity = 255;
    uint32_t color = 0;             // premultiplied 0xAARRGGBB
    gfx::RectF rect;                // device pixels
    float radius = 0;               // device pixels
    float width = 0;                // stroke width, device pixels
    float size = 0;                 // Glyphs: pixels per em
    uint32_t glyphBegin = 0;
    uint32_t glyphCount = 0;
    const gfx::Image* image = nullptr;
    gfx::RectI clip;                // the clip in force when the command was added
    gfx::RectI bounds;              // the device pixels it may touch, within `clip`
    uint64_t hash = 0;              // of type, geometry, colour, clip and data
};

// Built on the UI thread each frame; the compositor reads it. Commands outside the current clip
// are dropped when added; ClipPush and ClipPop are always kept, so pushes and pops match.
class DisplayList {
public:
    // Empties the list for a frame of `width` x `height` device pixels at `scale` device pixels
    // per logical pixel; the clip is the whole frame.
    void Reset(int width, int height, double scale);

    void FillRect(gfx::RectI rect, uint32_t color);
    // A border `width` pixels wide inside `rect`.
    void StrokeRect(gfx::RectI rect, int width, uint32_t color);
    void FillRoundRect(gfx::RectF rect, float radius, uint32_t color);
    // A border `width` pixels wide inside `rect`, its outer corners of `radius`.
    void StrokeRoundRect(gfx::RectF rect, float radius, float width, uint32_t color);
    // Glyphs at `sizePx` pixels per em. Their bounds are `ink` when given (device pixels), else
    // an estimate from the pen positions: 1.25 em above to 0.5 em below the baselines, 0.3 em
    // left of the first pen to 1.3 em right of the last.
    void Glyphs(const GlyphInstance* glyphs, size_t count, float sizePx, uint32_t color,
                const gfx::RectI* ink = nullptr);
    // The image (which must stay alive and unchanged until the frame is composited) scaled into
    // `dst`. Its hash covers image->id and image->version, not the pixels.
    void DrawImage(const gfx::Image& image, gfx::RectF dst, uint8_t opacity);
    // The icon's mask at rect.Width() pixels, its top left at rect's.
    void DrawIcon(Icon icon, gfx::RectI rect, uint32_t color);

    void PushClip(gfx::RectI rect);
    void PopClip();   // ignored at the root
    gfx::RectI CurrentClip() const { return clips_.back(); }

    // Appends `other`'s commands (a layer drawn above), unchanged, glyph ranges shifted.
    void Append(const DisplayList& other);

    const std::vector<Command>& Commands() const { return commands_; }
    const std::vector<GlyphInstance>& GlyphArena() const { return glyphs_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    double Scale() const { return scale_; }

private:
    void Add(Command& command, gfx::RectI rawBounds);

    std::vector<Command> commands_;
    std::vector<GlyphInstance> glyphs_;
    std::vector<gfx::RectI> clips_{gfx::RectI{}};
    int width_ = 0;
    int height_ = 0;
    double scale_ = 1.0;
};

} // namespace rgui::ui
