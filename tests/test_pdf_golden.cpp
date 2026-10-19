// SPDX-License-Identifier: Apache-2.0
// The preview renderer against poppler (pdftocairo), the independent reference: every page of a
// corpus of the engine's PDFs, at 2 px/pt, compared in luminance after a 4x box downscale.
#include "runner.h"

#include "pdf_testing.h"

#include "common/exact_math.h"
#include "common/file_io.h"
#include "core/export_options.h"
#include "core/inflate.h"
#include "core/png_filter.h"
#include "gfx/png_write.h"
#include "pdf/reader.h"
#include "pdf/renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using rgui::pdf::PdfFile;
using rgui::pdf::RenderStats;

double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Luminance 0..255 of each pixel, rows top to bottom.
struct Luma {
    int width = 0;
    int height = 0;
    std::vector<double> values;
};

double Luminance(double r, double g, double b) {
    return (299 * r + 587 * g + 114 * b) / 1000.0;
}

// An 8-bit RGB or RGBA PNG (RGBA over white), as pdftocairo writes them.
bool ReadPng(const std::string& path, Luma& out) {
    std::string file;
    if (!RayoMd::File::ReadAll(RayoMd::File::PathFromUtf8(path), 1u << 28, file) || file.size() < 8 || file.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) {
        return false;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(file.data());
    size_t at = 8;
    int width = 0, height = 0, channels = 0;
    std::string idat;
    while (at + 12 <= file.size()) {
        const uint32_t length = Be32(data + at);
        const std::string_view type(file.data() + at + 4, 4);
        if (length > file.size() - at - 12) return false;
        const uint8_t* body = data + at + 8;
        if (type == "IHDR" && length >= 13) {
            width = static_cast<int>(Be32(body));
            height = static_cast<int>(Be32(body + 4));
            if (body[8] != 8 || body[12] != 0 || (body[9] != 2 && body[9] != 6)) return false;
            channels = body[9] == 2 ? 3 : 4;
        } else if (type == "IDAT") {
            idat.append(reinterpret_cast<const char*>(body), length);
        } else if (type == "IEND") {
            break;
        }
        at += 12 + length;
    }
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536) return false;
    const size_t rowBytes = static_cast<size_t>(width) * channels;
    RayoMd::Inflate::ZlibReader reader;
    if (!reader.Start(reinterpret_cast<const uint8_t*>(idat.data()), idat.size(), (rowBytes + 1) * height, rowBytes + 1)) {
        return false;
    }
    std::vector<uint8_t> up(rowBytes, 0), row(rowBytes);
    out.width = width;
    out.height = height;
    out.values.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        const uint8_t* raw = reader.Take(rowBytes + 1);
        if (!raw || !TinyPdf::Internal::UnfilterPngRow(raw[0], raw + 1, up.data(), rowBytes, channels, row.data())) {
            return false;
        }
        for (int x = 0; x < width; x++) {
            const uint8_t* p = row.data() + static_cast<size_t>(x) * channels;
            double r = p[0], g = p[1], b = p[2];
            if (channels == 4) {
                const double a = p[3] / 255.0;
                r = r * a + 255 * (1 - a);
                g = g * a + 255 * (1 - a);
                b = b * a + 255 * (1 - a);
            }
            out.values[static_cast<size_t>(y) * width + x] = Luminance(r, g, b);
        }
        up.swap(row);
    }
    return reader.Finish();
}

Luma FromImage(const rgui::gfx::Image& image) {
    Luma out;
    out.width = image.width;
    out.height = image.height;
    out.values.resize(image.pixels.size());
    for (size_t i = 0; i < image.pixels.size(); i++) {
        const uint32_t p = image.pixels[i];
        out.values[i] = Luminance((p >> 16) & 255, (p >> 8) & 255, p & 255);
    }
    return out;
}

// Means of 4x4 blocks over the common area.
std::vector<double> Downscale(const Luma& luma, int width4, int height4) {
    std::vector<double> out(static_cast<size_t>(width4) * height4);
    for (int y = 0; y < height4; y++) {
        for (int x = 0; x < width4; x++) {
            double sum = 0;
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) sum += luma.values[static_cast<size_t>(y * 4 + dy) * luma.width + x * 4 + dx];
            }
            out[static_cast<size_t>(y) * width4 + x] = sum / 16;
        }
    }
    return out;
}

struct Document {
    std::string name;
    std::string markdown;  // path under the source tree
    std::function<bool(TinyPdf::PdfOptions&)> setup;  // false: skip the document
};

std::string Source(const std::string& relative) {
    return std::string(rgui::test::SourceDir()) + "/" + relative;
}

} // namespace

RGUI_TEST("pdf.golden") {
    const char* tool = std::getenv("RGUI_PDFTOCAIRO");
    if (!tool || !*tool) {
        std::printf("skip pdf.golden: RGUI_PDFTOCAIRO not set\n");
        return;
    }
    const std::filesystem::path dir = std::filesystem::path(rgui::test::BuildDir()) / "golden";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string sourceSans = "/usr/share/fonts/adobe-source-sans/SourceSans3-Regular.otf";
    const auto none = [](TinyPdf::PdfOptions&) { return true; };
    const std::vector<Document> documents = {
        {"tester", "external/rayomd/tester.md", none},
        {"ascii", "tests/corpus/ascii.md", none},
        {"native_math", "external/rayomd/docs/development/native_math.md", none},
        {"svg", "tests/corpus/svg.md", none},
        {"photo", "tests/corpus/photo.md",
         [](TinyPdf::PdfOptions& o) {
             o.allowUnsafeLocalImages = true;  // ../../assets/rayomd.png
             return true;
         }},
        {"source-sans", "external/rayomd/tester.md",
         [&](TinyPdf::PdfOptions& o) {
             std::string error;
             if (!std::filesystem::exists(sourceSans)) return false;
             CHECK(TinyPdf::Internal::ParseTheme("font = " + sourceSans + "\nheader-left = {title}\n", "", o.theme, error));
             return true;
         }},
        {"pdfua", "external/rayomd/tester.md", [](TinyPdf::PdfOptions& o) { return o.pdfua = true; }},
        {"pdfa", "external/rayomd/tester.md", [](TinyPdf::PdfOptions& o) { return o.pdfa = true; }},
        {"toc-numbers", "external/rayomd/tester.md",
         [](TinyPdf::PdfOptions& o) {
             o.toc = true;
             o.pageNumbers = true;
             return true;
         }},
        {"letter-landscape", "external/rayomd/tester.md",
         [](TinyPdf::PdfOptions& o) {
             o.pageSize = {TinyPdf::PdfPageSize::Letter().height, TinyPdf::PdfPageSize::Letter().width};
             return true;
         }},
    };
    for (const Document& doc : documents) {
        TinyPdf::PdfOptions options;
        if (!doc.setup(options)) {
            std::printf("golden %s: skipped (a font it needs is missing)\n", doc.name.c_str());
            continue;
        }
        const std::string path = Source(doc.markdown);
        std::string markdown;
        CHECK(RayoMd::File::ReadAll(RayoMd::File::PathFromUtf8(path), 1u << 24, markdown));
        options.sourcePath = path;
        std::string pdf = rgui::pdftest::BuildPreview(markdown, options);
        CHECK(!pdf.empty());
        const std::string pdfPath = (dir / (doc.name + ".pdf")).string();
        CHECK(RayoMd::File::WriteAll(RayoMd::File::PathFromUtf8(pdfPath), pdf));
        const auto file = PdfFile::Parse(std::move(pdf), nullptr);
        CHECK(file != nullptr);
        if (!file) continue;
        for (int page = 0; page < file->PageCount(); page++) {
            const rgui::pdf::PdfRect box = file->PageMediaBox(page);
            const int width = static_cast<int>(RayoMd::ExactMath::LRound(box.Width() * 2));
            rgui::gfx::Image ours;
            RenderStats stats;
            CHECK(rgui::pdf::RenderPage(*file, page, width, ours, &stats, rgui::pdftest::TestEnvironment()));
            const std::string prefix = (dir / (doc.name + "-" + std::to_string(page + 1))).string();
            const std::string command = std::string("\"") + tool + "\" -png -r 144 -f " + std::to_string(page + 1) +
                                        " -l " + std::to_string(page + 1) + " -singlefile \"" + pdfPath + "\" \"" +
                                        prefix + "\"";
            CHECK_EQ(std::system(command.c_str()), 0);
            Luma reference;
            CHECK(ReadPng(prefix + ".png", reference));
            const Luma mine = FromImage(ours);
            CHECK(std::abs(reference.width - mine.width) <= 1 && std::abs(reference.height - mine.height) <= 1);
            const int w4 = std::min(reference.width, mine.width) / 4, h4 = std::min(reference.height, mine.height) / 4;
            if (w4 <= 0 || h4 <= 0) continue;
            const std::vector<double> a = Downscale(reference, w4, h4), b = Downscale(mine, w4, h4);
            double sum = 0;
            size_t far = 0;
            for (size_t i = 0; i < a.size(); i++) {
                const double d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
                sum += d;
                far += d > 48;
            }
            const double mad = sum / a.size();
            const double farPercent = 100.0 * far / a.size();
            std::string unknown;
            for (const std::string& what : stats.unknown) unknown += " " + what;
            std::printf("golden %-16s page %d: MAD %.3f/255, >48 %.3f %%%s%s\n", doc.name.c_str(), page + 1, mad,
                        farPercent, unknown.empty() ? "" : ", unknown:", unknown.c_str());
            CHECK(mad <= 3.0);
            CHECK(farPercent <= 0.5);
            CHECK(stats.unknown.empty());
            if (mad > 3.0 || farPercent > 0.5 || !stats.unknown.empty()) {
                rgui::gfx::Image copy = ours;
                rgui::gfx::WritePng(prefix + "-ours.png", copy.AsSurface());
            }
        }
    }

    // Preview timings: tester.md built, parsed and page 1 rendered at 1200 px, on a thread of its
    // own with caches of its own (the first render), then a second build of the same text (what
    // typing does: the fonts and glyphs of the first are reused).
    std::thread([] {
        const std::string path = Source("external/rayomd/tester.md");
        std::string markdown;
        RayoMd::File::ReadAll(RayoMd::File::PathFromUtf8(path), 1u << 24, markdown);
        TinyPdf::PdfOptions options;
        options.sourcePath = path;
        rgui::pdf::ImageCache images;
        rgui::pdf::FontCache fonts;
        const rgui::pdf::RenderEnvironment environment{&rgui::pdftest::TestFontIndex(), &images, &fonts};
        for (const char* run : {"first", "again"}) {
            auto start = std::chrono::steady_clock::now();
            std::string pdf = rgui::pdftest::BuildPreview(markdown, options);
            const double build = MsSince(start);
            start = std::chrono::steady_clock::now();
            const auto file = PdfFile::Parse(std::move(pdf), nullptr);
            const double parse = MsSince(start);
            if (!file) return;
            rgui::gfx::Image image;
            start = std::chrono::steady_clock::now();
            rgui::pdf::RenderPage(*file, 0, 1200, image, nullptr, environment);
            const double render = MsSince(start);
            std::printf("golden timing (%s): tester.md build %.2f ms, parse %.2f ms, page 1 at 1200 px %.2f ms\n", run,
                        build, parse, render);
        }
        // Re-renders of every page at 1200 px with warm caches (what scrolling and an edit that
        // changed a page cost): the best of 5 per page.
        std::string pdf = rgui::pdftest::BuildPreview(markdown, options);
        const auto file = PdfFile::Parse(std::move(pdf), nullptr);
        if (!file) return;
        std::string line;
        for (int page = 0; page < file->PageCount(); page++) {
            double best = 1e9;
            for (int i = 0; i < 5; i++) {
                rgui::gfx::Image image;
                const auto start = std::chrono::steady_clock::now();
                rgui::pdf::RenderPage(*file, page, 1200, image, nullptr, environment);
                best = std::min(best, MsSince(start));
            }
            char item[32];
            std::snprintf(item, sizeof item, " %d:%.2f", page + 1, best);
            line += item;
        }
        std::printf("golden timing (warm): tester.md pages at 1200 px, ms:%s\n", line.c_str());
    }).join();
}
