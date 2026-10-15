// SPDX-License-Identifier: Apache-2.0
// The JPEG decoder of DCTDecode images: baseline and extended sequential (SOF0, SOF1) and
// progressive (SOF2) Huffman-coded JPEG with 8-bit samples and 1, 3 or 4 components, sampling
// factors 1 or 2 per axis, restart intervals. Arithmetic coding (SOF9-SOF15, DAC), 12-bit
// samples, lossless and hierarchical frames, and DNL fail with a reason.
//
// The output matches libjpeg's `djpeg -dct int -nosmooth`: the islow integer IDCT
// (gfx::IdctBlock), chroma upsampled by replication, the libjpeg YCbCr constants
// (gfx::ActiveKernels().yccToBgra). Dequantized coefficients are saturated to int16 before the
// IDCT.
//
// Colour models, chosen as libjpeg does from the Adobe APP14 marker's transform byte:
// - 1 component: gray.
// - 3 components: YCbCr, unless APP14 says transform 0, then RGB.
// - 4 components: YCCK when APP14 says transform 2, otherwise CMYK. With an APP14 marker the
//   samples are Adobe-inverted (Adobe stores 255 - ink), so R = c * k / 255; without one they are
//   plain ink amounts, so R = (255 - c) * (255 - k) / 255 (G from m, B from y the same way).
//   Each product is rounded to nearest as (t + (t >> 8)) >> 8 with t = a * b + 128 (gfx::Mul255),
//   which equals round(a * b / 255) for every pair of bytes (no ties) and so djpeg's
//   (int)(a * b / 255.0 + 0.5). YCCK is first turned into Adobe CMYK as libjpeg does:
//   c = 255 - R'(Y, Cr), m = 255 - G'(Y, Cb, Cr), y = 255 - B'(Y, Cb) with R', G', B' the
//   clamped YCbCr conversion, k as stored.
//
// Failures return false and set `reason` to a short token ("jpeg-arithmetic", "jpeg-12-bit",
// "jpeg-dnl", "jpeg-truncated", "jpeg-corrupt", ...), which callers report as `image:<reason>`.
// Every read is bounds-checked; buffers are sized from validated headers (no dimension 0 or
// above 65535, at most 2^28 pixels, no more blocks than the data could code). Work is bounded
// apart from the data size, which progressive scans do not bound (an end-of-band run codes
// 32767 blocks in a few bits): at most 1000 scans, which together visit each coefficient of the
// frame at most 32 times ("jpeg-too-much-work").
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gfx/surface.h"

namespace rgui::pdf {

struct JpegInfo {
    int width = 0;
    int height = 0;
    int components = 0;  // 1, 3 or 4
    bool progressive = false;
};

// Reads the markers up to the frame header. False (with a reason) when there is no supported
// frame header: the same checks the decoder makes before it decodes anything.
bool ReadJpegInfo(const uint8_t* data, size_t size, JpegInfo& info, std::string* reason);

// Decodes to opaque pixels 0xFFRRGGBB; sets out.width, out.height, out.pixels and out.opaque
// (id and version are the caller's). `out` is left unchanged on failure. With `scaleShift` s in
// 1..3 the image is decoded at 1/2^s of its size, ceil(width / 2^s) by ceil(height / 2^s): each
// sample is the rounded mean of a 2^s x 2^s square of the full-size block (the colour model
// applied after), so the planes take 4^s times less memory.
bool DecodeJpeg(const uint8_t* data, size_t size, gfx::Image& out, std::string* reason, int scaleShift = 0);

// Decodes a one-component JPEG to its gray samples, width * height bytes, rows top to bottom (a
// PDF /SMask). A JPEG with more components fails with "jpeg-not-gray".
bool DecodeJpegGray(const uint8_t* data, size_t size, int& width, int& height,
                    std::vector<uint8_t>& gray, std::string* reason);

} // namespace rgui::pdf
