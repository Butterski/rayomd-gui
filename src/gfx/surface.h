// SPDX-License-Identifier: Apache-2.0
// Pixel buffers. A pixel is 32-bit premultiplied, bytes B, G, R, A in memory: 0xAARRGGBB as a
// little-endian word, the layout of WL_SHM_FORMAT_ARGB8888/XRGB8888 and of 32-bit DIB sections.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rgui::gfx {

// A view of pixels someone else owns. `stride` counts pixels, not bytes.
struct Surface {
    uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;

    uint32_t* Row(int y) const { return pixels + static_cast<std::ptrdiff_t>(y) * stride; }
};

// An owned image, rows top to bottom, `width` pixels each (stride == width).
// `id` names the image for caches and display lists; `version` changes with its pixels.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    bool opaque = true;
    uint64_t id = 0;
    uint32_t version = 0;

    Surface AsSurface() { return {pixels.data(), width, height, width}; }
};

inline constexpr uint32_t PackArgb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

// mul255(a, b) = round(a * b / 255), exactly as the kernels compute it.
inline constexpr uint32_t Mul255(uint32_t a, uint32_t b) {
    const uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

// A straight-alpha colour as a premultiplied pixel.
inline constexpr uint32_t Premultiply(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return PackArgb(a, static_cast<uint8_t>(Mul255(r, a)), static_cast<uint8_t>(Mul255(g, a)),
                    static_cast<uint8_t>(Mul255(b, a)));
}

} // namespace rgui::gfx
