// SPDX-License-Identifier: Apache-2.0
// What a hostile or merely huge page may cost the preview: per-page limits on glyphs, geometry,
// dashes, pixels, clip masks and decoded images, cancellation of renders no longer wanted, the
// JPEG decoder's work bound, and the hash keys of the caches. Each page here is crafted to cost
// far more than its bytes; each test asserts the counters RenderStats keeps and a time bound.
#include "runner.h"

#include "headless_platform.h"
#include "pdf_testing.h"

#include "app/previewpages.h"
#include "common/file_io.h"
#include "core/flate.h"
#include "pdf/images.h"
#include "pdf/jpeg.h"
#include "pdf/reader.h"
#include "pdf/renderer.h"
#include "text/glyphcache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using rgui::gfx::Image;
using rgui::pdf::PdfFile;
using rgui::pdf::PdfHash;
using rgui::pdf::PdfValue;
using rgui::pdf::RenderLimits;
using rgui::pdf::RenderPage;
using rgui::pdf::RenderStats;
using rgui::pdftest::MakePdf;
using rgui::pdftest::PagePdf;
using rgui::pdftest::Stream;
using rgui::pdftest::TestEnvironment;
using Clock = std::chrono::steady_clock;

// Time bounds are for optimized builds; sanitizers run about ten times slower.
#if defined(__SANITIZE_ADDRESS__)
constexpr double kSlow = 10;
#else
constexpr double kSlow = 1;
#endif

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool Has(const RenderStats& stats, const std::string& what) {
    return std::find(stats.unknown.begin(), stats.unknown.end(), what) != stats.unknown.end();
}

// Page 0 of `pdf` rendered at `width` px; the time it took in `ms`.
Image Render(const std::string& pdf, int width, RenderStats& stats, double& ms,
             const rgui::pdf::RenderEnvironment& environment = TestEnvironment()) {
    Image image;
    const auto file = PdfFile::Parse(pdf, nullptr);
    CHECK(file != nullptr);
    const auto start = Clock::now();
    if (file) CHECK(RenderPage(*file, 0, width, image, &stats, environment));
    ms = MsSince(start);
    return image;
}

void Print(const char* what, const RenderStats& stats, double ms) {
    std::string unknown;
    for (const std::string& u : stats.unknown) unknown += " " + u;
    std::printf("%s: %.1f ms, %u ops, %u glyphs (%u culled, %u outlines), geometry %llu, pixels %llu, dashes %llu, "
                "images %llu bytes, masks %llu bytes, glyph cache %llu bytes%s%s\n",
                what, ms, stats.operators, stats.glyphs, stats.glyphsCulled, stats.glyphOutlines,
                static_cast<unsigned long long>(stats.geometry), static_cast<unsigned long long>(stats.pixels),
                static_cast<unsigned long long>(stats.dashes), static_cast<unsigned long long>(stats.imageBytes),
                static_cast<unsigned long long>(stats.maskBytes), static_cast<unsigned long long>(stats.glyphCacheBytes),
                unknown.empty() ? "" : ", unknown:", unknown.c_str());
}

std::string Compress(const std::string& data) {
    std::vector<uint8_t> out(RayoMd::Flate::ZlibBound(data.size()));
    RayoMd::Flate::Scratch scratch;
    const size_t n = RayoMd::Flate::ZlibCompress(data, out.data(), scratch);
    return std::string(reinterpret_cast<const char*>(out.data()), n);
}

const char kHelvetica[] = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                          "/Encoding /WinAnsiEncoding >> >> >>";

bool NoHelvetica(const char* test) {
    RenderStats stats;
    double ms = 0;
    Render(PagePdf("BT /F1 10 Tf (H) Tj ET", kHelvetica), 20, stats, ms);
    if (Has(stats, "font:no-substitute:Helvetica")) {
        std::printf("skip %s: no face for Helvetica (Liberation Sans, Arimo, ...)\n", test);
        return true;
    }
    return false;
}

// The page of 30 half-transparent page-size fills at 4000 px: ~0.5 G blended pixels, within the
// pixel limit, slow on purpose.
std::string HeavyPage() {
    std::string content;
    for (int i = 0; i < 30; i++) content += "q /G gs 0." + std::to_string(10 + i) + " g 0 0 100 100 re f Q\n";
    return PagePdf(content, "<< /ExtGState << /G << /ca 0.5 >> >> >>");
}

// A Flate-compressed DeviceGray image of `size` x `size` pixels of value `gray`.
std::string GrayImage(int size, int gray) {
    const std::string samples(static_cast<size_t>(size) * static_cast<size_t>(size), static_cast<char>(gray));
    return Stream(Compress(samples), " /Type /XObject /Subtype /Image /Width " + std::to_string(size) + " /Height " +
                                         std::to_string(size) +
                                         " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode");
}

// Writes bits most significant first, with JPEG's 0xFF 0x00 stuffing; pads with ones.
struct JpegBits {
    std::string out;
    int count = 0;
    unsigned current = 0;
    void Put(unsigned value, int bits) {
        for (int i = bits - 1; i >= 0; i--) {
            current = (current << 1) | ((value >> i) & 1);
            if (++count == 8) Flush();
        }
    }
    void Flush() {
        out.push_back(static_cast<char>(current));
        if (current == 0xFF) out.push_back('\0');
        current = 0;
        count = 0;
    }
    void Finish() {
        while (count != 0) Put(1, 1);
    }
};

std::string Segment(int marker, const std::string& body) {
    std::string s;
    s.push_back(static_cast<char>(0xFF));
    s.push_back(static_cast<char>(marker));
    const size_t n = body.size() + 2;
    s.push_back(static_cast<char>(n >> 8));
    s.push_back(static_cast<char>(n));
    return s + body;
}

// A progressive gray JPEG of `size` x `size` whose `refines` AC refinement scans are a few bytes
// each: every block of every scan is one end-of-band run, so the decoder's work grows with
// size^2 x refines while the file stays tiny.
std::vector<uint8_t> ProgressiveBomb(int size, int refines) {
    std::string jpeg = "\xFF\xD8";
    jpeg += Segment(0xDB, std::string(1, '\0') + std::string(64, '\x01'));
    std::string frame = "\x08";
    frame += static_cast<char>(size >> 8);
    frame += static_cast<char>(size);
    frame += static_cast<char>(size >> 8);
    frame += static_cast<char>(size);
    frame += std::string("\x01\x01\x11\x00", 4);
    jpeg += Segment(0xC2, frame);
    std::string counts(16, '\0');
    counts[0] = 1;  // one code of one bit: "0"
    jpeg += Segment(0xC4, std::string(1, '\x00') + counts + std::string(1, '\x00'));  // DC 0: symbol 0
    jpeg += Segment(0xC4, std::string(1, '\x10') + counts + std::string(1, '\xE0'));  // AC 0: EOBRUN 2^14 + 14 bits
    const int blocks = (size / 8) * (size / 8);
    // The first DC scan: one "0" bit per block (no difference).
    jpeg += Segment(0xDA, std::string("\x01\x01\x00\x00\x00\x00", 6));
    JpegBits dc;
    for (int i = 0; i < blocks; i++) dc.Put(0, 1);
    dc.Finish();
    jpeg += dc.out;
    for (int s = 0; s < refines; s++) {
        jpeg += Segment(0xDA, std::string("\x01\x01\x00\x01\x3F\x10", 6));  // Ss 1, Se 63, Ah 1, Al 0
        JpegBits ac;
        for (int covered = 0; covered < blocks; covered += 32767) ac.Put((0u << 14) | 0x3FFF, 15);
        ac.Finish();
        jpeg += ac.out;
    }
    jpeg += "\xFF\xD9";
    return std::vector<uint8_t>(jpeg.begin(), jpeg.end());
}

} // namespace

// ---- hashes ------------------------------------------------------------------------------

// An object hashed through a cycle cut elsewhere in the same walk depends on that walk: it is
// not kept for the file, so a page's key is the same whatever page was keyed first.
RGUI_TEST("pdf.page_key_walk_order") {
    const std::string pdf = MakePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 10 10] /Resources << /Properties << /P1 6 0 R /P2 8 0 R >> >> "
        "/Contents 5 0 R >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 10 10] /Resources << /Properties << /P2 8 0 R >> >> "
        "/Contents 5 0 R >>",
        Stream(""),
        "<< /X 7 0 R >>",
        "<< /Y 6 0 R >>",
        "<< /Z 7 0 R >>",
    });
    const auto fresh = PdfFile::Parse(pdf, nullptr);
    const auto after = PdfFile::Parse(pdf, nullptr);
    CHECK(fresh && after);
    if (!fresh || !after) return;
    const PdfHash alone = fresh->PageKey(1);
    after->PageKey(0);  // walks 6 -> 7 -> 6 (cut), then 8 -> 7 (this walk's hash of 7)
    CHECK(after->PageKey(1) == alone);
    CHECK(after->PageKey(0) != alone);
}

// Cache keys are compared in full: two keys with the same low half are different images.
RGUI_TEST("pdf.image_cache_full_key") {
    rgui::pdf::ImageCache cache;
    auto image = std::make_shared<Image>();
    image->width = image->height = 1;
    image->pixels = {0xFF000000u};
    CHECK(cache.Insert(PdfHash{1, 2}, 0, image));
    CHECK(cache.Find(PdfHash{1, 2}, 0) == image);
    CHECK(cache.Find(PdfHash{1, 3}, 0) == nullptr);
    CHECK(cache.Find(PdfHash{1, 2}, 1) == nullptr);
    // Keys of real values use both halves.
    const auto file = PdfFile::Parse(PagePdf("0 0 10 10 re f"), nullptr);
    CHECK(file != nullptr);
    if (!file) return;
    const PdfHash a = file->ValueHash(file->Object(3)), b = file->ValueHash(file->Object(4));
    CHECK(a != b && a.lo != b.lo && a.hi != b.hi);
}

// ---- glyphs ------------------------------------------------------------------------------

// 20,000 distinct sizes of one glyph: the glyph cache stays under its ceiling, the pixels the
// rasterization costs stop the page; off the page none is rasterized at all.
RGUI_TEST("pdf.limits_glyph_sizes") {
    if (NoHelvetica("pdf.limits_glyph_sizes")) return;
    auto page = [](const char* at) {
        std::string content;
        content.reserve(20000 * 48);
        char op[96];
        for (int i = 0; i < 20000; i++) {
            std::snprintf(op, sizeof op, "BT /F1 %.3f Tf %s Td (W) Tj ET\n", 1.0 + i * 0.001, at);
            content += op;
        }
        return PagePdf(content, kHelvetica);
    };
    RenderStats stats;
    double ms = 0;
    Render(page("10 50"), 1200, stats, ms);
    Print("on page", stats, ms);
    CHECK(Has(stats, "budget"));
    CHECK(stats.glyphCacheBytes <= 2 * rgui::text::GlyphCache::kDefaultBudget);
    CHECK(stats.pixels <= RenderLimits::kPixelsPerPagePixel * 1200 * 1200 + RenderLimits::kPixelsBase);
    CHECK(ms < 2000 * kSlow);

    RenderStats off;
    Render(page("10 -5000"), 1200, off, ms);
    Print("off page", off, ms);
    CHECK_EQ(off.glyphsCulled, uint32_t(20000));
    CHECK(off.unknown.empty());
    CHECK(off.pixels < 1000);
    CHECK(ms < 1000 * kSlow);

    // Above 256 px per em a glyph is filled as an outline, not cached.
    RenderStats big;
    Render(PagePdf("BT /F1 40 Tf 10 50 Td (W) Tj ET", kHelvetica), 1200, big, ms);
    CHECK_EQ(big.glyphs, uint32_t(1));
    CHECK_EQ(big.glyphOutlines, uint32_t(1));
}

// The limits leave real pages alone: every page of tester.md (text, tables, code, math, an
// image, SVG) at 1200 px stays under a quarter of each.
RGUI_TEST("pdf.limits_tester_margin") {
    std::string markdown;
    const std::string path = std::string(rgui::test::SourceDir()) + "/external/rayomd/tester.md";
    CHECK(RayoMd::File::ReadAll(path, 1u << 24, markdown));
    TinyPdf::PdfOptions options;
    options.sourcePath = path;
    const auto file = PdfFile::Parse(rgui::pdftest::BuildPreview(markdown, options), nullptr);
    CHECK(file != nullptr);
    if (!file) return;
    for (int page = 0; page < file->PageCount(); page++) {
        Image image;
        RenderStats stats;
        const auto start = Clock::now();
        CHECK(RenderPage(*file, page, 1200, image, &stats, TestEnvironment()));
        const double ms = MsSince(start);
        char what[32];
        std::snprintf(what, sizeof what, "tester.md page %d", page + 1);
        Print(what, stats, ms);
        const uint64_t pixels = static_cast<uint64_t>(image.width) * static_cast<uint64_t>(image.height);
        CHECK(stats.unknown.empty());
        CHECK(stats.operators < RenderLimits::kOperators / 4);
        CHECK(stats.glyphs < RenderLimits::kGlyphs / 4);
        CHECK(stats.geometry < RenderLimits::kGeometry / 4);
        CHECK(stats.dashes < RenderLimits::kDashes / 4);
        CHECK(stats.pixels < (RenderLimits::kPixelsPerPagePixel * pixels + RenderLimits::kPixelsBase) / 4);
        CHECK(stats.imageBytes < RenderLimits::kImageBytes / 4);
        CHECK(stats.maskBytes < RenderLimits::kMaskPerPagePixel * pixels / 4);
    }
}

// ---- geometry ----------------------------------------------------------------------------

// Curves far outside the page are flattened as chords; curves that cross it are paid for and
// stop the page at the geometry limit.
RGUI_TEST("pdf.limits_geometry") {
    std::string offClip;
    offClip.reserve(20000 * 64);
    char op[128];
    for (int i = 0; i < 20000; i++) {
        std::snprintf(op, sizeof op, "%d 100000 m 300000 -200000 300000 400000 %d 100500 c\n", 1000 + i, 1000 + i);
        offClip += op;
    }
    offClip += "f\n";
    RenderStats stats;
    double ms = 0;
    Render(PagePdf(offClip), 1200, stats, ms);
    Print("off-clip curves", stats, ms);
    CHECK(stats.unknown.empty());
    CHECK(stats.geometry < 50000);  // 256 pieces each before: 5.1 M segments
    CHECK(ms < 500 * kSlow);

    // The same curves stroked, then across the page: the stroke's reach keeps them coarse.
    RenderStats stroked;
    Render(PagePdf("2 w " + offClip.substr(0, offClip.size() - 2) + "S\n"), 1200, stroked, ms);
    Print("off-clip strokes", stroked, ms);
    CHECK(stroked.geometry < 100000);
    CHECK(ms < 500 * kSlow);

    std::string crossing;
    crossing.reserve(20000 * 48);
    for (int i = 0; i < 20000; i++) crossing += "50 50 m 50 100000 50.01 100000 50 50 c f\n";
    RenderStats visible;
    Render(PagePdf(crossing), 1200, visible, ms);
    Print("crossing curves", visible, ms);
    CHECK(Has(visible, "geometry"));
    CHECK(visible.geometry <= RenderLimits::kGeometry);
    CHECK(ms < 2000 * kSlow);
}

// Dense dashes stop at the dash limit; dashed lines off the page make none.
RGUI_TEST("pdf.limits_dashes") {
    std::string content = "[0.01 0.01] 0 d 0.5 w\n";
    std::string off = content;
    for (int i = 0; i < 1000; i++) {
        const std::string y = std::to_string(i % 100) + "." + std::to_string(i % 7);
        content += "0 " + y + " m 100 " + y + " l S\n";
        off += "0 -" + std::to_string(500 + i) + " m 100 -" + std::to_string(500 + i) + " l S\n";
    }
    RenderStats stats;
    double ms = 0;
    Render(PagePdf(content), 1200, stats, ms);
    Print("dense dashes", stats, ms);
    CHECK(Has(stats, "budget"));
    CHECK(stats.dashes <= RenderLimits::kDashes);
    CHECK(ms < 1000 * kSlow);  // ~1.7 s before the dash limit (100 lines: 173 ms)

    RenderStats offPage;
    Render(PagePdf(off), 1200, offPage, ms);
    Print("dashes off the page", offPage, ms);
    CHECK(offPage.unknown.empty());
    CHECK_EQ(offPage.dashes, uint64_t(0));
    CHECK(ms < 200 * kSlow);
}

// ---- clips -------------------------------------------------------------------------------

// A clip set again and again without q multiplies into one mask; nested q levels stop adding
// masks at the page's mask limit.
RGUI_TEST("pdf.limits_clip_stack") {
    const uint64_t page = 1200ull * 1200ull;
    std::string flat;
    for (int i = 0; i < 300; i++) flat += "0.37 0.41 99.1 98.3 re W n\n";
    flat += "0 0 1 rg 0 0 100 100 re f\n";
    // At 1200 px each clip rasterizes 1.2 M pixels: the page stops at its pixel limit, holding
    // one mask (300 before).
    RenderStats stats;
    double ms = 0;
    Render(PagePdf(flat), 1200, stats, ms);
    Print("300 clips at one level", stats, ms);
    CHECK(stats.maskBytes <= page);
    CHECK(ms < 1000 * kSlow);
    std::string few;
    for (int i = 0; i < 30; i++) few += "0.37 0.41 99.1 98.3 re W n\n";
    few += "0 0 1 rg 0 0 100 100 re f\n";
    RenderStats small;
    const Image image = Render(PagePdf(few), 300, small, ms);
    CHECK(small.unknown.empty());
    CHECK((image.pixels[150 * 300 + 150] & 0xFFFFFF) == 0x0000FF);
    CHECK((image.pixels[150 * 300] & 0xFFFFFF) == 0xFFFFFF);  // left of x = 0.37 pt

    std::string deep;
    for (int i = 0; i < 100; i++) deep += "q 0.37 0.41 99.1 98.3 re W n\n";
    deep += "0 0 1 rg 0 0 100 100 re f\n";
    for (int i = 0; i < 100; i++) deep += "Q\n";
    deep += "1 0 0 rg 0 0 100 100 re f\n";  // every clip restored
    RenderStats nested;
    Render(PagePdf(deep), 1200, nested, ms);
    Print("100 nested clips at 1200 px", nested, ms);
    CHECK(Has(nested, "clip-depth"));  // the mask bytes
    CHECK(nested.maskBytes <= RenderLimits::kMaskPerPagePixel * page + RenderLimits::kMaskBase + page);
    CHECK(ms < 1000 * kSlow);
    RenderStats shallow;
    const Image after = Render(PagePdf(deep), 300, shallow, ms);
    Print("100 nested clips at 300 px", shallow, ms);
    CHECK(shallow.unknown.empty());  // q past 64 is ignored, so its clips go in place
    CHECK(shallow.maskBytes <= RenderLimits::kClipDepth * 300 * 300);
    CHECK((after.pixels[2] & 0xFFFFFF) == 0xFF0000);

    // Two clips at one level: the intersection, as when they were pushed.
    RenderStats two;
    const Image both = Render(PagePdf("0 0 50.3 100 re W n 25.2 0 50 100 re W n 0 0 1 rg 0 0 100 100 re f"), 100, two, ms);
    CHECK((both.pixels[50 * 100 + 10] & 0xFFFFFF) == 0xFFFFFF);
    CHECK((both.pixels[50 * 100 + 40] & 0xFFFFFF) == 0x0000FF);
    CHECK((both.pixels[50 * 100 + 60] & 0xFFFFFF) == 0xFFFFFF);
}

// ---- images ------------------------------------------------------------------------------

// A large image drawn small is decoded at the level it is drawn at and kept that small; the
// decode cost of a page is limited; a JPEG soft mask obeys the pixel limit.
RGUI_TEST("pdf.limits_images") {
    rgui::pdf::ImageCache images;
    rgui::pdf::FontCache fonts;
    const rgui::pdf::RenderEnvironment environment{&rgui::pdftest::TestFontIndex(), &images, &fonts};
    RenderStats stats;
    double ms = 0;
    const Image page = Render(PagePdf("q 50 0 0 50 25 25 cm /I Do Q", "<< /XObject << /I 5 0 R >> >>", 100, 100,
                                      {GrayImage(4096, 0x80)}),
                              1200, stats, ms, environment);
    Print("4096^2 image drawn at 600 px", stats, ms);
    CHECK(stats.unknown.empty());
    CHECK_EQ(stats.imageBytes, uint64_t(4) * 4096 * 4096);
    CHECK_EQ(images.Count(), size_t(1));
    CHECK(images.Bytes() <= 1024 * 1024 * 4 + 4096);  // level 2: 1024^2, not 4096^2 x 4 bytes
    CHECK((page.pixels[600 * 1200 + 600] & 0xFF) == 0x80);
    // Again: from the cache, nothing decoded.
    // Again, a new build of the same page: the image from the cache, its samples from the
    // first render, the same pixels.
    RenderStats again;
    const Image second = Render(PagePdf("q 50 0 0 50 25 25 cm /I Do Q", "<< /XObject << /I 5 0 R >> >>", 100, 100,
                                        {GrayImage(4096, 0x80)}),
                                1200, again, ms, environment);
    CHECK_EQ(again.imageBytes, uint64_t(0));
    CHECK_EQ(again.imagesReused, uint32_t(1));
    CHECK(second.pixels == page.pixels);
    std::printf("again: %.1f ms\n", ms);

    // Nine distinct images of 64 MiB decode cost: eight fit the page's 512 MiB.
    std::string content, resources = "<< /XObject <<";
    std::vector<std::string> objects;
    for (int i = 0; i < 9; i++) {
        content += "q 10 0 0 10 " + std::to_string(i * 10) + " 0 cm /I" + std::to_string(i) + " Do Q\n";
        resources += " /I" + std::to_string(i) + " " + std::to_string(5 + i) + " 0 R";
        objects.push_back(GrayImage(4096, 10 + i));
    }
    resources += " >> >>";
    RenderStats many;
    Render(PagePdf(content, resources, 100, 100, objects), 1200, many, ms, environment);
    Print("nine 4096^2 images", many, ms);
    CHECK(Has(many, "budget"));
    CHECK(many.imageBytes <= RenderLimits::kImageBytes);
    CHECK_EQ(many.images, uint32_t(8));
    CHECK(ms < 10000 * kSlow);

    // DecodeImage itself: over its budget it spends nothing.
    const auto file = PdfFile::Parse(PagePdf("", "<< >>", 100, 100, {GrayImage(64, 1)}), nullptr);
    CHECK(file != nullptr);
    if (file) {
        Image out;
        std::string reason;
        uint64_t budget = 1000;
        CHECK(!rgui::pdf::DecodeImage(*file, file->Object(5), out, &reason, 0, &budget));
        CHECK_EQ(reason, std::string("budget"));
        CHECK_EQ(budget, uint64_t(1000));
        budget = 4 * 64 * 64;
        CHECK(rgui::pdf::DecodeImage(*file, file->Object(5), out, &reason, 3, &budget));
        CHECK_EQ(budget, uint64_t(0));
        CHECK(out.width == 8 && out.height == 8 && (out.pixels[0] & 0xFF) == 1);
    }

    // A JPEG soft mask whose header claims 8192 x 8192 fails on its size, before decoding.
    std::string gray;
    CHECK(RayoMd::File::ReadAll(std::string(rgui::test::SourceDir()) + "/tests/data/jpeg/gray.jpg", 1u << 20, gray));
    for (size_t i = 2; i + 9 < gray.size(); i++) {
        if (static_cast<uint8_t>(gray[i]) == 0xFF && static_cast<uint8_t>(gray[i + 1]) == 0xC0) {
            gray[i + 5] = gray[i + 7] = '\x20';  // 0x2000 = 8192
            gray[i + 6] = gray[i + 8] = '\0';
            break;
        }
    }
    RenderStats masked;
    Render(PagePdf("q 10 0 0 10 0 0 cm /I Do Q", "<< /XObject << /I 5 0 R >> >>", 100, 100,
                   {Stream("\x80", " /Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceGray "
                                   "/BitsPerComponent 8 /SMask 6 0 R"),
                    Stream(gray, " /Type /XObject /Subtype /Image /Width 8192 /Height 8192 /ColorSpace /DeviceGray "
                                 "/BitsPerComponent 8 /Filter /DCTDecode")}),
           100, masked, ms, environment);
    CHECK(Has(masked, "image:size"));
}

// A JPEG drawn small is decoded at 1/2, 1/4 or 1/8 of its size: the mean of each square.
RGUI_TEST("jpeg.scaled") {
    std::string jpeg;
    CHECK(RayoMd::File::ReadAll(std::string(rgui::test::SourceDir()) + "/tests/corpus/photo.jpg", 1u << 22, jpeg));
    const uint8_t* data = reinterpret_cast<const uint8_t*>(jpeg.data());
    Image full;
    std::string reason;
    CHECK(rgui::pdf::DecodeJpeg(data, jpeg.size(), full, &reason));
    for (int shift = 1; shift <= 3; shift++) {
        Image scaled;
        CHECK(rgui::pdf::DecodeJpeg(data, jpeg.size(), scaled, &reason, shift));
        const Image reduced = rgui::pdf::ReduceImage(full, shift);
        CHECK_EQ(scaled.width, (full.width + (1 << shift) - 1) >> shift);
        CHECK_EQ(scaled.height, reduced.height);
        double sum = 0;
        for (size_t i = 0; i < scaled.pixels.size(); i++) {
            for (int c = 0; c < 24; c += 8) {
                sum += std::abs(int((scaled.pixels[i] >> c) & 255) - int((reduced.pixels[i] >> c) & 255));
            }
        }
        const double mad = sum / (3.0 * scaled.pixels.size());
        std::printf("photo.jpg at 1/%d: MAD %.3f against the reduced full decode\n", 1 << shift, mad);
        CHECK(mad < 2.0);
    }
}

// A progressive JPEG of 1000 scans that each cost every block of the image: refused once the
// scans have visited every coefficient 32 times.
RGUI_TEST("jpeg.too_much_work") {
    const std::vector<uint8_t> bomb = ProgressiveBomb(2048, 999);
    std::printf("bomb: %zu bytes\n", bomb.size());
    Image image;
    std::string reason;
    const auto start = Clock::now();
    CHECK(!rgui::pdf::DecodeJpeg(bomb.data(), bomb.size(), image, &reason));
    const double ms = MsSince(start);
    std::printf("bomb: %s in %.1f ms\n", reason.c_str(), ms);
    CHECK_EQ(reason, std::string("jpeg-too-much-work"));
    CHECK(ms < 1000 * kSlow);
    // Thirty refinements are within the bound and decode.
    const std::vector<uint8_t> fine = ProgressiveBomb(256, 30);
    CHECK(rgui::pdf::DecodeJpeg(fine.data(), fine.size(), image, &reason));
}

// ---- cancellation ------------------------------------------------------------------------

// A render whose cancel flag is set stops within milliseconds and returns false.
RGUI_TEST("pdf.render_cancel") {
    const auto file = PdfFile::Parse(HeavyPage(), nullptr);
    CHECK(file != nullptr);
    if (!file) return;
    rgui::pdf::RenderEnvironment environment = TestEnvironment();
    std::atomic<bool> cancel{false};
    environment.cancel = &cancel;
    RenderStats stats;
    Image image;
    bool ok = true;
    Clock::time_point cancelled, finished;
    std::thread render([&] {
        ok = RenderPage(*file, 0, 4000, image, &stats, environment);
        finished = Clock::now();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancelled = Clock::now();
    cancel = true;
    render.join();
    const double after = std::chrono::duration<double, std::milli>(finished - cancelled).count();
    std::printf("render cancelled after %u operators, returned %.2f ms after the flag\n", stats.operators, after);
    CHECK(!ok);
    CHECK(stats.cancelled);
    CHECK(image.pixels.empty());
    CHECK(after < 100 * kSlow);
    // Uncancelled, the same page completes.
    cancel = false;
    RenderStats whole;
    const auto start = Clock::now();
    CHECK(RenderPage(*file, 0, 4000, image, &whole, environment));
    std::printf("heavy page: %.1f ms\n", MsSince(start));
    CHECK(!whole.cancelled && whole.unknown.empty());
}

// The preview abandons a render of an older build nobody wants, and quitting does not wait for
// one.
RGUI_TEST("pdf.preview_cancel") {
    rgui::test::HeadlessPlatform platform;
    auto heavy = PdfFile::Parse(HeavyPage(), nullptr);
    auto light = PdfFile::Parse(PagePdf("0 0 1 rg 10 10 20 20 re f"), nullptr);
    CHECK(heavy && light);
    if (!heavy || !light) return;
    {
        rgui::PreviewPages pages(platform, &rgui::pdftest::TestFontIndex(), [] {}, 1);
        pages.SetDocument(heavy, 1);
        pages.Request({0}, 4000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the heavy page is being drawn
        const auto start = Clock::now();
        pages.SetDocument(light, 2);
        pages.Request({0}, 4000);
        CHECK(platform.PumpUntil([&] { return pages.Current(0, 4000); }, 20000));
        const double ms = MsSince(start);
        std::printf("the new build's page shown %.1f ms after it arrived\n", ms);
        CHECK_EQ(pages.CancelledCount(), uint64_t(1));
        CHECK(ms < 1000 * kSlow);
    }
    const auto start = Clock::now();
    {
        rgui::PreviewPages pages(platform, &rgui::pdftest::TestFontIndex(), [] {}, 1);
        pages.SetDocument(heavy, 1);
        pages.Request({0}, 4000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const double quit = MsSince(start) - 100;
    std::printf("destroyed %.1f ms after the request's 100 ms\n", quit);
    CHECK(quit < 100 * kSlow);
    platform.Pump();
}
