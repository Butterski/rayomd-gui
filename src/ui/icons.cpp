// SPDX-License-Identifier: Apache-2.0
// Icon paths and stroke widths copied from Omawrite (src/FooterIconButton.qml, open and save, at
// a7da0b14e501d6aab5b67258564052727ae1d1d9) and Hype (src/AppIcon.qml, the rest, at
// d5f1a3ce6a6c924e4e07e3390c7b28b7120da59d), both MIT, "Copyright (c) 2026 David Heinemeier
// Hansson"; see third_party/omarchy/. Close, maximize, minimize and insert are ours.
#include "ui/icons.h"

#include "gfx/raster.h"
#include "gfx/stroke.h"
#include "gfx/svgpath.h"

#include <cstring>

namespace rgui::ui {

namespace {

const IconInfo kIcons[kIconCount] = {
    {"open", 16, 1.4f, "M2.5 13V3.5H6.5L8.5 5.5H13.5V13Z"},
    {"save", 16, 1.4f, "M2.5 2.5H10.5L13.5 5.5V13.5H2.5Z M5.5 2.5V6H10V2.5 M4.5 13.5V9.5H11.5V13.5"},
    {"export", 24, 1.8f, "M12 3V15 M7 10L12 15L17 10 M4 15V21H20V15"},
    // Hype's "adjust".
    {"options", 24, 1.8f, "M3 6H7 M11 6H21 M7 3V9H11V3Z M3 18H13 M17 18H21 M13 15V21H17V15Z"},
    {"chevron-down", 24, 1.8f, "M6 9L12 15L18 9"},
    {"check", 24, 1.8f, "M5 12.5L10 17.5L19 7"},
    {"close", 24, 1.8f, "M6 6L18 18 M18 6L6 18"},
    {"maximize", 24, 1.8f, "M5 5H19V19H5Z"},
    {"minimize", 24, 1.8f, "M5 12H19"},
    // A plus in a box: the insert list.
    {"insert", 24, 1.8f, "M4.5 4.5H19.5V19.5H4.5Z M12 8V16 M8 12H16"},
};

class MaskSink final : public gfx::SpanSink {
public:
    explicit MaskSink(IconMask& mask) : mask_(mask) {}
    void Span(int y, int x0, int width, const uint8_t* alpha) override {
        if (y < 0 || y >= mask_.size || x0 < 0 || x0 + width > mask_.size) return;
        std::memcpy(mask_.alpha.data() + static_cast<size_t>(y) * static_cast<size_t>(mask_.size) +
                        static_cast<size_t>(x0),
                    alpha, static_cast<size_t>(width));
    }

private:
    IconMask& mask_;
};

} // namespace

const IconInfo& IconData(Icon icon) {
    const size_t i = static_cast<size_t>(icon);
    return kIcons[i < static_cast<size_t>(kIconCount) ? i : 0];
}

bool RasterizeIcon(Icon icon, int sizePx, IconMask& out) {
    out = IconMask();
    if (sizePx < 1 || sizePx > 512) return false;
    const IconInfo& info = IconData(icon);
    gfx::Path path;
    if (!gfx::ParseSvgPath(info.path, path)) return false;
    const double s = static_cast<double>(sizePx) / info.grid;
    const gfx::Matrix m = gfx::Matrix::Scale(s, s);
    gfx::StrokeStyle style;
    style.width = info.stroke;
    style.cap = gfx::LineCap::Round;
    style.join = gfx::LineJoin::Round;
    gfx::Path outline;
    gfx::StrokeOutline(path, m, style, outline);
    out.size = sizePx;
    out.alpha.assign(static_cast<size_t>(sizePx) * static_cast<size_t>(sizePx), 0);
    gfx::Rasterizer rasterizer;
    rasterizer.Reset({0, 0, sizePx, sizePx});
    rasterizer.AddPath(outline, m);
    MaskSink sink(out);
    rasterizer.Sweep(gfx::FillRule::NonZero, sink);
    return true;
}

const IconMask* IconCache::Get(Icon icon, int sizePx) {
    if (sizePx < 1 || sizePx > 512) return nullptr;
    const uint32_t key = Key(icon, sizePx);
    const auto found = masks_.find(key);
    if (found != masks_.end()) return &found->second;
    IconMask mask;
    RasterizeIcon(icon, sizePx, mask);
    return &masks_.emplace(key, std::move(mask)).first->second;
}

const IconMask* IconCache::Find(Icon icon, int sizePx) const {
    const auto found = masks_.find(Key(icon, sizePx));
    return found != masks_.end() ? &found->second : nullptr;
}

} // namespace rgui::ui
