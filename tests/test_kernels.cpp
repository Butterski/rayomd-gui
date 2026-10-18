// SPDX-License-Identifier: Apache-2.0
// The assembly kernels against their C++ references: random and corner-case inputs, unaligned
// buffers, every output byte (guards around the buffers included), cleared cells and return
// values compared exactly, for each instruction set this CPU runs.
#include "gfx/kernels.h"
#include "runner.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kCases = 2000;
constexpr size_t kMaxLength = 300;
constexpr size_t kGuard = 64;

// xorshift64: fixed seeds, the same cases on every run and platform.
struct Rng {
    uint64_t state;
    uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    uint32_t U32() { return static_cast<uint32_t>(Next() >> 32); }
    uint32_t Below(uint32_t n) { return static_cast<uint32_t>((Next() >> 16) % n); }
    int32_t Range(int32_t lo, int32_t hi) {  // inclusive
        return static_cast<int32_t>(static_cast<int64_t>(lo) + Below(static_cast<uint32_t>(hi - lo + 1)));
    }
    uint8_t Byte() { return static_cast<uint8_t>(Next() >> 56); }
};

bool HasAvx2() {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
}

template <typename F>
struct Variant {
    const char* isa;
    F fn;
};

// The sse2 variant, and the avx2 one when the CPU has AVX2 (otherwise says it is skipped).
template <typename F>
std::vector<Variant<F>> Variants(const char* test, F sse2, F avx2) {
    std::vector<Variant<F>> variants{{"sse2", sse2}};
    if (HasAvx2()) {
        variants.push_back({"avx2", avx2});
    } else {
        std::printf("skip %s avx2: this CPU has no AVX2\n", test);
    }
    return variants;
}

// A byte buffer holding `bytes` of data at `offset`, surrounded by random guard bytes.
struct Buffer {
    std::vector<uint8_t> bytes;
    size_t offset;

    Buffer(Rng& rng, size_t size, size_t offsetBytes) : bytes(offsetBytes + size + kGuard), offset(offsetBytes) {
        for (uint8_t& b : bytes) b = rng.Byte();
    }
    uint8_t* Data() { return bytes.data() + offset; }
    void Put(const void* data, size_t size) {
        if (size) std::memcpy(Data(), data, size);
    }
};

bool SameBytes(const Buffer& a, const Buffer& b) {
    return a.bytes == b.bytes;
}

void Report(const char* isa, int testCase, const std::string& what) {
    rgui::test::Fail(__FILE__, __LINE__, std::string(isa) + " case " + std::to_string(testCase) + ": " + what);
}

uint32_t Premultiplied(Rng& rng) {
    const uint32_t a = rng.Byte();
    const uint32_t r = a ? rng.Below(a + 1) : 0;
    const uint32_t g = a ? rng.Below(a + 1) : 0;
    const uint32_t b = a ? rng.Below(a + 1) : 0;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// A pixel: premultiplied, arbitrary, or one of the extremes.
uint32_t Pixel(Rng& rng, uint32_t mode) {
    switch (mode) {
    case 0: return Premultiplied(rng);
    case 1: return rng.U32();
    case 2: return 0;
    case 3: return 0xFFFFFFFFu;
    default: return rng.Below(2) ? 0u : 0xFFFFFFFFu;
    }
}

// A coverage byte: random, 0, 255, or a mix of the three.
uint8_t Coverage(Rng& rng, uint32_t mode) {
    switch (mode) {
    case 0: return rng.Byte();
    case 1: return 0;
    case 2: return 255;
    default: {
        const uint32_t pick = rng.Below(3);
        return pick == 0 ? 0 : (pick == 1 ? 255 : rng.Byte());
    }
    }
}

size_t Length(Rng& rng, int testCase) {
    // The first cases walk every length around the vector widths, the rest are random.
    if (testCase < 40) return static_cast<size_t>(testCase);
    return rng.Below(static_cast<uint32_t>(kMaxLength + 1));
}

// ---- accumulate_row ----

// One {cover, area} cell pair. Mode 0 is what the rasterizer makes; the others reach the
// wrapping and clamping edges of the definition.
void Cell(Rng& rng, uint32_t mode, int32_t& cover, int32_t& area) {
    switch (mode) {
    case 0: {
        if (rng.Below(3) == 0) {
            cover = 0;
            area = 0;
            return;
        }
        cover = rng.Range(-256, 256);
        area = cover * rng.Range(0, 512);
        return;
    }
    case 1:
        cover = static_cast<int32_t>(rng.U32());
        area = static_cast<int32_t>(rng.U32());
        return;
    case 2: {
        // acc << 9 near +-2^31: covers of about 2^22, areas near the int32 limits.
        static const int32_t kAreas[] = {0, 1, -1, 255, 256, 511, 512, -511, -512, INT32_MIN, INT32_MAX,
                                         INT32_MIN + 1, INT32_MAX - 1, 1 << 30, -(1 << 30)};
        const int32_t base = rng.Below(2) ? (1 << 22) : -(1 << 22);
        cover = rng.Below(2) ? base + rng.Range(-2, 2) : rng.Range(-3, 3);
        area = rng.Below(2) ? kAreas[rng.Below(sizeof(kAreas) / sizeof(kAreas[0]))]
                            : static_cast<int32_t>(rng.U32());
        return;
    }
    default:
        cover = rng.Range(-1024, 1024);
        area = static_cast<int32_t>(rng.U32());
        return;
    }
}

using AccumulateFn = uint32_t (*)(int32_t*, uint8_t*, size_t, uint32_t);

// Runs `fn` on unaligned copies of `cells` and compares with the reference; false on a mismatch.
bool CheckAccumulate(Rng& rng, const Variant<AccumulateFn>& variant, int testCase, const std::vector<int32_t>& cells,
                     uint32_t evenOdd) {
    const size_t width = cells.size() / 2;
    const size_t cellBytes = cells.size() * sizeof(int32_t);
    std::vector<int32_t> refCells = cells;
    std::vector<uint8_t> refAlpha(width + 1);
    const uint32_t refAny = rgui_accumulate_row_scalar(refCells.data(), refAlpha.data(), width, evenOdd);

    Buffer cellBuf(rng, cellBytes, rng.Below(32));
    Buffer alphaBuf(rng, width, rng.Below(32));
    cellBuf.Put(cells.data(), cellBytes);
    Buffer expectCells = cellBuf;
    Buffer expectAlpha = alphaBuf;
    expectCells.Put(refCells.data(), cellBytes);
    expectAlpha.Put(refAlpha.data(), width);

    const uint32_t any = variant.fn(reinterpret_cast<int32_t*>(cellBuf.Data()), alphaBuf.Data(), width, evenOdd);
    std::string what;
    if (any != refAny) what += "return " + std::to_string(any) + " vs " + std::to_string(refAny) + "; ";
    if (!SameBytes(alphaBuf, expectAlpha)) what += "alpha bytes differ; ";
    if (!SameBytes(cellBuf, expectCells)) what += "cells differ; ";
    if (what.empty()) return true;
    Report(variant.isa, testCase,
           what + "width " + std::to_string(width) + (evenOdd ? " even-odd" : " nonzero") + ", offsets " +
               std::to_string(cellBuf.offset) + "/" + std::to_string(alphaBuf.offset));
    return false;
}

RGUI_TEST("kernels.accumulate") {
    for (const auto& variant :
         Variants<AccumulateFn>("kernels.accumulate", rgui_accumulate_row_sse2, rgui_accumulate_row_avx2)) {
        Rng rng{0x9E3779B97F4A7C15ull};
        bool ok = true;
        for (int testCase = 0; testCase < kCases && ok; testCase++) {
            const size_t width = Length(rng, testCase);
            const uint32_t mode = rng.Below(5);
            std::vector<int32_t> cells(2 * width);
            for (size_t i = 0; i < width; i++) {
                Cell(rng, mode == 4 ? rng.Below(4) : mode, cells[2 * i], cells[2 * i + 1]);
            }
            ok = CheckAccumulate(rng, variant, testCase, cells, rng.Below(2) ? 0u : 1u + rng.Below(7));
        }
        // v = (acc << 9) - area exactly at and around +-2^31, for both fill rules.
        static const int32_t kAreas[] = {0, 1, -1, 256, 511, 512, 513, INT32_MIN, INT32_MAX, INT32_MIN + 511};
        for (uint32_t evenOdd = 0; evenOdd < 2 && ok; evenOdd++) {
            for (int32_t acc : {1 << 22, -(1 << 22), (1 << 22) - 1, -(1 << 22) + 1, 0}) {
                std::vector<int32_t> cells;
                for (int32_t area : kAreas) {
                    // Each cell brings the running sum back to `acc`.
                    const int32_t previous = cells.empty() ? 0 : acc;
                    cells.push_back(static_cast<int32_t>(static_cast<uint32_t>(acc) - static_cast<uint32_t>(previous)));
                    cells.push_back(area);
                }
                ok = ok && CheckAccumulate(rng, variant, -1, cells, evenOdd);
            }
        }
        // Rows of full coverage, half coverage and nothing, over several vector widths.
        for (int32_t cover : {256, -256, 128, 0}) {
            std::vector<int32_t> cells(2 * 37, 0);
            cells[0] = cover;
            for (uint32_t evenOdd = 0; evenOdd < 2 && ok; evenOdd++) {
                ok = CheckAccumulate(rng, variant, -2, cells, evenOdd);
            }
        }
    }
}

// ---- blend_solid, blend_span ----

using SolidFn = void (*)(uint32_t*, const uint8_t*, size_t, uint32_t);
using SpanFn = void (*)(uint32_t*, const uint32_t*, const uint8_t*, size_t);

RGUI_TEST("kernels.blend_solid") {
    for (const auto& variant : Variants<SolidFn>("kernels.blend_solid", rgui_blend_solid_sse2, rgui_blend_solid_avx2)) {
        Rng rng{0xD1B54A32D192ED03ull};
        for (int testCase = 0; testCase < kCases; testCase++) {
            const size_t n = Length(rng, testCase);
            const uint32_t color = Pixel(rng, rng.Below(4));
            const uint32_t dstMode = rng.Below(5);
            const uint32_t coverageMode = rng.Below(4);
            std::vector<uint32_t> dst(n + 1);
            std::vector<uint8_t> coverage(n + 1);
            for (size_t i = 0; i < n; i++) {
                dst[i] = Pixel(rng, dstMode);
                coverage[i] = Coverage(rng, coverageMode);
            }
            Buffer dstBuf(rng, n * 4, rng.Below(32));
            Buffer coverageBuf(rng, n, rng.Below(32));
            dstBuf.Put(dst.data(), n * 4);
            coverageBuf.Put(coverage.data(), n);
            const Buffer coverageBefore = coverageBuf;
            rgui_blend_solid_scalar(dst.data(), coverage.data(), n, color);
            Buffer expect = dstBuf;
            expect.Put(dst.data(), n * 4);

            variant.fn(reinterpret_cast<uint32_t*>(dstBuf.Data()), coverageBuf.Data(), n, color);
            if (!SameBytes(dstBuf, expect) || !SameBytes(coverageBuf, coverageBefore)) {
                Report(variant.isa, testCase,
                       "n " + std::to_string(n) + ", color " + std::to_string(color) + ", offsets " +
                           std::to_string(dstBuf.offset) + "/" + std::to_string(coverageBuf.offset));
                break;
            }
        }
    }
}

RGUI_TEST("kernels.blend_span") {
    for (const auto& variant : Variants<SpanFn>("kernels.blend_span", rgui_blend_span_sse2, rgui_blend_span_avx2)) {
        Rng rng{0x2545F4914F6CDD1Dull};
        for (int testCase = 0; testCase < kCases; testCase++) {
            const size_t n = Length(rng, testCase);
            const uint32_t srcMode = rng.Below(5);
            const uint32_t dstMode = rng.Below(5);
            const uint32_t coverageMode = rng.Below(4);
            std::vector<uint32_t> dst(n + 1);
            std::vector<uint32_t> src(n + 1);
            std::vector<uint8_t> coverage(n + 1);
            for (size_t i = 0; i < n; i++) {
                dst[i] = Pixel(rng, dstMode);
                src[i] = Pixel(rng, srcMode);
                coverage[i] = Coverage(rng, coverageMode);
            }
            Buffer dstBuf(rng, n * 4, rng.Below(32));
            Buffer srcBuf(rng, n * 4, rng.Below(32));
            Buffer coverageBuf(rng, n, rng.Below(32));
            dstBuf.Put(dst.data(), n * 4);
            srcBuf.Put(src.data(), n * 4);
            coverageBuf.Put(coverage.data(), n);
            const Buffer srcBefore = srcBuf;
            const Buffer coverageBefore = coverageBuf;
            rgui_blend_span_scalar(dst.data(), src.data(), coverage.data(), n);
            Buffer expect = dstBuf;
            expect.Put(dst.data(), n * 4);

            variant.fn(reinterpret_cast<uint32_t*>(dstBuf.Data()), reinterpret_cast<const uint32_t*>(srcBuf.Data()),
                       coverageBuf.Data(), n);
            if (!SameBytes(dstBuf, expect) || !SameBytes(srcBuf, srcBefore) ||
                !SameBytes(coverageBuf, coverageBefore)) {
                Report(variant.isa, testCase,
                       "n " + std::to_string(n) + ", offsets " + std::to_string(dstBuf.offset) + "/" +
                           std::to_string(srcBuf.offset) + "/" + std::to_string(coverageBuf.offset));
                break;
            }
        }
    }
}

// ---- bilinear_row ----

using BilinearFn = void (*)(const uint32_t*, const uint32_t*, uint32_t, const int32_t*, const uint32_t*, size_t,
                            uint32_t*);

RGUI_TEST("kernels.bilinear_row") {
    // The reference against the bilinear sum of gfx::Canvas's general sampler, horizontal first:
    // ((a w0 + b w1) y0 + (p w0 + q w1) y1 + 32768) >> 16 per channel.
    {
        Rng rng{0x9E3779B97F4A7C15ull};
        bool same = true;
        for (int testCase = 0; testCase < kCases && same; testCase++) {
            const uint32_t a = rng.U32(), b = rng.U32(), p = rng.U32(), q = rng.U32();
            const uint32_t rowA[2] = {a, b}, rowB[2] = {p, q};
            const uint32_t wy1 = testCase < 4 ? (testCase & 1) * 255 : rng.Below(256);
            const uint32_t w1 = testCase < 4 ? (testCase >> 1) * 256 : rng.Below(257);
            const int32_t x = 0;
            const uint32_t ws = (256 - w1) | (w1 << 16);
            uint32_t out = 0;
            rgui_bilinear_row_scalar(rowA, rowB, wy1, &x, &ws, 1, &out);
            uint32_t expect = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                const uint32_t top = ((a >> shift) & 0xFF) * (256 - w1) + ((b >> shift) & 0xFF) * w1;
                const uint32_t bottom = ((p >> shift) & 0xFF) * (256 - w1) + ((q >> shift) & 0xFF) * w1;
                expect |= ((top * (256 - wy1) + bottom * wy1 + 32768) >> 16) << shift;
            }
            if (out != expect) {
                Report("scalar", testCase, "against the horizontal-first sum");
                same = false;
            }
        }
    }
    for (const auto& variant :
         Variants<BilinearFn>("kernels.bilinear_row", rgui_bilinear_row_sse2, rgui_bilinear_row_avx2)) {
        Rng rng{0xD1B54A32D192ED03ull};
        for (int testCase = 0; testCase < kCases; testCase++) {
            const size_t n = Length(rng, testCase);
            const uint32_t pixelMode = rng.Below(5);
            const size_t width = 2 + rng.Below(40);
            std::vector<uint32_t> rowA(width), rowB(width);
            for (size_t i = 0; i < width; i++) {
                rowA[i] = Pixel(rng, pixelMode);
                rowB[i] = Pixel(rng, pixelMode);
            }
            std::vector<int32_t> xs(n + 1);
            std::vector<uint32_t> ws(n + 1);
            for (size_t i = 0; i < n; i++) {
                xs[i] = static_cast<int32_t>(rng.Below(static_cast<uint32_t>(width - 1)));
                const uint32_t pick = rng.Below(4);
                const uint32_t w1 = pick == 0 ? 0 : (pick == 1 ? 256 : rng.Below(257));
                ws[i] = (256 - w1) | (w1 << 16);
            }
            const uint32_t wy1 = rng.Below(3) == 0 ? 0 : rng.Below(256);
            std::vector<uint32_t> expect(n + 1);
            rgui_bilinear_row_scalar(rowA.data(), rowB.data(), wy1, xs.data(), ws.data(), n, expect.data());
            Buffer dstBuf(rng, n * 4, rng.Below(32));
            Buffer want = dstBuf;
            want.Put(expect.data(), n * 4);
            variant.fn(rowA.data(), rowB.data(), wy1, xs.data(), ws.data(), n, reinterpret_cast<uint32_t*>(dstBuf.Data()));
            if (!SameBytes(dstBuf, want)) {
                Report(variant.isa, testCase, "n " + std::to_string(n) + ", offset " + std::to_string(dstBuf.offset));
                break;
            }
        }
    }
}

// ---- ycc_to_bgra ----

using YccFn = void (*)(const uint8_t* const*, uint32_t*, size_t);

RGUI_TEST("kernels.ycc") {
    for (const auto& variant : Variants<YccFn>("kernels.ycc", rgui_ycc_to_bgra_sse2, rgui_ycc_to_bgra_avx2)) {
        Rng rng{0xBF58476D1CE4E5B9ull};
        for (int testCase = 0; testCase < kCases; testCase++) {
            const size_t n = Length(rng, testCase);
            // Random samples, or only the extremes 0, 1, 127, 128, 254, 255.
            const bool extremes = rng.Below(4) == 0;
            std::vector<uint8_t> planes[3];
            Buffer planeBufs[3] = {Buffer(rng, n, rng.Below(32)), Buffer(rng, n, rng.Below(32)),
                                   Buffer(rng, n, rng.Below(32))};
            for (int p = 0; p < 3; p++) {
                planes[p].resize(n + 1);
                for (size_t i = 0; i < n; i++) {
                    static const uint8_t kEdges[] = {0, 1, 127, 128, 254, 255};
                    planes[p][i] = extremes ? kEdges[rng.Below(6)] : rng.Byte();
                }
                planeBufs[p].Put(planes[p].data(), n);
            }
            std::vector<uint32_t> dst(n + 1);
            const uint8_t* const refPlanes[3] = {planes[0].data(), planes[1].data(), planes[2].data()};
            rgui_ycc_to_bgra_scalar(refPlanes, dst.data(), n);
            Buffer dstBuf(rng, n * 4, rng.Below(32));
            Buffer expect = dstBuf;
            expect.Put(dst.data(), n * 4);

            const uint8_t* const asmPlanes[3] = {planeBufs[0].Data(), planeBufs[1].Data(), planeBufs[2].Data()};
            variant.fn(asmPlanes, reinterpret_cast<uint32_t*>(dstBuf.Data()), n);
            if (!SameBytes(dstBuf, expect)) {
                Report(variant.isa, testCase, "n " + std::to_string(n) + ", dst offset " + std::to_string(dstBuf.offset));
                break;
            }
        }
        // Every (Y, Cb, Cr) triple once.
        std::vector<uint8_t> ys(1 << 16), cbs(1 << 16), crs(1 << 16);
        bool ok = true;
        for (uint32_t y = 0; y < 256 && ok; y++) {
            for (uint32_t i = 0; i < (1u << 16); i++) {
                ys[i] = static_cast<uint8_t>(y);
                cbs[i] = static_cast<uint8_t>(i >> 8);
                crs[i] = static_cast<uint8_t>(i);
            }
            const uint8_t* const planes[3] = {ys.data(), cbs.data(), crs.data()};
            std::vector<uint32_t> expect(1 << 16), got(1 << 16);
            rgui_ycc_to_bgra_scalar(planes, expect.data(), expect.size());
            variant.fn(planes, got.data(), got.size());
            if (expect != got) {
                Report(variant.isa, -1, "exhaustive Y " + std::to_string(y));
                ok = false;
            }
        }
    }
}

// ---- idct_islow ----

// A dequantized block: full-range int16, only extremes, typical JPEG magnitudes, or DC only.
void Block(Rng& rng, uint32_t mode, int16_t (&block)[64]) {
    for (int i = 0; i < 64; i++) {
        const int row = i / 8;
        const int column = i % 8;
        int32_t value = 0;
        switch (mode) {
        case 0: value = static_cast<int16_t>(rng.U32()); break;
        case 1: {
            static const int16_t kEdges[] = {-32768, 32767, 0, -1, 1, -32767, 16384, -16384};
            value = kEdges[rng.Below(8)];
            break;
        }
        case 2: {
            // Quantized values shrinking with frequency (many zero), times a quantizer step.
            const int32_t limit = 1024 >> (row + column < 10 ? row + column : 10);
            const int32_t q = rng.Range(1, 64);
            value = rng.Below(3) == 0 ? 0 : rng.Range(-limit, limit) * q;
            break;
        }
        default: value = i == 0 ? rng.Range(-1024, 1023) * rng.Range(1, 16) : 0; break;
        }
        block[i] = static_cast<int16_t>(value < -32768 ? -32768 : (value > 32767 ? 32767 : value));
    }
}

RGUI_TEST("kernels.idct") {
    Rng rng{0x94D049BB133111EBull};
    for (int testCase = 0; testCase < kCases; testCase++) {
        int16_t block[64];
        Block(rng, rng.Below(4), block);
        const size_t stride = 8 + rng.Below(33);
        const size_t outBytes = 7 * stride + 8;
        Buffer outBuf(rng, outBytes, rng.Below(32));
        Buffer inBuf(rng, sizeof(block), rng.Below(32));
        inBuf.Put(block, sizeof(block));
        const Buffer inBefore = inBuf;
        std::vector<uint8_t> out(outBuf.Data(), outBuf.Data() + outBytes);
        rgui_idct_islow_scalar(block, out.data(), stride);
        Buffer expect = outBuf;
        expect.Put(out.data(), outBytes);

        rgui_idct_islow_sse2(reinterpret_cast<const int16_t*>(inBuf.Data()), outBuf.Data(), stride);
        if (!SameBytes(outBuf, expect) || !SameBytes(inBuf, inBefore)) {
            Report("sse2", testCase, "stride " + std::to_string(stride) + ", offsets " + std::to_string(inBuf.offset) +
                                         "/" + std::to_string(outBuf.offset));
            break;
        }
    }
}

} // namespace
