// SPDX-License-Identifier: Apache-2.0
// The preview renderer on hand-written content streams and on the engine's PDFs.
#include "runner.h"

#include "pdf_testing.h"

#include "common/file_io.h"
#include "core/flate.h"
#include "pdf/fonts.h"
#include "pdf/images.h"
#include "pdf/jpeg.h"
#include "pdf/reader.h"
#include "pdf/renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using rgui::gfx::Image;
using rgui::pdf::PdfFile;
using rgui::pdf::RenderPage;
using rgui::pdf::RenderStats;
using rgui::pdftest::MakePdf;
using rgui::pdftest::PagePdf;
using rgui::pdftest::Stream;
using rgui::pdftest::TestEnvironment;
using rgui::pdf::PdfValue;

struct Rgb {
    int r, g, b;
};

Rgb At(const Image& image, int x, int y) {
    const uint32_t p = image.pixels[static_cast<size_t>(y) * image.width + x];
    return {static_cast<int>((p >> 16) & 255), static_cast<int>((p >> 8) & 255), static_cast<int>(p & 255)};
}

// Within `tolerance` per channel; prints both colours when not.
bool Near(Rgb a, Rgb b, int tolerance = 2) {
    const bool near =
        std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance;
    if (!near) std::printf("  colour %d %d %d, expected %d %d %d\n", a.r, a.g, a.b, b.r, b.g, b.b);
    return near;
}

bool Has(const RenderStats& stats, const std::string& what) {
    return std::find(stats.unknown.begin(), stats.unknown.end(), what) != stats.unknown.end();
}

// Renders page 0 of `pdf` at `width` px with the test environment.
Image Render(const std::string& pdf, int width, RenderStats* stats = nullptr) {
    Image image;
    const auto file = PdfFile::Parse(pdf, nullptr);
    CHECK(file != nullptr);
    if (file) CHECK(RenderPage(*file, 0, width, image, stats, TestEnvironment()));
    return image;
}

// Pixels darker than mid-grey.
size_t DarkPixels(const Image& image) {
    size_t n = 0;
    for (uint32_t p : image.pixels) n += ((p >> 8) & 255) < 128;
    return n;
}

std::string Compress(const std::string& data) {
    std::vector<uint8_t> out(RayoMd::Flate::ZlibBound(data.size()));
    RayoMd::Flate::Scratch scratch;
    const size_t n = RayoMd::Flate::ZlibCompress(data, out.data(), scratch);
    return std::string(reinterpret_cast<const char*>(out.data()), n);
}

const char kHelvetica[] = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                          "/Encoding /WinAnsiEncoding >> >> >>";

// True (with a skip line) when the test environment has no face for Helvetica.
bool NoHelvetica(const char* test) {
    RenderStats stats;
    Render(PagePdf("BT /F1 10 Tf (H) Tj ET", kHelvetica), 20, &stats);
    if (Has(stats, "font:no-substitute:Helvetica")) {
        std::printf("skip %s: no face for Helvetica (Liberation Sans, Arimo, ...)\n", test);
        return true;
    }
    return false;
}

} // namespace

RGUI_TEST("pdf.render_page_size") {
    const auto file = PdfFile::Parse(PagePdf("", "<< >>", 200, 100), nullptr);
    CHECK(file != nullptr);
    if (!file) return;
    Image image;
    image.id = 7;
    CHECK(RenderPage(*file, 0, 300, image, nullptr, TestEnvironment()));
    CHECK_EQ(image.width, 300);
    CHECK_EQ(image.height, 150);
    CHECK(image.opaque);
    CHECK_EQ(image.id, uint64_t(7));
    CHECK(std::all_of(image.pixels.begin(), image.pixels.end(), [](uint32_t p) { return p == 0xFFFFFFFFu; }));
    // Pages that do not exist and sizes out of range leave the image alone.
    CHECK(!RenderPage(*file, 1, 300, image, nullptr, TestEnvironment()));
    CHECK(!RenderPage(*file, -1, 300, image, nullptr, TestEnvironment()));
    CHECK(!RenderPage(*file, 0, 0, image, nullptr, TestEnvironment()));
    CHECK(!RenderPage(*file, 0, rgui::pdf::kMaxRenderSide + 1, image, nullptr, TestEnvironment()));
    const auto tall = PdfFile::Parse(PagePdf("", "<< >>", 1, 1000), nullptr);
    CHECK(tall && !RenderPage(*tall, 0, 1000, image, nullptr, TestEnvironment()));
    CHECK_EQ(image.width, 300);
}

RGUI_TEST("pdf.render_fill_clip") {
    // y up in the PDF: (0,0)-(50,50) is the bottom-left quarter of the image.
    const Image image = Render(PagePdf("1 0 0 rg 0 0 100 100 re f\n"
                                       "q 0 0 50 50 re W n 0 0 1 rg 0 0 100 100 re f Q\n"
                                       "0 1 0 rg 60 60 10 10 re f\n"
                                       "q 0 50 50 50 re 10 60 30 30 re W* n 0 0 0 rg 0 0 100 100 re f Q\n"),
                               100);
    CHECK(Near(At(image, 25, 75), {0, 0, 255}));    // inside the clip
    CHECK(Near(At(image, 75, 75), {255, 0, 0}));    // outside it
    CHECK(Near(At(image, 65, 35), {0, 255, 0}));    // after Q: no clip
    CHECK(Near(At(image, 5, 5), {0, 0, 0}));        // even-odd ring
    CHECK(Near(At(image, 25, 25), {255, 0, 0}));    // its hole
    // A clip off the pixel grid is antialiased; one on it is sharp.
    const Image soft = Render(PagePdf("q 10.5 10 20 20 re W n 0 0 0 rg 0 0 100 100 re f Q"), 100);
    CHECK(Near(At(soft, 10, 80), {128, 128, 128}, 3));
    CHECK(Near(At(soft, 11, 80), {0, 0, 0}));
    CHECK(Near(At(soft, 9, 80), {255, 255, 255}));
    CHECK(Near(At(soft, 30, 80), {128, 128, 128}, 3));
}

RGUI_TEST("pdf.render_strokes") {
    // A 4-unit line along y = 10 dashed 10 on, 10 off.
    const Image dashed = Render(PagePdf("0 0 0 RG 4 w [10 10] 0 d 0 10 m 100 10 l S", "<< >>", 100, 20), 100);
    CHECK(Near(At(dashed, 5, 10), {0, 0, 0}));
    CHECK(Near(At(dashed, 15, 10), {255, 255, 255}));
    CHECK(Near(At(dashed, 25, 10), {0, 0, 0}));
    CHECK(Near(At(dashed, 5, 5), {255, 255, 255}));  // beyond half the width
    // The phase shifts the pattern; an empty array is solid.
    const Image phased = Render(PagePdf("0 0 0 RG 4 w [10 10] 5 d 0 10 m 100 10 l S", "<< >>", 100, 20), 100);
    CHECK(Near(At(phased, 2, 10), {0, 0, 0}));
    CHECK(Near(At(phased, 7, 10), {255, 255, 255}));
    const Image solid = Render(PagePdf("0 0 0 RG 4 w [10 10] 0 d [] 0 d 0 10 m 100 10 l S", "<< >>", 100, 20), 100);
    CHECK(Near(At(solid, 15, 10), {0, 0, 0}));
    // ExtGState line width, cap and dash.
    const Image gs = Render(PagePdf("/G1 gs 0 0 1 RG 20 10 m 80 10 l S",
                                    "<< /ExtGState << /G1 << /LW 6 /LC 2 /D [[] 0] >> >> >>", 100, 20),
                            100);
    CHECK(Near(At(gs, 18, 10), {0, 0, 255}));   // the square cap reaches 3 units past the end
    CHECK(Near(At(gs, 50, 8), {0, 0, 255}));
    CHECK(Near(At(gs, 15, 10), {255, 255, 255}));
}

RGUI_TEST("pdf.render_colors_alpha") {
    // Regions that do not overlap: fill x < 50 above y 30, a stroke at x 75 above y 30, a band
    // along the top, CMYK squares at the bottom, a gray square and a green line at x 55..90.
    const Image image = Render(PagePdf("/GS1 gs 0 0 0 rg 0 30 50 70 re f 0 0 0 RG 10 w 75 30 m 75 100 l S\n"
                                       "q /GS2 gs Q 0 0 0 rg 0 90 100 10 re f\n"
                                       "1 0 0 0 k 0 0 20 20 re f 0 0 1 0 k 30 0 20 20 re f\n"
                                       "/DeviceGray cs 0.5 sc 55 40 10 10 re f /DeviceRGB CS 0 1 0 SC 4 w 60 40 m 90 40 l S\n",
                                       "<< /ExtGState << /GS1 << /ca 0.5 /CA 0.25 >> /GS2 << /ca 1 >> >> >>"),
                               100);
    CHECK(Near(At(image, 25, 50), {128, 128, 128}));   // fill at ca 0.5
    CHECK(Near(At(image, 75, 50), {191, 191, 191}));   // stroke at CA 0.25
    CHECK(Near(At(image, 60, 5), {128, 128, 128}));    // q gs Q: the alpha stays 0.5
    CHECK(Near(At(image, 5, 95), {128, 255, 255}));    // cyan at 0.5 over white
    CHECK(Near(At(image, 35, 95), {255, 255, 128}));   // yellow at 0.5
    CHECK(Near(At(image, 57, 55), {191, 191, 191}));   // 50 % gray at 0.5
    CHECK(Near(At(image, 85, 60), {191, 255, 191}));   // green stroke at 0.25
}

RGUI_TEST("pdf.render_shading") {
    const std::string fn =
        "/Function << /FunctionType 3 /Domain [0 1] /Bounds [0.5] /Encode [0 1 0 1] /Functions ["
        "<< /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 1 0] /N 1 >> "
        "<< /FunctionType 2 /Domain [0 1] /C0 [0 1 0] /C1 [0 0 1] /N 1 >>] >>";
    const std::string resources = "<< /Shading << /S1 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 100 0] "
                                  "/Extend [true true] " + fn + " >> /S2 << /ShadingType 2 /ColorSpace /DeviceRGB "
                                  "/Coords [20 0 80 0] " + fn + " >> /S3 << /ShadingType 2 /ColorSpace /DeviceRGB "
                                  "/Coords [20 0 80 0] /Extend [true true] /Function << /FunctionType 2 /C0 [1 1 1] "
                                  "/C1 [0 0 0] /N 2 >> >> >> >>";
    RenderStats stats;
    const Image image = Render(PagePdf("q 0 20 100 10 re W n /S1 sh Q q 0 10 100 10 re W n /S2 sh Q "
                                       "q 0 0 100 10 re W n /S3 sh Q",
                                       resources, 100, 30),
                               100, &stats);
    CHECK(Near(At(image, 0, 5), {253, 3, 0}, 3));
    CHECK(Near(At(image, 50, 5), {0, 255, 0}, 3));
    CHECK(Near(At(image, 99, 5), {0, 3, 253}, 3));
    CHECK(Near(At(image, 25, 5), {128, 128, 0}, 4));
    // Without /Extend nothing is painted past the ends; with it, the end colours are.
    CHECK(Near(At(image, 10, 15), {255, 255, 255}));
    CHECK(Near(At(image, 90, 15), {255, 255, 255}));
    CHECK(Near(At(image, 50, 15), {0, 255, 0}, 6));
    CHECK(Near(At(image, 10, 25), {255, 255, 255}));
    CHECK(Near(At(image, 90, 25), {0, 0, 0}));
    // N 2 is drawn as N 1 and reported.
    CHECK(Near(At(image, 50, 25), {128, 128, 128}, 4));
    CHECK(Has(stats, "shading-exponent"));
    CHECK_EQ(stats.unknown.size(), size_t(1));
}

RGUI_TEST("pdf.render_images") {
    // 2x2 RGB, no filter: row 0 (red, green) is the top of the unit square.
    const std::string raw("\xFF\x00\x00\x00\xFF\x00\x00\x00\xFF\xFF\xFF\xFF", 12);
    // 4x4 gray, PNG-predicted (Sub and Up rows), with a soft mask that hides the left half.
    std::string rows, mask;
    for (int y = 0; y < 4; y++) {
        rows += y % 2 ? '\x02' : '\x01';
        for (int x = 0; x < 4; x++) rows += y % 2 ? '\x00' : (x == 0 ? '\x40' : '\x00');  // every pixel 64
        for (int x = 0; x < 4; x++) mask += x < 2 ? '\x00' : '\xFF';
    }
    // 8x1 gray at 1 bit: 10110000.
    const std::string bits("\xB0", 1);
    const std::string resources = "<< /XObject << /A 5 0 R /B 6 0 R /C 8 0 R >> >>";
    const std::vector<std::string> extra = {
        Stream(raw, " /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8"),
        Stream(Compress(rows), " /Type /XObject /Subtype /Image /Width 4 /Height 4 /ColorSpace /DeviceGray "
                               "/BitsPerComponent 8 /Filter /FlateDecode /DecodeParms << /Predictor 15 /Columns 4 >> "
                               "/SMask 7 0 R"),
        Stream(Compress(mask), " /Type /XObject /Subtype /Image /Width 4 /Height 4 /ColorSpace /DeviceGray "
                               "/BitsPerComponent 8 /Filter /FlateDecode"),
        Stream(Compress(bits), " /Type /XObject /Subtype /Image /Width 8 /Height 1 /ColorSpace /DeviceGray "
                               "/BitsPerComponent 1 /Filter [/FlateDecode]")};
    RenderStats stats;
    const Image image = Render(PagePdf("q 100 0 0 100 0 0 cm /A Do Q q 40 0 0 40 0 0 cm /B Do Q "
                                       "q 80 0 0 10 0 90 cm /C Do Q",
                                       resources, 100, 100, extra),
                               100, &stats);
    CHECK(stats.unknown.empty());
    CHECK_EQ(stats.images, uint32_t(3));
    CHECK(Near(At(image, 25, 25), {255, 0, 0}, 5));  // bilinear: 2/256 of the neighbours
    CHECK(Near(At(image, 75, 25), {0, 255, 0}));
    CHECK(Near(At(image, 75, 75), {255, 255, 255}));
    // The gray image: hidden on the left, 64 on the right (over the blue of A's bottom left).
    CHECK(Near(At(image, 5, 80), {0, 0, 255}));
    CHECK(Near(At(image, 35, 80), {64, 64, 64}));
    // The bit image: 1 = white, 0 = black, 10 units a pixel.
    CHECK(Near(At(image, 5, 5), {255, 255, 255}, 20));
    CHECK(Near(At(image, 15, 5), {0, 0, 0}, 20));
    CHECK(Near(At(image, 25, 5), {255, 255, 255}, 20));
    CHECK(Near(At(image, 45, 5), {0, 0, 0}, 20));

    // DCTDecode: the corpus photo against the JPEG decoder.
    std::string jpeg;
    CHECK(RayoMd::File::ReadAll(std::string(rgui::test::SourceDir()) + "/tests/corpus/photo.jpg", 1u << 22, jpeg));
    Image decoded;
    std::string reason;
    CHECK(rgui::pdf::DecodeJpeg(reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size(), decoded, &reason));
    const std::string photoPdf =
        PagePdf("q 64 0 0 64 0 0 cm /P Do Q", "<< /XObject << /P 5 0 R >> >>", 64, 64,
                {Stream(jpeg, " /Type /XObject /Subtype /Image /Width " + std::to_string(decoded.width) + " /Height " +
                                  std::to_string(decoded.height) +
                                  " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode")});
    const auto file = PdfFile::Parse(photoPdf, nullptr);
    CHECK(file != nullptr);
    if (!file) return;
    rgui::pdf::ImageCache cache;
    const PdfValue& entry = *file->Get(file->PageResources(0), "XObject").Find("P");
    const auto first = cache.Load(*file, entry, &reason);
    CHECK(first != nullptr);
    if (!first) return;
    CHECK(first->pixels == decoded.pixels);
    CHECK(cache.Load(*file, entry, &reason) == first);   // decoded once
    CHECK_EQ(cache.Count(), size_t(1));
    CHECK(cache.Bytes() >= first->pixels.size() * 4);
    // Rendered at the image's own size, each device pixel is one image pixel.
    const Image photo = Render(photoPdf, decoded.width);
    size_t equal = 0;
    for (size_t i = 0; i < photo.pixels.size(); i++) equal += photo.pixels[i] == decoded.pixels[i];
    CHECK(equal * 100 >= photo.pixels.size() * 99);
}

RGUI_TEST("pdf.render_text") {
    if (NoHelvetica("pdf.render_text")) return;
    auto text = [](const std::string& ops) {
        RenderStats stats;
        Image image = Render(PagePdf("BT /F1 40 Tf 10 30 Td " + ops + " ET", kHelvetica, 100, 100), 100, &stats);
        CHECK(stats.unknown.empty());
        return std::make_pair(image, stats);
    };
    const auto fill = text("(H) Tj");
    const size_t filled = DarkPixels(fill.first);
    CHECK(filled > 150);
    CHECK_EQ(fill.second.glyphs, uint32_t(1));
    CHECK_EQ(fill.second.glyphOutlines, uint32_t(0));  // from the glyph cache
    // Modes 4-7 draw as 0-3 (no clipping).
    CHECK(text("4 Tr (H) Tj").first.pixels == fill.first.pixels);
    CHECK_EQ(DarkPixels(text("3 Tr (H) Tj").first), size_t(0));
    CHECK_EQ(DarkPixels(text("7 Tr (H) Tj").first), size_t(0));
    const auto stroke = text("1 Tr 0 0 1 RG (H) Tj");
    CHECK_EQ(stroke.second.glyphOutlines, uint32_t(1));
    size_t blue = 0;
    for (uint32_t p : stroke.first.pixels) blue += (p & 255) > 200 && ((p >> 16) & 255) < 100;
    CHECK(blue > 50 && blue < filled);
    // A sheared or rotated text matrix draws the outline: the same area, within the edges.
    const auto rotated = text("0 1 -1 0 60 30 Tm (H) Tj");
    CHECK_EQ(rotated.second.glyphOutlines, uint32_t(1));
    const size_t turned = DarkPixels(rotated.first);
    CHECK(turned * 100 > filled * 85 && turned * 100 < filled * 115);
    // Advances: Helvetica's H is 722/1000 em, plus Tc, plus Tw after a space only, times Tz.
    CHECK(text("(HH) Tj").first.pixels == text("(H) Tj 28.88 0 Td (H) Tj").first.pixels);
    CHECK(text("5 Tc (HH) Tj").first.pixels == text("(H) Tj 33.88 0 Td (H) Tj").first.pixels);
    CHECK(text("6 Tw (H H) Tj").first.pixels == text("(H) Tj 46.0 0 Td (H) Tj").first.pixels);  // 28.88 + 11.12 + 6
    CHECK(text("[(H) -500 (H)] TJ").first.pixels == text("(H) Tj 48.88 0 Td (H) Tj").first.pixels);
    CHECK(text("50 Tz (HH) Tj").first.pixels != fill.first.pixels);
}

RGUI_TEST("pdf.render_font_widths") {
    // A Type0 font over DejaVu Sans with a CIDToGIDMap stream, /W in both forms and /DW.
    std::string dejavu;
    if (!RayoMd::File::ReadAll("/usr/share/fonts/TTF/DejaVuSans.ttf", 1u << 24, dejavu)) {
        std::printf("skip pdf.render_font_widths: DejaVu Sans not found\n");
        return;
    }
    std::string map(8, '\0');
    map[3] = 36;  // CID 1 -> glyph 36 ('A' in DejaVu Sans)
    const std::string pdf = PagePdf(
        "BT /F1 50 Tf 10 30 Td <0001> Tj ET", "<< /Font << /F1 5 0 R >> >>", 100, 100,
        {"<< /Type /Font /Subtype /Type0 /BaseFont /X /Encoding /Identity-H /DescendantFonts [6 0 R] >>",
         "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /X /FontDescriptor 7 0 R /CIDToGIDMap 9 0 R "
         "/DW 300 /W [1 [500 600] 10 20 700] >>",
         "<< /Type /FontDescriptor /FontName /X /FontFile2 8 0 R >>", Stream(dejavu), Stream(map)});
    const auto file = PdfFile::Parse(pdf, nullptr);
    CHECK(file != nullptr);
    if (!file) return;
    const auto font = rgui::pdf::LoadFont(*file, file->Object(5), nullptr);
    CHECK(font->Unsupported().empty());
    CHECK(font->Source() != nullptr);
    RenderStats stats;
    CHECK(DarkPixels(Render(pdf, 100, &stats)) > 300);  // the A
    CHECK(stats.unknown.empty());
    const std::string codes("\x00\x01\x00\x02\x00\x03\x00\x0F\x00\x15\x00", 11);
    std::vector<rgui::pdf::PdfGlyph> glyphs;
    size_t at = 0;
    rgui::pdf::PdfGlyph g;
    while (font->Next(codes, at, g)) glyphs.push_back(g);
    CHECK_EQ(glyphs.size(), size_t(5));  // the odd last byte is dropped
    if (glyphs.size() != 5) return;
    CHECK_EQ(glyphs[0].glyph, uint16_t(36));
    CHECK_EQ(glyphs[1].glyph, uint16_t(0));
    CHECK_EQ(glyphs[0].width, 500.0);
    CHECK_EQ(glyphs[1].width, 600.0);
    CHECK_EQ(glyphs[2].width, 300.0);
    CHECK_EQ(glyphs[3].width, 700.0);
    CHECK_EQ(glyphs[4].width, 300.0);
    CHECK(!glyphs[0].wordSpace);
    // Fonts the renderer does not read say what they are.
    const std::string others = PagePdf("", "<< >>", 100, 100,
                                       {"<< /Type /Font /Subtype /Type3 >>",
                                        "<< /Type /Font /Subtype /Type1 /BaseFont /Symbol >>",
                                        "<< /Type /Font /Subtype /Type0 /Encoding /UniJIS-UCS2-H /DescendantFonts [] >>",
                                        "<< /Type /Font /Subtype /TrueType /BaseFont /Arial >>"});
    const auto other = PdfFile::Parse(others, nullptr);
    CHECK(other != nullptr);
    if (!other) return;
    CHECK(rgui::pdf::LoadFont(*other, other->Object(5), nullptr)->Unsupported() == "font:Type3");
    CHECK(rgui::pdf::LoadFont(*other, other->Object(6), nullptr)->Unsupported() == "font:standard:Symbol");
    CHECK(rgui::pdf::LoadFont(*other, other->Object(7), nullptr)->Unsupported() == "font:Type0-encoding");
    CHECK(rgui::pdf::LoadFont(*other, other->Object(8), nullptr)->Unsupported() == "font:TrueType");
}

RGUI_TEST("pdf.render_forms") {
    // Fm1 fills beyond its BBox (only the box shows) under a /Matrix; Fm2 restores more than it
    // saved; Fm3 draws itself without end.
    const std::string resources = "<< /XObject << /Fm1 5 0 R /Fm2 6 0 R /Fm3 7 0 R >> >>";
    const std::vector<std::string> extra = {
        Stream("0 0 1 rg 0 0 100 100 re f", " /Type /XObject /Subtype /Form /BBox [0 0 10 10] /Matrix [2 0 0 2 0 0]"),
        Stream("Q Q Q 0 1 0 rg", " /Type /XObject /Subtype /Form /BBox [0 0 100 100]"),
        Stream("0 0 0 rg 0 0 1 1 re f /Fm3 Do",
               " /Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources << /XObject << /Fm3 7 0 R >> >>")};
    RenderStats stats;
    const auto start = std::chrono::steady_clock::now();
    const Image image = Render(PagePdf("q 1 0 0 rg /Fm1 Do /Fm2 Do 50 50 10 10 re f Q 70 70 10 10 re f /Fm3 Do",
                                       resources, 100, 100, extra),
                               100, &stats);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(Near(At(image, 10, 90), {0, 0, 255}));       // in the BBox, scaled by 2
    CHECK(Near(At(image, 30, 70), {255, 255, 255}));   // outside it
    CHECK(Near(At(image, 55, 45), {255, 0, 0}));       // Fm2 could not undo the q nor set the colour
    CHECK(Near(At(image, 75, 25), {0, 0, 0}));         // after Q: black
    CHECK(Has(stats, "form-depth"));
    CHECK_EQ(stats.unknown.size(), size_t(1));
}

RGUI_TEST("pdf.render_malformed") {
    RenderStats stats;
    std::string content = "Q Q 1 0 0 rg 0 0 re f (abc) 0 0 1 1 re f /N cm [1 2] w 0 0 m 50 l S h h n W* ";
    for (int i = 0; i < 80; i++) content += "q 2 0 0 2 0 0 cm ";
    content += "0 1 0 rg Q ";
    for (int i = 0; i < 79; i++) content += "Q ";
    content += "BT /NoFont 12 Tf (x) Tj (y) ' 1 2 (z) \" [(a) 5 /n] TJ ET 12 xyz 10 10 20 20 re f "
               "0 0 1 rg 1 2 3 sc /Pattern cs /P1 scn 70 70 20 20 re f d0 d0 EMC BX EX";
    const Image image = Render(PagePdf(content), 100, &stats);
    // Everything before the valid fill came to nothing and the q/Q nesting (past the 64 kept
    // saves) balanced out: the square is at its place and size, in the red set before the nest.
    CHECK(Near(At(image, 20, 80), {255, 0, 0}));
    CHECK(Near(At(image, 5, 95), {255, 255, 255}));
    CHECK(Near(At(image, 35, 75), {255, 255, 255}));
    CHECK(Near(At(image, 80, 20), {255, 255, 255}));  // a pattern colour paints nothing
    CHECK(Has(stats, "font:missing"));
    CHECK(Has(stats, "op:xyz"));
    CHECK(Has(stats, "op:d0"));
    CHECK(Has(stats, "pattern"));
    CHECK(Has(stats, "colorspace:Pattern") || Has(stats, "colorspace:?"));
    CHECK_EQ(std::count(stats.unknown.begin(), stats.unknown.end(), std::string("op:d0")), 1);
    // Unreadable content is a white page with the reason.
    RenderStats filtered;
    const Image blank = Render(MakePdf({"<< /Type /Catalog /Pages 2 0 R >>",
                                        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                                        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 10 10] /Contents 4 0 R >>",
                                        Stream("x", " /Filter /LZWDecode")}),
                               10, &filtered);
    CHECK(Has(filtered, "filter:LZWDecode"));
    CHECK(std::all_of(blank.pixels.begin(), blank.pixels.end(), [](uint32_t p) { return p == 0xFFFFFFFFu; }));
    // Images that do not decode are left out and named.
    RenderStats images;
    Render(PagePdf("/A Do /B Do /C Do /A Do", "<< /XObject << /A 5 0 R /B 6 0 R /C 7 0 R >> >>", 100, 100,
                   {Stream("x", " /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8 "
                                "/Filter /LZWDecode"),
                    Stream("xyz", " /Subtype /Image /Width 9 /Height 9 /ColorSpace /DeviceRGB /BitsPerComponent 8 "
                                  "/Filter /FlateDecode"),
                    Stream("xyz", " /Subtype /Image /Width 1 /Height 1 /ColorSpace /Indexed /BitsPerComponent 8")}),
           100, &images);
    CHECK(Has(images, "image:filter:LZWDecode"));
    CHECK(Has(images, "image:flate"));
    CHECK(Has(images, "image:colorspace:Indexed"));
    CHECK_EQ(images.unknown.size(), size_t(3));
    CHECK_EQ(images.images, uint32_t(0));
}

namespace {

std::string TesterPdf(TinyPdf::PdfOptions options = {}) {
    const std::string path = std::string(rgui::test::SourceDir()) + "/external/rayomd/tester.md";
    std::string markdown;
    CHECK(RayoMd::File::ReadAll(path, 1u << 24, markdown));
    options.sourcePath = path;
    const std::string pdf = rgui::pdftest::BuildPreview(markdown, options);
    CHECK(!pdf.empty());
    return pdf;
}

} // namespace

RGUI_TEST("pdf.render_tester") {
    if (NoHelvetica("pdf.render_tester")) return;
    const std::string pdf = TesterPdf();
    const auto file = PdfFile::Parse(pdf, nullptr);
    CHECK(file != nullptr);
    if (!file) return;
    std::vector<Image> pages(static_cast<size_t>(file->PageCount()));
    for (int page = 0; page < file->PageCount(); page++) {
        RenderStats stats;
        CHECK(RenderPage(*file, page, 300, pages[static_cast<size_t>(page)], &stats, TestEnvironment()));
        CHECK(stats.unknown.empty());
        for (const std::string& what : stats.unknown) std::printf("page %d: %s\n", page + 1, what.c_str());
        // Every page has text, and ink: pixels that are not white.
        const Image& image = pages[static_cast<size_t>(page)];
        const size_t ink = static_cast<size_t>(
            std::count_if(image.pixels.begin(), image.pixels.end(), [](uint32_t p) { return p != 0xFFFFFFFFu; }));
        CHECK(stats.glyphs > 50);
        CHECK(ink > 1000);
    }
    // Three threads at once, each rendering every page: the same pixels.
    std::vector<int> same(3, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; t++) {
        threads.emplace_back([&, t] {
            for (int page = file->PageCount() - 1; page >= 0; page--) {
                Image image;
                RenderPage(*file, page, 300, image, nullptr, TestEnvironment());
                same[static_cast<size_t>(t)] += image.pixels == pages[static_cast<size_t>(page)].pixels;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    for (int n : same) CHECK_EQ(n, file->PageCount());
}

namespace {

struct Random {
    uint64_t state;
    uint64_t Next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    size_t Below(size_t n) { return n ? static_cast<size_t>(Next() % n) : 0; }
};

} // namespace

RGUI_TEST("pdf.render_hardening") {
    const std::string pdf = TesterPdf();
    const auto original = PdfFile::Parse(pdf, nullptr);
    CHECK(original != nullptr);
    if (!original) return;
    // The byte ranges of the page contents, to render the page a mutation hits.
    std::vector<std::pair<size_t, size_t>> contents;
    for (int page = 0; page < original->PageCount(); page++) {
        std::string_view content;
        CHECK(original->PageContent(page, content, nullptr));
        const size_t begin = static_cast<size_t>(content.data() - original->Bytes().data());
        contents.push_back({begin, begin + content.size()});
    }
    // Positions in content streams, in font programs and other streams, and anywhere.
    std::vector<size_t> streams;
    for (size_t at = pdf.find("stream\n"); at != std::string::npos; at = pdf.find("stream\n", at + 1)) {
        streams.push_back(at + 7);
    }
    static const char kBytes[] = "()<>[]{}/%\\ \n\r0123456789.-+qQcmreWnfSBTjJdDo#";
    Random random{0x7e57ab1eull};
    // Caches of their own: mutated fonts and images must not fill the shared ones.
    rgui::pdf::ImageCache images(16u << 20);
    rgui::pdf::FontCache fonts(8u << 20);
    const rgui::pdf::RenderEnvironment environment{&rgui::pdftest::TestFontIndex(), &images, &fonts};
    double slowest = 0;
    int rendered = 0;
    constexpr int kMutations = 10000;
    for (int i = 0; i < kMutations; i++) {
        size_t at = 0;
        switch (i % 4) {
            case 0:
            case 1: {
                const auto& range = contents[random.Below(contents.size())];
                at = range.first + random.Below(range.second - range.first);
                break;
            }
            case 2: at = streams[random.Below(streams.size())] + random.Below(2000); break;
            default: at = random.Below(pdf.size()); break;
        }
        at = std::min(at, pdf.size() - 1);
        std::string mutated = pdf;
        for (int k = 0; k < 1 + static_cast<int>(random.Below(3)); k++) {
            const size_t where = std::min(pdf.size() - 1, at + random.Below(16));
            mutated[where] = random.Below(4) == 0 ? static_cast<char>(random.Below(256))
                                                  : kBytes[random.Below(sizeof kBytes - 1)];
        }
        const auto start = std::chrono::steady_clock::now();
        const auto file = PdfFile::Parse(std::move(mutated), nullptr);
        if (!file) continue;
        int page = 0;
        for (size_t p = 0; p < contents.size(); p++) {
            if (at >= contents[p].first && at < contents[p].second) page = static_cast<int>(p);
        }
        if (page >= file->PageCount()) page = 0;
        Image image;
        RenderStats stats;
        if (RenderPage(*file, page, 64, image, &stats, environment)) rendered++;
        slowest = std::max(slowest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("render hardening: %d mutations, %d rendered; slowest %.2f ms\n", kMutations, rendered, slowest);
    CHECK(rendered > kMutations / 2);
    CHECK(slowest < 2000.0);
}
