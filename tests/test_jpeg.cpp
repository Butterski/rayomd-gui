// SPDX-License-Identifier: Apache-2.0
// The JPEG decoder (pdf/jpeg.h).
//
// Fixtures, made with libjpeg-turbo 3.2 in rgui-arch:
// - corpus/photo.jpg: rayomd-lab's harness image (1024x1024 baseline YCbCr 4:2:0);
//   corpus/photo-progressive.jpg: `djpeg photo.jpg | cjpeg -progressive`.
// - data/jpeg/: from crop.ppm = `jpegtran -crop 157x101+400+380 photo.jpg | djpeg -ppm`
//   (157x113 after jpegtran's MCU alignment), cjpeg -quality 85 with -grayscale (gray.jpg),
//   -grayscale -progressive, -sample 1x1 (444), -sample 2x1 (422), -sample 1x2,2x1,1x1
//   (mixed-sampling), -restart 1 (420-restart-rows), -restart 7B -progressive -sample 2x1
//   (422-progressive-restart), -rgb (Adobe transform 0); -quality 2 (sof1-16bit-dqt: SOF1 with
//   16-bit quantization tables); cmyk.jpg and ycck.jpg (APP14 transforms 0 and 2, YCCK with
//   -restart 2 rows) by a throwaway libjpeg program writing Adobe-inverted CMYK of crop.ppm;
//   420-jpegtran-progressive = `jpegtran -progressive 420-restart-rows.jpg`,
//   444-jpegtran-progressive-restart = `jpegtran -progressive -restart 5B 444.jpg`;
//   photo-crop.jpg and photo-progressive-crop.jpg = `jpegtran [-progressive] -crop
//   128x128+448+448` of the corpus files; arithmetic.jpg and 12bit.jpg = a 16x16 crop through
//   `cjpeg -arithmetic` and `cjpeg -precision 12`.
#include "runner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "pdf/jpeg.h"

namespace {

namespace fs = std::filesystem;
using rgui::pdf::DecodeJpeg;
using rgui::pdf::DecodeJpegGray;
using rgui::pdf::JpegInfo;
using rgui::pdf::ReadJpegInfo;

fs::path DataPath(const char* relative) {
    return fs::path(rgui::test::SourceDir()) / "tests" / relative;
}

std::vector<uint8_t> Load(const char* relative) {
    std::vector<uint8_t> bytes;
    if (!RayoMd::File::ReadAll(DataPath(relative), 16u << 20, bytes)) {
        std::printf("cannot read %s\n", relative);
    }
    return bytes;
}

// Every JPEG of the corpus and of tests/data/jpeg that the decoder supports.
std::vector<fs::path> SupportedFiles() {
    std::vector<fs::path> files = {DataPath("corpus/photo.jpg"), DataPath("corpus/photo-progressive.jpg")};
    std::vector<fs::path> data;
    for (const fs::directory_entry& entry : fs::directory_iterator(DataPath("data/jpeg"))) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".jpg" || name == "arithmetic.jpg" || name == "12bit.jpg") continue;
        data.push_back(entry.path());
    }
    std::sort(data.begin(), data.end());
    files.insert(files.end(), data.begin(), data.end());
    return files;
}

// The offset of a frame header's height field (SOF0-SOF2 marker + 5).
size_t FrameHeightOffset(const std::vector<uint8_t>& jpeg) {
    for (size_t i = 2; i + 9 < jpeg.size(); i++) {
        if (jpeg[i] == 0xFF && (jpeg[i + 1] == 0xC0 || jpeg[i + 1] == 0xC1 || jpeg[i + 1] == 0xC2)) return i + 5;
    }
    return 0;
}

void Put16(std::vector<uint8_t>& bytes, size_t at, int value) {
    bytes[at] = static_cast<uint8_t>(value >> 8);
    bytes[at + 1] = static_cast<uint8_t>(value);
}

std::string Reason(const std::vector<uint8_t>& jpeg) {
    rgui::gfx::Image image;
    std::string reason;
    if (DecodeJpeg(jpeg.data(), jpeg.size(), image, &reason)) return "decoded";
    return reason;
}

struct Pnm {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> samples;
};

// A binary P5/P6 file with maximum 255, as djpeg writes it.
bool ParsePnm(const std::string& file, Pnm& pnm) {
    if (file.size() < 3 || file[0] != 'P' || (file[1] != '5' && file[1] != '6')) return false;
    pnm.channels = file[1] == '5' ? 1 : 3;
    size_t pos = 2;
    int fields[3] = {};
    for (int& field : fields) {
        while (pos < file.size() && (file[pos] == ' ' || file[pos] == '\n' || file[pos] == '\r' || file[pos] == '\t')) pos++;
        if (pos >= file.size() || file[pos] < '0' || file[pos] > '9') return false;
        while (pos < file.size() && file[pos] >= '0' && file[pos] <= '9') field = field * 10 + (file[pos++] - '0');
    }
    pos++;  // the single whitespace before the samples
    pnm.width = fields[0];
    pnm.height = fields[1];
    const size_t bytes = static_cast<size_t>(pnm.width) * static_cast<size_t>(pnm.height) * static_cast<size_t>(pnm.channels);
    if (fields[2] != 255 || file.size() < pos || file.size() - pos != bytes) return false;
    pnm.samples.assign(file.begin() + static_cast<std::ptrdiff_t>(pos), file.end());
    return true;
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

RGUI_TEST("jpeg.header") {
    struct Expected {
        const char* file;
        int width, height, components;
        bool progressive;
    };
    const Expected cases[] = {
        {"corpus/photo.jpg", 1024, 1024, 3, false},
        {"corpus/photo-progressive.jpg", 1024, 1024, 3, true},
        {"data/jpeg/gray.jpg", 157, 113, 1, false},
        {"data/jpeg/gray-progressive.jpg", 157, 113, 1, true},
        {"data/jpeg/cmyk.jpg", 157, 113, 4, false},
        {"data/jpeg/sof1-16bit-dqt.jpg", 157, 113, 3, false},
    };
    for (const Expected& e : cases) {
        const std::vector<uint8_t> jpeg = Load(e.file);
        JpegInfo info;
        std::string reason;
        CHECK(ReadJpegInfo(jpeg.data(), jpeg.size(), info, &reason));
        CHECK_EQ(info.width, e.width);
        CHECK_EQ(info.height, e.height);
        CHECK_EQ(info.components, e.components);
        CHECK_EQ(info.progressive, e.progressive);
        rgui::gfx::Image image;
        CHECK(DecodeJpeg(jpeg.data(), jpeg.size(), image, &reason));
        CHECK_EQ(image.width, e.width);
        CHECK_EQ(image.height, e.height);
        CHECK_EQ(image.pixels.size(), static_cast<size_t>(e.width) * static_cast<size_t>(e.height));
        CHECK(image.opaque);
    }
}

RGUI_TEST("jpeg.unsupported_rejected") {
    // Arithmetic coding (SOF9 + DAC) and 12-bit samples, both from cjpeg.
    const std::vector<uint8_t> arithmetic = Load("data/jpeg/arithmetic.jpg");
    const std::vector<uint8_t> twelve = Load("data/jpeg/12bit.jpg");
    JpegInfo info;
    std::string reason;
    CHECK(!ReadJpegInfo(arithmetic.data(), arithmetic.size(), info, &reason));
    CHECK_EQ(reason, std::string("jpeg-arithmetic"));
    CHECK_EQ(Reason(arithmetic), std::string("jpeg-arithmetic"));
    CHECK(!ReadJpegInfo(twelve.data(), twelve.size(), info, &reason));
    CHECK_EQ(reason, std::string("jpeg-12-bit"));
    CHECK_EQ(Reason(twelve), std::string("jpeg-12-bit"));

    // A frame height of 0 means the height follows in a DNL marker.
    std::vector<uint8_t> photo = Load("corpus/photo.jpg");
    const size_t at = FrameHeightOffset(photo);
    CHECK(at != 0);
    std::vector<uint8_t> dnl = photo;
    Put16(dnl, at, 0);
    CHECK_EQ(Reason(dnl), std::string("jpeg-dnl"));
    std::vector<uint8_t> noWidth = photo;
    Put16(noWidth, at + 2, 0);
    CHECK_EQ(Reason(noWidth), std::string("jpeg-dimensions"));
    // 65535 x 65535 is above 2^28 pixels; 16384 x 16384 is exactly 2^28 and allowed by size,
    // but 46 KB of data cannot code its 6 million blocks, so it is refused before allocating.
    std::vector<uint8_t> huge = photo;
    Put16(huge, at, 65535);
    Put16(huge, at + 2, 65535);
    CHECK_EQ(Reason(huge), std::string("jpeg-too-large"));
    std::vector<uint8_t> forged = photo;
    Put16(forged, at, 16384);
    Put16(forged, at + 2, 16384);
    CHECK(ReadJpegInfo(forged.data(), forged.size(), info, &reason));
    CHECK_EQ(info.width, 16384);
    CHECK_EQ(Reason(forged), std::string("jpeg-truncated"));

    // A soft mask must be one component.
    int width = 0;
    int height = 0;
    std::vector<uint8_t> gray;
    CHECK(!DecodeJpegGray(photo.data(), photo.size(), width, height, gray, &reason));
    CHECK_EQ(reason, std::string("jpeg-not-gray"));

    // Not a JPEG at all.
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    CHECK(!ReadJpegInfo(png, sizeof(png), info, &reason));
    CHECK_EQ(reason, std::string("jpeg-not-jpeg"));
}

RGUI_TEST("jpeg.truncated_fails") {
    // Every proper prefix fails (the EOI marker is required), with a reason, leaving the
    // output as it was: each length through the headers and the first scans, every third one
    // after, and each of the last 15 (cuts into the final scan and the EOI marker).
    for (const char* file : {"data/jpeg/gray-progressive.jpg", "data/jpeg/420-restart-rows.jpg",
                             "data/jpeg/444-jpegtran-progressive-restart.jpg"}) {
        const std::vector<uint8_t> jpeg = Load(file);
        CHECK(!jpeg.empty());
        int decoded = 0;
        for (size_t length = 0; length < jpeg.size(); length += (length < 1024 || length + 16 >= jpeg.size()) ? 1 : 3) {
            rgui::gfx::Image image;
            image.width = 7;
            std::string reason;
            if (DecodeJpeg(jpeg.data(), length, image, &reason)) {
                decoded++;
            } else {
                CHECK(!reason.empty());
                CHECK_EQ(image.width, 7);
            }
        }
        CHECK_EQ(decoded, 0);
        rgui::gfx::Image image;
        CHECK(DecodeJpeg(jpeg.data(), jpeg.size(), image, nullptr));
    }
}

RGUI_TEST("jpeg.progressive_matches_sequential") {
    // jpegtran -progressive and cjpeg -progressive at the same quality reorganise the same
    // quantized coefficients into spectral-selection and successive-approximation scans, so the
    // decoded pixels are identical to the sequential file's.
    const char* pairs[][2] = {
        {"data/jpeg/420-restart-rows.jpg", "data/jpeg/420-jpegtran-progressive.jpg"},
        {"data/jpeg/444.jpg", "data/jpeg/444-jpegtran-progressive-restart.jpg"},
        {"data/jpeg/gray.jpg", "data/jpeg/gray-progressive.jpg"},
    };
    for (const auto& pair : pairs) {
        const std::vector<uint8_t> a = Load(pair[0]);
        const std::vector<uint8_t> b = Load(pair[1]);
        rgui::gfx::Image imageA;
        rgui::gfx::Image imageB;
        CHECK(DecodeJpeg(a.data(), a.size(), imageA, nullptr));
        CHECK(DecodeJpeg(b.data(), b.size(), imageB, nullptr));
        CHECK_EQ(imageA.width, imageB.width);
        CHECK_EQ(imageA.height, imageB.height);
        CHECK(imageA.pixels == imageB.pixels);
    }
}

RGUI_TEST("jpeg.gray_plane") {
    for (const char* file : {"data/jpeg/gray.jpg", "data/jpeg/gray-progressive.jpg"}) {
        const std::vector<uint8_t> jpeg = Load(file);
        int width = 0;
        int height = 0;
        std::vector<uint8_t> gray;
        std::string reason;
        CHECK(DecodeJpegGray(jpeg.data(), jpeg.size(), width, height, gray, &reason));
        CHECK_EQ(width, 157);
        CHECK_EQ(height, 113);
        rgui::gfx::Image image;
        CHECK(DecodeJpeg(jpeg.data(), jpeg.size(), image, &reason));
        CHECK_EQ(gray.size(), image.pixels.size());
        bool same = gray.size() == image.pixels.size();
        for (size_t i = 0; same && i < gray.size(); i++) {
            same = image.pixels[i] == (0xFF000000u | uint32_t(gray[i]) * 0x010101u);
        }
        CHECK(same);
    }
}

RGUI_TEST("jpeg.mutations") {
    // 10,000 mutated or truncated copies of the corpus photographs: each decodes to an image of
    // the size its header gives or fails with a reason; none crashes (linux-asan) or runs long.
    // Most cases use 128x128 lossless crops of photo.jpg and photo-progressive.jpg (jpegtran
    // -crop: the same quantization, Huffman coding and scan structure, 1/64 of the blocks), so
    // the test takes seconds, not minutes, under the sanitizers; 100 cases use the full files.
    // Half of the edits go to the markers and tables before the first scan's data, where sizes
    // and tables are read; the others anywhere, the entropy-coded data included.
    struct Source {
        std::vector<uint8_t> bytes;
        size_t header = 0;  // the bytes up to the end of the first SOS segment
    };
    Source sources[] = {{Load("data/jpeg/photo-crop.jpg")}, {Load("data/jpeg/photo-progressive-crop.jpg")},
                        {Load("corpus/photo.jpg")}, {Load("corpus/photo-progressive.jpg")}};
    for (Source& source : sources) {
        CHECK(source.bytes.size() > 4);
        if (source.bytes.size() <= 4) return;
        size_t i = 2;
        while (i + 4 <= source.bytes.size() && source.bytes[i] == 0xFF) {
            const int marker = source.bytes[i + 1];
            i += 2 + ((size_t(source.bytes[i + 2]) << 8) | source.bytes[i + 3]);
            if (marker == 0xDA) break;
        }
        source.header = std::min(i, source.bytes.size());
    }
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    const auto start = std::chrono::steady_clock::now();
    double slowest = 0;
    int decoded = 0;
    const int kCases = 10000;
    const int kFullCases = 100;
    for (int n = 0; n < kCases; n++) {
        const Source& source = sources[(n < kCases - kFullCases ? 0 : 2) + (n & 1)];
        std::vector<uint8_t> jpeg = source.bytes;
        const uint64_t kind = next() % 10;
        if (kind < 2) {
            jpeg.resize(static_cast<size_t>(next() % jpeg.size()));
        } else {
            const int edits = 1 + static_cast<int>(next() % 4);
            for (int e = 0; e < edits; e++) {
                const size_t range = kind < 6 ? source.header : jpeg.size();
                jpeg[static_cast<size_t>(next() % range)] = static_cast<uint8_t>(next());
            }
        }
        const auto one = std::chrono::steady_clock::now();
        rgui::gfx::Image image;
        std::string reason;
        if (DecodeJpeg(jpeg.data(), jpeg.size(), image, &reason)) {
            decoded++;
            JpegInfo info;
            CHECK(ReadJpegInfo(jpeg.data(), jpeg.size(), info, nullptr));
            CHECK_EQ(image.pixels.size(), static_cast<size_t>(info.width) * static_cast<size_t>(info.height));
        } else {
            CHECK(!reason.empty());
        }
        const double ms = MillisecondsSince(one);
        if (ms > slowest) slowest = ms;
    }
    std::printf("jpeg.mutations: %d cases, %d decoded, %.0f ms total, slowest %.1f ms\n", kCases, decoded,
                MillisecondsSince(start), slowest);
}

RGUI_TEST("jpeg.djpeg") {
    // libjpeg-turbo's djpeg as the reference: the same integer IDCT and replicated chroma, so
    // the mean absolute difference must stay within 1.5 / 255 per channel (it is 0 where our
    // arithmetic matches libjpeg's exactly).
    const char* djpeg = std::getenv("RGUI_DJPEG");
    if (!djpeg || !*djpeg) {
        std::printf("skip jpeg.djpeg: RGUI_DJPEG not set\n");
        return;
    }
    const fs::path dir = fs::path(rgui::test::BuildDir()) / "jpeg-test";
    std::error_code error;
    fs::create_directories(dir, error);
    for (const fs::path& file : SupportedFiles()) {
        const std::string name = file.filename().string();
        const fs::path reference = dir / (file.stem().string() + ".pnm");
        const std::string command = std::string("\"") + djpeg + "\" -ppm -dct int -nosmooth -outfile \"" +
                                    reference.string() + "\" \"" + file.string() + "\"";
        CHECK_EQ(std::system(command.c_str()), 0);
        std::string pnmBytes;
        Pnm pnm;
        CHECK(RayoMd::File::ReadAll(reference, 1u << 30, pnmBytes));
        CHECK(ParsePnm(pnmBytes, pnm));

        std::vector<uint8_t> jpeg;
        CHECK(RayoMd::File::ReadAll(file, 16u << 20, jpeg));
        rgui::gfx::Image image;
        std::string reason;
        double best = 1e9;
        for (int run = 0; run < 5; run++) {
            const auto start = std::chrono::steady_clock::now();
            const bool ok = DecodeJpeg(jpeg.data(), jpeg.size(), image, &reason);
            const double ms = MillisecondsSince(start);
            CHECK(ok);
            if (!ok) break;
            if (ms < best) best = ms;
        }
        CHECK_EQ(image.width, pnm.width);
        CHECK_EQ(image.height, pnm.height);
        if (image.width != pnm.width || image.height != pnm.height) continue;

        uint64_t sum[3] = {};
        int worst = 0;
        const size_t pixels = image.pixels.size();
        for (size_t i = 0; i < pixels; i++) {
            const uint32_t p = image.pixels[i];
            const int ours[3] = {int((p >> 16) & 255), int((p >> 8) & 255), int(p & 255)};
            for (int c = 0; c < 3; c++) {
                const int ref = pnm.samples[pnm.channels == 1 ? i : i * 3 + static_cast<size_t>(c)];
                const int d = ours[c] > ref ? ours[c] - ref : ref - ours[c];
                sum[c] += static_cast<uint64_t>(d);
                if (d > worst) worst = d;
            }
        }
        double mad[3];
        for (int c = 0; c < 3; c++) {
            mad[c] = static_cast<double>(sum[c]) / static_cast<double>(pixels);
            CHECK(mad[c] <= 1.5);
        }
        if (pnm.channels == 1) {
            int width = 0;
            int height = 0;
            std::vector<uint8_t> gray;
            CHECK(DecodeJpegGray(jpeg.data(), jpeg.size(), width, height, gray, &reason));
            CHECK(gray == pnm.samples);
        }
        std::printf("jpeg.djpeg %-38s %4dx%-4d MAD R %.4f G %.4f B %.4f (/255), max %d, decode %.2f ms\n",
                    name.c_str(), image.width, image.height, mad[0], mad[1], mad[2], worst, best);
    }
}
