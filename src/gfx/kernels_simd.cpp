// SPDX-License-Identifier: Apache-2.0
// The kernels written with intrinsics (definitions in kernels.h): SSE2, the x86-64 baseline, and
// AVX2 compiled for its functions alone, which ActiveKernels picks once.
#include "gfx/kernels.h"

#include <immintrin.h>

#define RGUI_AVX2 __attribute__((target("avx2")))

namespace {

// bilinear_row's channels are 16-bit: a column's vertical blend a_c y0 + p_c y1 is at most
// 255 * 256 = 65,280, exact in a 16-bit lane. Biased by -32768 it is a signed word, so pmaddwd
// with the pair (w0, w1) gives v - 32768 * 256 exactly; kBias adds that back with the rounding
// 32768, and the sum stays within [32768, 16,744,448] before the shift.
constexpr int kBias = 32768 * 256 + 32768;

inline __m128i LoadPair(const uint32_t* p) {
    return _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));
}

// Two pixels' interleaved columns, (a, b) of rowA and (p, q) of rowB at x0 in the low half and
// x1 in the high half: the four channels of each, rounded, as dwords.
inline void BlendTwo(__m128i top, __m128i bottom, __m128i y0, __m128i y1, __m128i w0, __m128i w1, __m128i& r0,
                     __m128i& r1) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i sign = _mm_set1_epi16(-32768);
    const __m128i bias = _mm_set1_epi32(kBias);
    __m128i v0 = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(top, zero), y0),
                               _mm_mullo_epi16(_mm_unpacklo_epi8(bottom, zero), y1));
    __m128i v1 = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(top, zero), y0),
                               _mm_mullo_epi16(_mm_unpackhi_epi8(bottom, zero), y1));
    v0 = _mm_xor_si128(v0, sign);
    v1 = _mm_xor_si128(v1, sign);
    v0 = _mm_unpacklo_epi16(v0, _mm_srli_si128(v0, 8));
    v1 = _mm_unpacklo_epi16(v1, _mm_srli_si128(v1, 8));
    r0 = _mm_srli_epi32(_mm_add_epi32(_mm_madd_epi16(v0, w0), bias), 16);
    r1 = _mm_srli_epi32(_mm_add_epi32(_mm_madd_epi16(v1, w1), bias), 16);
}

// One pixel, for the tails.
inline uint32_t BlendOne(const uint32_t* rowA, const uint32_t* rowB, int32_t x, uint32_t w, __m128i y0,
                         __m128i y1) {
    const __m128i weights = _mm_set1_epi32(static_cast<int>(w));
    __m128i r0, r1;
    BlendTwo(LoadPair(rowA + x), LoadPair(rowB + x), y0, y1, weights, weights, r0, r1);
    const __m128i packed = _mm_packs_epi32(r0, r0);
    return static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(packed, packed)));
}

RGUI_AVX2 inline __m256i LoadFour(const uint32_t* row, const int32_t* xs) {
    const __m128i lo = _mm_unpacklo_epi64(LoadPair(row + xs[0]), LoadPair(row + xs[1]));
    const __m128i hi = _mm_unpacklo_epi64(LoadPair(row + xs[2]), LoadPair(row + xs[3]));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

// Four pixels as words, in the lane order of _mm256_packs_epi32: pixels 0, 1 in the low lane, 2,
// 3 in the high lane.
RGUI_AVX2 inline __m256i BlendFour(const uint32_t* rowA, const uint32_t* rowB, const int32_t* xs, const uint32_t* ws,
                                   __m256i y0, __m256i y1) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i sign = _mm256_set1_epi16(-32768);
    const __m256i bias = _mm256_set1_epi32(kBias);
    const __m256i top = LoadFour(rowA, xs);
    const __m256i bottom = LoadFour(rowB, xs);
    // v02: pixels 0 and 2 (the low halves of the lanes), v13: pixels 1 and 3.
    __m256i v02 = _mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpacklo_epi8(top, zero), y0),
                                   _mm256_mullo_epi16(_mm256_unpacklo_epi8(bottom, zero), y1));
    __m256i v13 = _mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpackhi_epi8(top, zero), y0),
                                   _mm256_mullo_epi16(_mm256_unpackhi_epi8(bottom, zero), y1));
    v02 = _mm256_xor_si256(v02, sign);
    v13 = _mm256_xor_si256(v13, sign);
    v02 = _mm256_unpacklo_epi16(v02, _mm256_srli_si256(v02, 8));
    v13 = _mm256_unpacklo_epi16(v13, _mm256_srli_si256(v13, 8));
    const __m256i w = _mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ws)));
    const __m256i w02 = _mm256_permutevar8x32_epi32(w, _mm256_setr_epi32(0, 0, 0, 0, 2, 2, 2, 2));
    const __m256i w13 = _mm256_permutevar8x32_epi32(w, _mm256_setr_epi32(1, 1, 1, 1, 3, 3, 3, 3));
    const __m256i r02 = _mm256_srli_epi32(_mm256_add_epi32(_mm256_madd_epi16(v02, w02), bias), 16);
    const __m256i r13 = _mm256_srli_epi32(_mm256_add_epi32(_mm256_madd_epi16(v13, w13), bias), 16);
    return _mm256_packs_epi32(r02, r13);
}

} // namespace

extern "C" {

void rgui_bilinear_row_sse2(const uint32_t* rowA, const uint32_t* rowB, uint32_t wy1, const int32_t* xs,
                            const uint32_t* ws, size_t n, uint32_t* dst) {
    const __m128i y1 = _mm_set1_epi16(static_cast<short>(wy1));
    const __m128i y0 = _mm_set1_epi16(static_cast<short>(256 - wy1));
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128i w = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ws + i));
        __m128i r0, r1, r2, r3;
        BlendTwo(_mm_unpacklo_epi64(LoadPair(rowA + xs[i]), LoadPair(rowA + xs[i + 1])),
                 _mm_unpacklo_epi64(LoadPair(rowB + xs[i]), LoadPair(rowB + xs[i + 1])), y0, y1,
                 _mm_shuffle_epi32(w, 0x00), _mm_shuffle_epi32(w, 0x55), r0, r1);
        BlendTwo(_mm_unpacklo_epi64(LoadPair(rowA + xs[i + 2]), LoadPair(rowA + xs[i + 3])),
                 _mm_unpacklo_epi64(LoadPair(rowB + xs[i + 2]), LoadPair(rowB + xs[i + 3])), y0, y1,
                 _mm_shuffle_epi32(w, 0xAA), _mm_shuffle_epi32(w, 0xFF), r2, r3);
        const __m128i out = _mm_packus_epi16(_mm_packs_epi32(r0, r1), _mm_packs_epi32(r2, r3));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), out);
    }
    for (; i < n; i++) dst[i] = BlendOne(rowA, rowB, xs[i], ws[i], y0, y1);
}

RGUI_AVX2 void rgui_bilinear_row_avx2(const uint32_t* rowA, const uint32_t* rowB, uint32_t wy1, const int32_t* xs,
                                      const uint32_t* ws, size_t n, uint32_t* dst) {
    const __m256i y1 = _mm256_set1_epi16(static_cast<short>(wy1));
    const __m256i y0 = _mm256_set1_epi16(static_cast<short>(256 - wy1));
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i lo = BlendFour(rowA, rowB, xs + i, ws + i, y0, y1);      // pixels 0 1 | 2 3
        const __m256i hi = BlendFour(rowA, rowB, xs + i + 4, ws + i + 4, y0, y1);  // pixels 4 5 | 6 7
        // Bytes 0 1 4 5 | 2 3 6 7, put in order by 64-bit quarters.
        const __m256i out = _mm256_permute4x64_epi64(_mm256_packus_epi16(lo, hi), 0xD8);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), out);
    }
    const __m128i y1s = _mm256_castsi256_si128(y1);
    const __m128i y0s = _mm256_castsi256_si128(y0);
    for (; i < n; i++) dst[i] = BlendOne(rowA, rowB, xs[i], ws[i], y0s, y1s);
}

} // extern "C"
