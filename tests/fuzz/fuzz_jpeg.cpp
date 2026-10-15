// SPDX-License-Identifier: Apache-2.0
// The JPEG decoder (DCTDecode images and soft masks): header, colour and gray decoding.
#include "harness.h"

#include "gfx/surface.h"
#include "pdf/jpeg.h"

#include <string>
#include <vector>

namespace rgui::fuzz {

int FuzzJpeg(const uint8_t* data, size_t size) {
    std::string reason;
    pdf::JpegInfo info;
    const bool header = pdf::ReadJpegInfo(data, size, info, &reason);

    gfx::Image image;
    if (pdf::DecodeJpeg(data, size, image, &reason)) {
        if (!header) Fail("DecodeJpeg accepted what ReadJpegInfo refused");
        if (image.width != info.width || image.height != info.height) Fail("DecodeJpeg size differs from ReadJpegInfo");
        if (image.pixels.size() != static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) {
            Fail("DecodeJpeg pixel count");
        }
    }
    int width = 0;
    int height = 0;
    std::vector<uint8_t> gray;
    if (pdf::DecodeJpegGray(data, size, width, height, gray, &reason)) {
        if (gray.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) Fail("DecodeJpegGray pixel count");
    }
    return 0;
}

} // namespace rgui::fuzz
