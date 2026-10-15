// SPDX-License-Identifier: Apache-2.0
// The C++ references of the kernels (definitions in kernels.h) and their dispatch.
#include "gfx/kernels.h"

namespace {

inline uint32_t Mul255(uint32_t a, uint32_t b) {
    const uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

inline uint32_t BlendPixel(uint32_t dst, uint32_t src, uint32_t cov) {
    const uint32_t sa = Mul255(src >> 24, cov);
    const uint32_t keep = 255 - sa;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const uint32_t s = Mul255((src >> shift) & 0xFF, cov);
        const uint32_t d = Mul255((dst >> shift) & 0xFF, keep);
        out |= ((s + d) & 0xFF) << shift;
    }
    return out;
}

// int32 arithmetic that wraps, as the vector code's does.
inline int32_t Add(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t Sub(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int32_t Mul(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
inline int32_t Shl(int32_t a, int n) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) << n);
}
inline int32_t Descale(int32_t x, int n) {
    return Add(x, 1 << (n - 1)) >> n;
}
inline int16_t Saturate16(int32_t x) {
    return static_cast<int16_t>(x < -32768 ? -32768 : (x > 32767 ? 32767 : x));
}
inline uint8_t ClampSample(int32_t x) {
    // x + 128 in int32, clamped: x itself first, as the vector code saturates before adding.
    if (x < -128) return 0;
    if (x > 127) return 255;
    return static_cast<uint8_t>(x + 128);
}

constexpr int kConstBits = 13;
constexpr int kPass1Bits = 2;
constexpr int32_t FIX_0_298631336 = 2446;
constexpr int32_t FIX_0_390180644 = 3196;
constexpr int32_t FIX_0_541196100 = 4433;
constexpr int32_t FIX_0_765366865 = 6270;
constexpr int32_t FIX_0_899976223 = 7373;
constexpr int32_t FIX_1_175875602 = 9633;
constexpr int32_t FIX_1_501321110 = 12299;
constexpr int32_t FIX_1_847759065 = 15137;
constexpr int32_t FIX_1_961570560 = 16069;
constexpr int32_t FIX_2_053119869 = 16819;
constexpr int32_t FIX_2_562915447 = 20995;
constexpr int32_t FIX_3_072711026 = 25172;

// One 1-D pass on in[0], in[step], ... in[7 * step]; out[0..7] before descaling.
struct Idct1D {
    int32_t tmp10, tmp11, tmp12, tmp13, tmp0, tmp1, tmp2, tmp3;

    void Run(int32_t i0, int32_t i1, int32_t i2, int32_t i3, int32_t i4, int32_t i5, int32_t i6, int32_t i7) {
        // Even part.
        int32_t z2 = i2;
        int32_t z3 = i6;
        int32_t z1 = Mul(Add(z2, z3), FIX_0_541196100);
        tmp2 = Add(z1, Mul(z3, -FIX_1_847759065));
        tmp3 = Add(z1, Mul(z2, FIX_0_765366865));
        z2 = i0;
        z3 = i4;
        tmp0 = Shl(Add(z2, z3), kConstBits);
        tmp1 = Shl(Sub(z2, z3), kConstBits);
        tmp10 = Add(tmp0, tmp3);
        tmp13 = Sub(tmp0, tmp3);
        tmp11 = Add(tmp1, tmp2);
        tmp12 = Sub(tmp1, tmp2);
        // Odd part.
        tmp0 = i7;
        tmp1 = i5;
        tmp2 = i3;
        tmp3 = i1;
        z1 = Add(tmp0, tmp3);
        z2 = Add(tmp1, tmp2);
        z3 = Add(tmp0, tmp2);
        int32_t z4 = Add(tmp1, tmp3);
        const int32_t z5 = Mul(Add(z3, z4), FIX_1_175875602);
        tmp0 = Mul(tmp0, FIX_0_298631336);
        tmp1 = Mul(tmp1, FIX_2_053119869);
        tmp2 = Mul(tmp2, FIX_3_072711026);
        tmp3 = Mul(tmp3, FIX_1_501321110);
        z1 = Mul(z1, -FIX_0_899976223);
        z2 = Mul(z2, -FIX_2_562915447);
        z3 = Mul(z3, -FIX_1_961570560);
        z4 = Mul(z4, -FIX_0_390180644);
        z3 = Add(z3, z5);
        z4 = Add(z4, z5);
        tmp0 = Add(tmp0, Add(z1, z3));
        tmp1 = Add(tmp1, Add(z2, z4));
        tmp2 = Add(tmp2, Add(z2, z3));
        tmp3 = Add(tmp3, Add(z1, z4));
    }
    // The outputs 0..7 in order.
    void Outputs(int32_t (&o)[8]) const {
        o[0] = Add(tmp10, tmp3);
        o[7] = Sub(tmp10, tmp3);
        o[1] = Add(tmp11, tmp2);
        o[6] = Sub(tmp11, tmp2);
        o[2] = Add(tmp12, tmp1);
        o[5] = Sub(tmp12, tmp1);
        o[3] = Add(tmp13, tmp0);
        o[4] = Sub(tmp13, tmp0);
    }
};

int ClampChannel(int value) {
    return value < 0 ? 0 : (value > 255 ? 255 : value);
}

} // namespace

extern "C" {

uint32_t rgui_accumulate_row_scalar(int32_t* cells, uint8_t* alpha, size_t width, uint32_t evenOdd) {
    int32_t acc = 0;
    uint32_t any = 0;
    for (size_t i = 0; i < width; i++) {
        acc = Add(acc, cells[2 * i]);
        const int32_t v = Sub(Shl(acc, 9), cells[2 * i + 1]);
        const int32_t a = v >> 9;
        int32_t m = Sub(a ^ (a >> 31), a >> 31);  // |a|; a lies in [-2^22, 2^22)
        if (evenOdd) {
            m &= 511;
            if (m > 256) m = 512 - m;
        }
        const uint8_t out = static_cast<uint8_t>(m > 255 ? 255 : m);
        alpha[i] = out;
        any |= out;
        cells[2 * i] = 0;
        cells[2 * i + 1] = 0;
    }
    return any;
}

void rgui_blend_solid_scalar(uint32_t* dst, const uint8_t* coverage, size_t n, uint32_t color) {
    for (size_t i = 0; i < n; i++) dst[i] = BlendPixel(dst[i], color, coverage[i]);
}

void rgui_blend_span_scalar(uint32_t* dst, const uint32_t* src, const uint8_t* coverage, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = BlendPixel(dst[i], src[i], coverage[i]);
}

void rgui_idct_islow_scalar(const int16_t* coefficients, uint8_t* out, size_t stride) {
    int16_t workspace[64];
    for (int column = 0; column < 8; column++) {
        const int16_t* in = coefficients + column;
        Idct1D pass;
        pass.Run(in[0], in[8], in[16], in[24], in[32], in[40], in[48], in[56]);
        int32_t o[8];
        pass.Outputs(o);
        for (int row = 0; row < 8; row++) {
            workspace[row * 8 + column] = Saturate16(Descale(o[row], kConstBits - kPass1Bits));
        }
    }
    for (int row = 0; row < 8; row++) {
        const int16_t* w = workspace + row * 8;
        Idct1D pass;
        pass.Run(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        int32_t o[8];
        pass.Outputs(o);
        uint8_t* line = out + static_cast<size_t>(row) * stride;
        for (int column = 0; column < 8; column++) {
            line[column] = ClampSample(Descale(o[column], kConstBits + kPass1Bits + 3));
        }
    }
}

void rgui_ycc_to_bgra_scalar(const uint8_t* const* planes, uint32_t* dst, size_t n) {
    const uint8_t* ys = planes[0];
    const uint8_t* cbs = planes[1];
    const uint8_t* crs = planes[2];
    for (size_t i = 0; i < n; i++) {
        const int y = ys[i];
        const int cb = cbs[i] - 128;
        const int cr = crs[i] - 128;
        const int r = ClampChannel(y + ((91881 * cr + 32768) >> 16));
        const int g = ClampChannel(y + ((-22554 * cb - 46802 * cr + 32768) >> 16));
        const int b = ClampChannel(y + ((116130 * cb + 32768) >> 16));
        dst[i] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                 static_cast<uint32_t>(b);
    }
}

} // extern "C"

namespace rgui::gfx {

const Kernels& ScalarKernels() {
    static const Kernels kernels{rgui_accumulate_row_scalar, rgui_blend_solid_scalar, rgui_blend_span_scalar,
                                 rgui_ycc_to_bgra_scalar, "scalar"};
    return kernels;
}

const Kernels& ActiveKernels() {
    static const Kernels sse2{rgui_accumulate_row_sse2, rgui_blend_solid_sse2, rgui_blend_span_sse2,
                              rgui_ycc_to_bgra_sse2, "sse2"};
    static const Kernels avx2{rgui_accumulate_row_avx2, rgui_blend_solid_avx2, rgui_blend_span_avx2,
                              rgui_ycc_to_bgra_avx2, "avx2"};
    // libgcc's check includes the OS saving the ymm state (OSXSAVE and XCR0).
    static const Kernels* const active = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") ? &avx2 : &sse2;
    }();
    return *active;
}

void IdctBlock(const int16_t* coefficients, uint8_t* out, size_t stride) {
    rgui_idct_islow_sse2(coefficients, out, stride);
}

} // namespace rgui::gfx
