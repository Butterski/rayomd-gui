// SPDX-License-Identifier: Apache-2.0
// The pixel and JPEG kernels. Every kernel's arithmetic is defined exactly, so the C++ scalar
// reference and the x86-64 code (the assembly of gfx/x86_64/*.S, the intrinsics of
// gfx/kernels_simd.cpp) produce the same bytes for every input.
// mul255(a, b) = (t + (t >> 8)) >> 8 with t = a * b + 128.
//
// accumulate_row: for i in [0, width): acc += cells[2i]; v = (acc << 9) - cells[2i+1] (int32,
//   wrapping); a = v >> 9 (arithmetic); m = |a| as (a ^ (a >> 31)) - (a >> 31). NonZero:
//   alpha[i] = min(m, 255). EvenOdd (evenOdd != 0): m &= 511; if (m > 256) m = 512 - m;
//   alpha[i] = min(m, 255). Sets cells[2i] = cells[2i+1] = 0. Returns the OR of all alpha bytes.
// blend_solid: per pixel and channel c (A, R, G, B): s = mul255(color_c, cov), sa =
//   mul255(color_a, cov); dst_c = s + mul255(dst_c, 255 - sa).
// blend_span: the same with a premultiplied src pixel per destination pixel.
// idct_islow: the IJG "islow" integer IDCT (Loeffler-Ligtenberg-Moschytz, CONST_BITS 13,
//   PASS1_BITS 2, the 12 FIX_* constants), columns then rows, rounding by adding 1 << (n - 1)
//   before each arithmetic shift, int32 wrapping arithmetic, the pass-1 results saturated to
//   int16 (as SIMD implementations hold them). Input: the dequantized 8x8 block in natural
//   order. Output: clamp(x + 128, 0, 255), row r at out + r * stride.
// ycc_to_bgra: with cb' = Cb - 128, cr' = Cr - 128: R = Y + ((91881 cr' + 32768) >> 16),
//   G = Y + ((-22554 cb' - 46802 cr' + 32768) >> 16), B = Y + ((116130 cb' + 32768) >> 16),
//   each clamped to 0..255, alpha 255. planes[0..2] are Y, Cb, Cr, n samples each.
// bilinear_row: for i in [0, n): x = xs[i], w0 = ws[i] & 0xFFFF, w1 = ws[i] >> 16 (w0 + w1 =
//   256), y0 = 256 - wy1 (wy1 in 0..255); per channel c, with a = rowA[x], b = rowA[x + 1],
//   p = rowB[x], q = rowB[x + 1]: v = (a_c y0 + p_c y1) w0 + (b_c y0 + q_c y1) w1, dst[i]_c =
//   (v + 32768) >> 16. Reads rowA and rowB only at xs[i] and xs[i] + 1.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

uint32_t rgui_accumulate_row_scalar(int32_t* cells, uint8_t* alpha, size_t width, uint32_t evenOdd);
void rgui_blend_solid_scalar(uint32_t* dst, const uint8_t* coverage, size_t n, uint32_t color);
void rgui_blend_span_scalar(uint32_t* dst, const uint32_t* src, const uint8_t* coverage, size_t n);
void rgui_idct_islow_scalar(const int16_t* coefficients, uint8_t* out, size_t stride);
void rgui_ycc_to_bgra_scalar(const uint8_t* const* planes, uint32_t* dst, size_t n);
void rgui_bilinear_row_scalar(const uint32_t* rowA, const uint32_t* rowB, uint32_t wy1, const int32_t* xs,
                              const uint32_t* ws, size_t n, uint32_t* dst);

// The x86-64 assembly (gfx/x86_64/*.S): same signatures and results, any n including 0, any
// pointer alignment. The avx2 variants need a CPU with AVX2.
uint32_t rgui_accumulate_row_sse2(int32_t* cells, uint8_t* alpha, size_t width, uint32_t evenOdd);
uint32_t rgui_accumulate_row_avx2(int32_t* cells, uint8_t* alpha, size_t width, uint32_t evenOdd);
void rgui_blend_solid_sse2(uint32_t* dst, const uint8_t* coverage, size_t n, uint32_t color);
void rgui_blend_solid_avx2(uint32_t* dst, const uint8_t* coverage, size_t n, uint32_t color);
void rgui_blend_span_sse2(uint32_t* dst, const uint32_t* src, const uint8_t* coverage, size_t n);
void rgui_blend_span_avx2(uint32_t* dst, const uint32_t* src, const uint8_t* coverage, size_t n);
void rgui_idct_islow_sse2(const int16_t* coefficients, uint8_t* out, size_t stride);
void rgui_ycc_to_bgra_sse2(const uint8_t* const* planes, uint32_t* dst, size_t n);
void rgui_ycc_to_bgra_avx2(const uint8_t* const* planes, uint32_t* dst, size_t n);

// The intrinsics (gfx/kernels_simd.cpp): same signatures and results, any n including 0, any
// pointer alignment. The avx2 variants need a CPU with AVX2.
void rgui_bilinear_row_sse2(const uint32_t* rowA, const uint32_t* rowB, uint32_t wy1, const int32_t* xs,
                            const uint32_t* ws, size_t n, uint32_t* dst);
void rgui_bilinear_row_avx2(const uint32_t* rowA, const uint32_t* rowB, uint32_t wy1, const int32_t* xs,
                            const uint32_t* ws, size_t n, uint32_t* dst);

} // extern "C"

namespace rgui::gfx {

struct Kernels {
    uint32_t (*accumulateRow)(int32_t* cells, uint8_t* alpha, size_t width, uint32_t evenOdd);
    void (*blendSolid)(uint32_t* dst, const uint8_t* coverage, size_t n, uint32_t color);
    void (*blendSpan)(uint32_t* dst, const uint32_t* src, const uint8_t* coverage, size_t n);
    void (*yccToBgra)(const uint8_t* const* planes, uint32_t* dst, size_t n);
    void (*bilinearRow)(const uint32_t* rowA, const uint32_t* rowB, uint32_t wy1, const int32_t* xs, const uint32_t* ws,
                        size_t n, uint32_t* dst);
    const char* isa;  // "scalar", "sse2" or "avx2"
};

// The fastest kernels this CPU runs, chosen once: avx2 when the CPU (and OS) support AVX2,
// otherwise sse2.
const Kernels& ActiveKernels();
// The C++ references.
const Kernels& ScalarKernels();

// One 8x8 block through the IDCT kernel (sse2).
void IdctBlock(const int16_t* coefficients, uint8_t* out, size_t stride);

} // namespace rgui::gfx
