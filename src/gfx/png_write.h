// SPDX-License-Identifier: Apache-2.0
// Writes a surface as an 8-bit RGB PNG (filter 0 on every row, IHDR, IDAT and IEND), the zlib
// data made by the engine's RayoMd::Flate::ZlibCompress. Alpha is dropped: the surface is
// expected opaque, as a framebuffer is.
#pragma once

#include "gfx/surface.h"

#include <filesystem>

namespace rgui::gfx {

bool WritePng(const std::filesystem::path& path, const Surface& surface);

} // namespace rgui::gfx
