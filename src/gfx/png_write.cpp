// SPDX-License-Identifier: Apache-2.0
// The PNG writer (plan Phase 4 step 7): 8-bit RGB, filter 0 on every row, one IDAT.
#include "gfx/png_write.h"

#include "common/file_io.h"
#include "core/flate.h"
#include "gfx/crc32.h"

#include <string>
#include <vector>

namespace rgui::gfx {

namespace {

void PutU32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

// Length, type, data and the CRC of type and data.
void PutChunk(std::string& out, const char type[4], const uint8_t* data, size_t size) {
    PutU32(out, static_cast<uint32_t>(size));
    const size_t typeAt = out.size();
    out.append(type, 4);
    if (size > 0) out.append(reinterpret_cast<const char*>(data), size);
    PutU32(out, rgui_crc32(0, out.data() + typeAt, 4 + size));
}

} // namespace

bool WritePng(const std::filesystem::path& path, const Surface& surface) {
    if (surface.pixels == nullptr || surface.width <= 0 || surface.height <= 0 || surface.stride < surface.width) {
        return false;
    }
    const size_t w = static_cast<size_t>(surface.width);
    const size_t h = static_cast<size_t>(surface.height);
    const size_t rowBytes = 1 + 3 * w;
    if (h > RayoMd::Flate::kMaxInput / rowBytes) return false;

    std::string raw;
    raw.resize(rowBytes * h);
    for (size_t y = 0; y < h; ++y) {
        char* out = &raw[y * rowBytes];
        *out++ = 0;  // filter: none
        const uint32_t* row = surface.Row(static_cast<int>(y));
        for (size_t x = 0; x < w; ++x) {
            const uint32_t p = row[x];
            *out++ = static_cast<char>(p >> 16);
            *out++ = static_cast<char>(p >> 8);
            *out++ = static_cast<char>(p);
        }
    }
    std::vector<uint8_t> zlib(RayoMd::Flate::ZlibBound(raw.size()));
    RayoMd::Flate::Scratch scratch;
    const size_t zlibSize = RayoMd::Flate::ZlibCompress(raw, zlib.data(), scratch);

    std::string file("\x89PNG\r\n\x1a\n", 8);
    uint8_t ihdr[13] = {};
    for (int i = 0; i < 4; ++i) {
        ihdr[i] = static_cast<uint8_t>(w >> (24 - 8 * i));
        ihdr[4 + i] = static_cast<uint8_t>(h >> (24 - 8 * i));
    }
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 2;  // colour type: RGB
    // Compression, filter and interlace methods 0.
    PutChunk(file, "IHDR", ihdr, sizeof ihdr);
    PutChunk(file, "IDAT", zlib.data(), zlibSize);
    PutChunk(file, "IEND", nullptr, 0);
    return RayoMd::File::WriteAll(path, file);
}

} // namespace rgui::gfx
