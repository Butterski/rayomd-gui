// SPDX-License-Identifier: Apache-2.0
// The interface icons: stroked SVG paths from Omawrite and Hype (MIT, third_party/omarchy/), and
// three window-control icons of ours, rasterized into alpha masks per device size.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rgui::ui {

enum class Icon : uint8_t { Open, Save, Export, Options, ChevronDown, Check, Close, Maximize, Minimize };
inline constexpr int kIconCount = 9;

// An icon's source: the path in a `grid` x `grid` box, stroked `stroke` grid units wide with
// round caps and joins.
struct IconInfo {
    const char* name;
    int grid;
    float stroke;
    const char* path;
};

const IconInfo& IconData(Icon icon);

// A square coverage mask, `size` x `size` bytes, top row first.
struct IconMask {
    int size = 0;
    std::vector<uint8_t> alpha;
};

// The icon stroked and rasterized at `sizePx` device pixels square (the grid scaled to it).
// False, with `out` empty, when its path does not parse or the size is outside 1..512.
bool RasterizeIcon(Icon icon, int sizePx, IconMask& out);

// Masks by (icon, device size). Get fills on a miss (the UI thread, in the compositor's prepare
// pass); Find only reads, so tile threads may call it while nothing fills. Pointers stay valid
// until Clear.
class IconCache {
public:
    const IconMask* Get(Icon icon, int sizePx);
    const IconMask* Find(Icon icon, int sizePx) const;
    void Clear() { masks_.clear(); }
    size_t Count() const { return masks_.size(); }

private:
    static uint32_t Key(Icon icon, int sizePx) {
        return (static_cast<uint32_t>(icon) << 16) | (static_cast<uint32_t>(sizePx) & 0xFFFFu);
    }
    std::unordered_map<uint32_t, IconMask> masks_;
};

} // namespace rgui::ui
