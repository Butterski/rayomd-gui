// SPDX-License-Identifier: Apache-2.0
#include "runner.h"

#include "common/file_io.h"
#include "core/cff.h"
#include "gfx/budget.h"
#include "gfx/path.h"
#include "text/cff_outlines.h"
#include "text/fontchoice.h"
#include "text/fontindex.h"
#include "text/glyphcache.h"
#include "text/mapped_file.h"
#include "text/sfnt.h"
#include "text/textlayout.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace rgui::text;
namespace gfx = rgui::gfx;

#ifdef _WIN32
const char* const kMono = "C:\\Windows\\Fonts\\consola.ttf";
const char* const kSans = "C:\\Windows\\Fonts\\arial.ttf";
const char* const kCff = nullptr;
#else
const char* const kMono = "/usr/share/fonts/TTF/DejaVuSansMono.ttf";
const char* const kSans = "/usr/share/fonts/TTF/DejaVuSans.ttf";
const char* const kCff = "/usr/share/fonts/adobe-source-sans/SourceSans3-Regular.otf";
#endif

// A mapped font and its first face; prints a skip line when the file is missing.
struct TestFont {
    MappedFile file;
    FontFace face;

    bool Load(const char* path, const char* test) {
        if (path == nullptr || !file.Open(path)) {
            std::printf("skip %s: %s not found\n", test, path != nullptr ? path : "a CFF font");
            return false;
        }
        return face.Open(file.Data(), file.Size(), 0);
    }
};

int Contours(const gfx::Path& path) {
    int n = 0;
    for (gfx::Verb v : path.Verbs()) n += v == gfx::Verb::MoveTo ? 1 : 0;
    return n;
}

// The signed area enclosed by the flattened outline (shoelace), at a fine tolerance.
class AreaSink final : public gfx::LineSink {
public:
    void Line(gfx::PointF a, gfx::PointF b) override { twice += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y; }
    double twice = 0;
};

double OutlineArea(const gfx::Path& path, const gfx::Matrix& m) {
    AreaSink sink;
    gfx::Flatten(path, m, 0.01f, sink);
    return (sink.twice < 0 ? -sink.twice : sink.twice) / 2;
}

double CoverageSum(const GlyphBitmap& bitmap) {
    double sum = 0;
    for (uint8_t a : bitmap.alpha) sum += a;
    return sum / 255.0;
}

// For `o`, `g` and `@` at 200 px: the rasterized coverage within 1.5 % of the outline's area.
void CheckCoverage(const FontFace& face) {
    for (const char c : {'o', 'g', '@'}) {
        const uint16_t glyph = face.GlyphForCodePoint(static_cast<unsigned char>(c));
        CHECK(glyph != 0);
        gfx::Path path;
        CHECK(GlyphOutline(face, glyph, path));
        const double s = 200.0 / face.UnitsPerEm();
        const double area = OutlineArea(path, gfx::Matrix::Scale(s, -s));
        GlyphBitmap bitmap;
        RasterizeGlyph(face, glyph, 200.0f, 0.0f, bitmap);
        const double coverage = CoverageSum(bitmap);
        CHECK(area > 1000);
        CHECK_NEAR(coverage / area, 1.0, 0.015);
    }
}

std::vector<uint8_t> ReadBytes(const char* path) {
    std::vector<uint8_t> bytes;
    if (path != nullptr) RayoMd::File::ReadAll(std::filesystem::u8path(path), 64u << 20, bytes);
    return bytes;
}

// The bytes of a table of a plain sfnt.
std::vector<uint8_t> TableBytes(const std::vector<uint8_t>& font, const char tag[4]) {
    if (font.size() < 12) return {};
    const size_t tables = static_cast<size_t>(font[4] << 8 | font[5]);
    for (size_t i = 0; i < tables && 12 + i * 16 + 16 <= font.size(); i++) {
        const uint8_t* r = font.data() + 12 + i * 16;
        if (std::memcmp(r, tag, 4) != 0) continue;
        const uint32_t offset = static_cast<uint32_t>(r[8] << 24 | r[9] << 16 | r[10] << 8 | r[11]);
        const uint32_t length = static_cast<uint32_t>(r[12] << 24 | r[13] << 16 | r[14] << 8 | r[15]);
        if (static_cast<uint64_t>(offset) + length > font.size()) return {};
        return std::vector<uint8_t>(font.begin() + offset, font.begin() + offset + length);
    }
    return {};
}

std::string Fixture(const char* name) {
    return std::string(rgui::test::SourceDir()) + "/tests/data/text/" + name;
}

std::string ReadText(const std::string& path) {
    std::string text;
    RayoMd::File::ReadAll(std::filesystem::u8path(path), 1u << 20, text);
    return text;
}

#ifndef _WIN32
bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
#endif

// Decodes every part of a face the toolkit and the preview use, for the mutation tests: the
// result does not matter, only that it stays in bounds and ends.
void Exercise(const uint8_t* data, size_t size, uint32_t& checksum) {
    FaceInfo info;
    if (ReadFaceInfo(data, size, 0, info)) checksum += static_cast<uint32_t>(info.families.size());
    FontFace face;
    if (!face.Open(data, size, 0)) return;
    gfx::Path path;
    const char32_t probe[] = U"Ag@oÄ€中";
    for (char32_t cp : probe) {
        if (cp == 0) break;
        const uint16_t glyph = face.GlyphForCodePoint(cp);
        checksum += glyph + face.AdvanceWidth(glyph);
        if (GlyphOutline(face, glyph, path)) checksum += static_cast<uint32_t>(path.Verbs().size());
    }
    for (uint32_t g = 0; g < 24; g++) {
        const uint16_t glyph = static_cast<uint16_t>((g * 2654435761u) % face.GlyphCount());
        if (GlyphOutline(face, glyph, path)) checksum += static_cast<uint32_t>(path.Points().size());
    }
    GlyphBitmap bitmap;
    RasterizeGlyph(face, face.GlyphForCodePoint('g'), 24.0f, 0.25f, bitmap);
    checksum += static_cast<uint32_t>(bitmap.alpha.size());
}

// Mutates bytes inside the tables a reader decodes (picked uniformly by table, so a 600 KB glyf
// does not swallow every mutation), plus the table directory itself.
void Mutate(std::vector<uint8_t>& bytes, std::mt19937& rng) {
    const size_t tables = static_cast<size_t>(bytes[4] << 8 | bytes[5]);
    const size_t pick = rng() % (tables + 1);
    size_t begin = 0, length = 12 + tables * 16;
    if (pick < tables) {
        const uint8_t* r = bytes.data() + 12 + pick * 16;
        begin = static_cast<size_t>(r[8] << 24 | r[9] << 16 | r[10] << 8 | r[11]);
        length = static_cast<size_t>(r[12] << 24 | r[13] << 16 | r[14] << 8 | r[15]);
        // Big tables: mostly their first part, where headers, cmap segments and early glyphs are.
        if (length > 4096 && rng() % 2 == 0) length = 4096;
    }
    if (length == 0 || begin + length > bytes.size()) return;
    const int count = 1 + static_cast<int>(rng() % 3);
    for (int i = 0; i < count; i++) {
        const size_t at = begin + rng() % length;
        switch (rng() % 4) {
        case 0: bytes[at] = static_cast<uint8_t>(rng()); break;
        case 1: bytes[at] = 0xFF; break;
        case 2: bytes[at] = 0; break;
        default: bytes[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
        }
    }
}

void FuzzFont(const char* path, const char* test, int mutations) {
    const std::vector<uint8_t> original = ReadBytes(path);
    if (original.size() < 12) {
        std::printf("skip %s: %s not found\n", test, path != nullptr ? path : "a CFF font");
        return;
    }
    std::mt19937 rng(12345);
    uint32_t checksum = 0;
    const auto begin = std::chrono::steady_clock::now();
    // Truncations: every length in the header area, then spread over the file.
    for (size_t length = 0; length < 1024; length += 7) Exercise(original.data(), length, checksum);
    for (int i = 0; i < 200; i++) Exercise(original.data(), rng() % original.size(), checksum);
    std::vector<uint8_t> bytes;
    for (int i = 0; i < mutations; i++) {
        bytes = original;
        Mutate(bytes, rng);
        Exercise(bytes.data(), bytes.size(), checksum);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::printf("  %s: %d mutations + 347 truncations in %.0f ms (checksum %u)\n", test, mutations, ms, checksum);
}

} // namespace

RGUI_TEST("text.mapped_file") {
    MappedFile missing;
    CHECK(!missing.Open(std::string(rgui::test::BuildDir()) + "/no-such-font.ttf"));
    CHECK(!missing.IsOpen());
    const std::string empty = std::string(rgui::test::BuildDir()) + "/text-empty-file";
    RayoMd::File::WriteAll(std::filesystem::u8path(empty), "");
    MappedFile none;
    CHECK(!none.Open(empty));
    MappedFile fixture;
    CHECK(fixture.Open(Fixture("fonts.conf")));
    CHECK_EQ(fixture.Size(), ReadText(Fixture("fonts.conf")).size());
    MappedFile moved(std::move(fixture));
    CHECK(moved.IsOpen());
    CHECK(!fixture.IsOpen());
    CHECK_EQ(std::memcmp(moved.Data(), "<?xml", 5), 0);
}

RGUI_TEST("text.fixed_pitch") {
    TestFont font;
    if (!font.Load(kMono, "text.fixed_pitch")) return;
    CHECK(font.face.FixedPitch());
    const uint16_t space = font.face.AdvanceWidth(font.face.GlyphForCodePoint(' '));
    CHECK(space > 0);
    for (uint32_t cp = 0x21; cp < 0x7F; cp++) {
        const uint16_t glyph = font.face.GlyphForCodePoint(cp);
        CHECK(glyph != 0);
        CHECK_EQ(font.face.AdvanceWidth(glyph), space);
    }
}

RGUI_TEST("text.sfnt_tables") {
    TestFont font;
    if (!font.Load(kSans, "text.sfnt_tables")) return;
    const FontFace& face = font.face;
    CHECK_EQ(face.OutlineFormat(), FontFace::Outlines::TrueType);
    CHECK_EQ(face.UnitsPerEm(), 2048);
    CHECK(face.Ascender() > 0);
    CHECK(face.Descender() < 0);
    CHECK_EQ(face.Weight(), 400);
    CHECK(!face.Italic());
    CHECK(!face.FixedPitch());
    CHECK(face.GlyphForCodePoint('A') != face.GlyphForCodePoint('B'));
    CHECK_EQ(face.GlyphForCodePoint(0x10FFFD), 0);   // private use, in no font
    CHECK_EQ(face.GlyphForCodePoint(0xFFFFFFFFu), 0);
    // Beyond the glyph count: no outline, no advance.
    gfx::Path path;
    CHECK(!GlyphOutline(face, face.GlyphCount(), path));
    CHECK_EQ(face.AdvanceWidth(face.GlyphCount()), 0);
    // A space has an advance and an empty outline.
    const uint16_t space = face.GlyphForCodePoint(' ');
    CHECK(GlyphOutline(face, space, path));
    CHECK(path.Empty());
    CHECK(face.AdvanceWidth(space) > 0);
    // Two faces of the same bytes are distinct glyph sources.
    FontFace again;
    CHECK(again.Open(font.file.Data(), font.file.Size(), 0));
    CHECK(again.SourceId() != face.SourceId());
    CHECK(!again.Open(font.file.Data(), font.file.Size(), 1));   // not a collection

    FaceInfo info;
    CHECK(ReadFaceInfo(font.file.Data(), font.file.Size(), 0, info));
    CHECK(!info.families.empty());
#ifndef _WIN32
    CHECK_EQ(info.families.front(), std::string("DejaVu Sans"));
    CHECK_EQ(info.style, std::string("Book"));
#endif
    CHECK_EQ(FaceCount(font.file.Data(), font.file.Size()), 1u);
}

RGUI_TEST("text.collection") {
    // A two-face ttcf around one sfnt: both faces read the same tables (offsets are absolute).
    const std::vector<uint8_t> sfnt = ReadBytes(kSans);
    if (sfnt.size() < 12) {
        std::printf("skip text.collection: %s not found\n", kSans);
        return;
    }
    std::vector<uint8_t> ttc = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 20, 0, 0, 0, 20};
    const size_t shift = ttc.size();
    ttc.insert(ttc.end(), sfnt.begin(), sfnt.end());
    // Move every table offset by the header.
    const size_t tables = static_cast<size_t>(sfnt[4] << 8 | sfnt[5]);
    for (size_t i = 0; i < tables; i++) {
        uint8_t* r = ttc.data() + shift + 12 + i * 16 + 8;
        uint32_t offset = static_cast<uint32_t>(r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3]) + static_cast<uint32_t>(shift);
        r[0] = static_cast<uint8_t>(offset >> 24);
        r[1] = static_cast<uint8_t>(offset >> 16);
        r[2] = static_cast<uint8_t>(offset >> 8);
        r[3] = static_cast<uint8_t>(offset);
    }
    CHECK_EQ(FaceCount(ttc.data(), ttc.size()), 2u);
    FontFace first, second, third;
    CHECK(first.Open(ttc.data(), ttc.size(), 0));
    CHECK(second.Open(ttc.data(), ttc.size(), 1));
    CHECK(!third.Open(ttc.data(), ttc.size(), 2));
    CHECK_EQ(second.GlyphForCodePoint('g'), first.GlyphForCodePoint('g'));
    // numFonts beyond the header's room is capped to what fits.
    ttc[11] = 200;
    CHECK_EQ(FaceCount(ttc.data(), ttc.size()), kMaxCollectionFaces);
}

RGUI_TEST("text.coverage_glyf") {
    TestFont font;
    if (!font.Load(kSans, "text.coverage_glyf")) return;
    CheckCoverage(font.face);
}

RGUI_TEST("text.coverage_cff") {
    TestFont font;
    if (!font.Load(kCff, "text.coverage_cff")) return;
    CHECK_EQ(font.face.OutlineFormat(), FontFace::Outlines::Cff);
    CheckCoverage(font.face);
}

RGUI_TEST("text.composite") {
    TestFont font;
    if (!font.Load(kSans, "text.composite")) return;
    gfx::Path a, umlaut;
    CHECK(GlyphOutline(font.face, font.face.GlyphForCodePoint('A'), a));
    CHECK(GlyphOutline(font.face, font.face.GlyphForCodePoint(0xC4), umlaut));
    CHECK(Contours(umlaut) > Contours(a));
    // The base letter sits where A does; the dieresis above its top.
    const gfx::RectF ab = a.Bounds();
    const gfx::RectF ub = umlaut.Bounds();
    CHECK_NEAR(ub.x, ab.x, 1.0);
    CHECK_NEAR(ub.y, ab.y, 1.0);
    CHECK(ub.y + ub.h > ab.y + ab.h + 100);
}

RGUI_TEST("text.cff_bare") {
    TestFont font;
    if (!font.Load(kCff, "text.cff_bare")) return;
    const std::vector<uint8_t> otf = ReadBytes(kCff);
    const std::vector<uint8_t> table = TableBytes(otf, "CFF ");
    CHECK(!table.empty());
    CffOutlines cff;
    CHECK(cff.Open(table.data(), table.size()));   // count from the CharStrings INDEX
    CHECK_EQ(cff.GlyphCount(), font.face.GlyphCount());
    CHECK(!cff.IsCidKeyed());
    CHECK_NEAR(cff.FontMatrix().a, 0.001, 1e-12);
    CffOutlines wrongCount;
    CHECK(!wrongCount.Open(table.data(), table.size(), static_cast<uint16_t>(font.face.GlyphCount() + 1)));

    for (const char c : {'A', 'o', 'g', '@'}) {
        const uint16_t glyph = font.face.GlyphForCodePoint(static_cast<unsigned char>(c));
        // The same outline as the face's, the same advance as hmtx.
        gfx::Path bare, faced;
        double advance = 0;
        CHECK(cff.Outline(glyph, bare, gfx::Matrix(), &advance));
        CHECK(GlyphOutline(font.face, glyph, faced));
        CHECK_EQ(bare.Verbs().size(), faced.Verbs().size());
        CHECK_NEAR(advance, font.face.AdvanceWidth(glyph), 0.5);
        double width = 0;
        CHECK(cff.Advance(glyph, width));
        CHECK_NEAR(width, advance, 1e-9);
        // Name-keyed: the Standard encoding and the charset agree with cmap for ASCII letters.
        CHECK_EQ(cff.Encoding().GlyphFor(static_cast<uint8_t>(c)), glyph);
        CHECK_EQ(cff.Charset().GlyphFor(static_cast<uint16_t>(c - 31)), glyph);   // SIDs of ASCII
    }
    // The bare CFF of a PDF is the same bytes: truncated, it fails cleanly.
    CffOutlines cut;
    CHECK(!cut.Open(table.data(), table.size() / 2));
}

RGUI_TEST("text.cff_subsets") {
    // RayoMD's own PDF font programs (core/cff.h writers): the CID-keyed subset's charset maps
    // CIDs, the simple font's encoding maps codes.
    namespace Cff = TinyPdf::Internal::Cff;
    TestFont font;
    if (!font.Load(kCff, "text.cff_subsets")) return;
    const std::vector<uint8_t> otf = ReadBytes(kCff);
    const std::vector<uint8_t> table = TableBytes(otf, "CFF ");
    Cff::CffFont source;
    CHECK(Cff::ParseCff(table.data(), table.size(), font.face.GlyphCount(), source));
    const uint16_t a = font.face.GlyphForCodePoint('A');
    const uint16_t o = font.face.GlyphForCodePoint('o');
    std::string subset;
    CHECK(Cff::WriteCidKeyedSubset(source, "Subset", {{0x41, a}, {0x6F, o}}, subset));
    CffOutlines cid;
    CHECK(cid.Open(reinterpret_cast<const uint8_t*>(subset.data()), subset.size()));
    CHECK(cid.IsCidKeyed());
    CHECK_EQ(cid.GlyphCount(), 3);
    CHECK_EQ(cid.Charset().GlyphFor(0x41), 1);
    CHECK_EQ(cid.Charset().GlyphFor(0x6F), 2);
    CHECK_EQ(cid.Charset().GlyphFor(0x42), 0);
    CHECK_EQ(cid.Encoding().GlyphFor('A'), 0);
    gfx::Path fromSubset, fromFont;
    CHECK(cid.Outline(1, fromSubset, gfx::Matrix()));
    CHECK(GlyphOutline(font.face, a, fromFont));
    CHECK_EQ(fromSubset.Verbs().size(), fromFont.Verbs().size());
    const gfx::Matrix m = cid.GlyphMatrix(1);
    CHECK_NEAR(m.a, 0.001, 1e-9);
    CHECK_NEAR(m.d, 0.001, 1e-9);

    std::string programA, programO;
    CHECK(Cff::Desubroutinize(source, a, programA));
    CHECK(Cff::Desubroutinize(source, o, programO));
    const Cff::SimpleGlyph glyphs[] = {{'A', 34, {}, programA}, {'o', 80, {}, programO}};
    const int16_t bbox[4] = {0, -200, 1000, 800};
    std::string simple;
    CHECK(Cff::WriteSimpleFont("Simple", glyphs, 2, bbox, simple));
    CffOutlines named;
    CHECK(named.Open(reinterpret_cast<const uint8_t*>(simple.data()), simple.size()));
    CHECK(!named.IsCidKeyed());
    CHECK_EQ(named.Encoding().GlyphFor('A'), 1);
    CHECK_EQ(named.Encoding().GlyphFor('o'), 2);
    CHECK_EQ(named.Encoding().GlyphFor('B'), 0);
    CHECK_EQ(named.Charset().IdOf(1), 34);
    gfx::Path simplePath;
    CHECK(named.Outline(2, simplePath, gfx::Matrix()));
    CHECK_EQ(Contours(simplePath), 2);   // o: outer and inner
}

RGUI_TEST("text.glyph_cache") {
    TestFont font;
    if (!font.Load(kSans, "text.glyph_cache")) return;
    const uint16_t g = font.face.GlyphForCodePoint('g');
    GlyphCache cache;
    const GlyphBitmap* first = cache.Get(font.face, g, 16.0f, 10.0f);
    const GlyphBitmap* again = cache.Get(font.face, g, 16.0f, 10.1f);   // same quarter-pixel phase
    CHECK(first == again);
    CHECK_EQ(cache.Count(), 1u);
    CHECK(first->width > 0 && first->height > 0);
    CHECK(first->top < 0);                          // above the baseline
    CHECK(first->top + first->height > 0);          // g descends below it
    // Another phase shifts the coverage to the right.
    const GlyphBitmap* shifted = cache.Get(font.face, g, 16.0f, 10.5f);
    CHECK(shifted != first);
    CHECK_EQ(cache.Count(), 2u);
    double cx0 = 0, cx1 = 0, s0 = 0, s1 = 0;
    for (int y = 0; y < first->height; y++) {
        for (int x = 0; x < first->width; x++) {
            const double a = first->alpha[static_cast<size_t>(y * first->width + x)];
            cx0 += a * (x + first->left);
            s0 += a;
        }
    }
    for (int y = 0; y < shifted->height; y++) {
        for (int x = 0; x < shifted->width; x++) {
            const double a = shifted->alpha[static_cast<size_t>(y * shifted->width + x)];
            cx1 += a * (x + shifted->left);
            s1 += a;
        }
    }
    CHECK_NEAR(cx1 / s1 - cx0 / s0, 0.5, 0.05);
    CHECK(cache.Find(GlyphCache::KeyFor(font.face, g, 16.0f, 3.0f)) == first);
    CHECK(cache.Find(GlyphCache::KeyFor(font.face, g, 17.0f, 3.0f)) == nullptr);
    // A space is cached empty.
    const GlyphBitmap* space = cache.Get(font.face, font.face.GlyphForCodePoint(' '), 16.0f, 0.0f);
    CHECK_EQ(space->width, 0);

    // A small budget: glyphs of the current frame stay, older ones go.
    GlyphCache small(16 * 1024);
    std::vector<const GlyphBitmap*> current;
    for (uint32_t cp = 'A'; cp <= 'Z'; cp++) current.push_back(small.Get(font.face, font.face.GlyphForCodePoint(cp), 48.0f, 0.0f));
    const size_t frameBytes = small.Bytes();
    CHECK(frameBytes > 16 * 1024);   // over budget within one frame, nothing evicted
    CHECK_EQ(small.Count(), 26u);
    CHECK(small.Find(GlyphCache::KeyFor(font.face, font.face.GlyphForCodePoint('A'), 48.0f, 0.0f)) == current[0]);
    small.BeginFrame();
    small.Get(font.face, font.face.GlyphForCodePoint('a'), 48.0f, 0.0f);
    CHECK(small.Bytes() <= 16 * 1024);
    CHECK(small.Find(GlyphCache::KeyFor(font.face, font.face.GlyphForCodePoint('A'), 48.0f, 0.0f)) == nullptr);
    CHECK(small.Find(GlyphCache::KeyFor(font.face, font.face.GlyphForCodePoint('a'), 48.0f, 0.0f)) != nullptr);
}

// Within one frame nothing is evicted, but the cache never passes twice its budget: a page of
// thousands of distinct glyph sizes stays bounded (the rest are rasterized and handed out
// without being kept).
RGUI_TEST("text.glyph_cache_ceiling") {
    TestFont font;
    if (!font.Load(kSans, "text.glyph_cache_ceiling")) return;
    const uint16_t g = font.face.GlyphForCodePoint('W');
    GlyphCache cache(256 * 1024);
    const GlyphBitmap* last = nullptr;
    for (int i = 0; i < 2000; i++) {
        last = cache.Get(font.face, g, 40.0f + i * 0.05f, 0.0f);
        CHECK(cache.Bytes() <= 2 * cache.Budget());
    }
    // The last ones were not kept, yet handed out whole.
    CHECK(last != nullptr && last->width > 100 && last->height > 100);
    CHECK(cache.Find(GlyphCache::KeyFor(font.face, g, 40.0f + 1999 * 0.05f, 0.0f)) == nullptr);
    CHECK(cache.Count() < 2000);
    // A budget that runs out leaves the glyph out of the cache.
    gfx::WorkBudget budget;
    budget.pixels = 10;
    const GlyphBitmap* starved = cache.Get(font.face, GlyphCache::KeyFor(font.face, g, 300.0f, 0.0f), &budget);
    CHECK(budget.Stopped());
    CHECK(starved != nullptr && starved->width == 0);
    CHECK(cache.Find(GlyphCache::KeyFor(font.face, g, 300.0f, 0.0f)) == nullptr);
}

namespace {

// A glyph source that counts the outlines it hands out.
struct CountingSource final : rgui::text::GlyphSource {
    const rgui::text::GlyphSource& inner;
    mutable int paths = 0;
    explicit CountingSource(const rgui::text::GlyphSource& source) : inner(source) {}
    uint32_t SourceId() const override { return inner.SourceId(); }
    bool GlyphPath(uint16_t glyph, gfx::Path& out) const override {
        paths++;
        return inner.GlyphPath(glyph, out);
    }
    gfx::Matrix UnitsToEm(uint16_t glyph) const override { return inner.UnitsToEm(glyph); }
};

} // namespace

// Caches over one store rasterize a glyph once between them, with the same bitmap; a glyph whose
// budget ran out is not stored; the store keeps to its budget.
RGUI_TEST("text.glyph_store") {
    TestFont font;
    if (!font.Load(kSans, "text.glyph_store")) return;
    const CountingSource source(font.face);
    const uint16_t g = font.face.GlyphForCodePoint('g');
    rgui::text::GlyphStore store;
    GlyphCache a(GlyphCache::kDefaultBudget, &store), b(GlyphCache::kDefaultBudget, &store);
    const GlyphBitmap* fromA = a.Get(source, g, 16.0f, 10.25f);
    const GlyphBitmap* fromB = b.Get(source, g, 16.0f, 10.25f);
    CHECK_EQ(source.paths, 1);
    CHECK(fromA != fromB);
    CHECK(fromA->left == fromB->left && fromA->top == fromB->top && fromA->width == fromB->width &&
          fromA->height == fromB->height && fromA->alpha == fromB->alpha);
    GlyphBitmap alone;
    rgui::text::RasterizeGlyph(font.face, g, 16.0f, 0.25f, alone);
    CHECK(alone.alpha == fromB->alpha);
    b.Get(source, g, 16.0f, 10.75f);
    CHECK_EQ(source.paths, 2);
    CHECK(store.Bytes() > 0);
    // Out of budget: not stored, so the next cache rasterizes it again.
    gfx::WorkBudget starved;
    starved.pixels = 10;
    a.Get(source, GlyphCache::KeyFor(source, g, 300.0f, 0.0f), &starved);
    CHECK(starved.Stopped());
    GlyphCache c(GlyphCache::kDefaultBudget, &store);
    const GlyphBitmap* big = c.Get(source, g, 300.0f, 0.0f);
    CHECK(big->width > 100);
    CHECK_EQ(source.paths, 4);
    // A store of 64 KiB keeps about that much.
    rgui::text::GlyphStore small(64 * 1024);
    GlyphCache d(GlyphCache::kDefaultBudget, &small);
    for (int i = 0; i < 400; i++) d.Get(source, g, 20.0f + i * 0.25f, 0.0f);
    CHECK(small.Bytes() <= 64 * 1024);
}

RGUI_TEST("text.fontconfig_user") {
    // Exactly what `omarchy-font-set "Fira Code"` writes.
    const std::string xml = ReadText(Fixture("fonts.conf"));
    CHECK(!xml.empty());
    const std::vector<std::string> families = MonospaceFamilies(xml, false);
    CHECK_EQ(families.size(), 1u);
    if (!families.empty()) CHECK_EQ(families[0], std::string("Fira Code"));

    FontChoicePaths paths;
    paths.userFontsConf = Fixture("fonts.conf");
    paths.omarchyFontsConf = Fixture("50-omarchy.conf");
    const std::vector<std::string> candidates = InterfaceFamilyCandidates(paths);
    CHECK(candidates.size() >= 3);
    if (candidates.size() >= 3) {
        CHECK_EQ(candidates[0], std::string("Fira Code"));
        CHECK_EQ(candidates[1], std::string("JetBrainsMono Nerd Font"));
        CHECK_EQ(candidates[2], std::string("Noto Sans Mono CJK JP"));
    }
    // A missing user file: Omarchy's system choice comes first.
    paths.userFontsConf = Fixture("no-such-fonts.conf");
    const std::vector<std::string> systemFirst = InterfaceFamilyCandidates(paths);
    CHECK(!systemFirst.empty() && systemFirst.front() == "JetBrainsMono Nerd Font");
}

RGUI_TEST("text.fontconfig_omarchy") {
    // 50-omarchy.conf: the language-specific CJK matches (with another test) and the appends do
    // not count; the plain monospace match does, then the aliases.
    const std::vector<std::string> families = MonospaceFamilies(ReadText(Fixture("50-omarchy.conf")), true);
    CHECK_EQ(families.size(), 3u);
    if (families.size() == 3) {
        CHECK_EQ(families[0], std::string("JetBrainsMono Nerd Font"));
        CHECK_EQ(families[1], std::string("Noto Sans Mono CJK JP"));
        CHECK_EQ(families[2], std::string("Noto Color Emoji"));
    }
    CHECK_EQ(MonospaceFamilies(ReadText(Fixture("50-omarchy.conf")), false).size(), 1u);

    // The alias path alone, entities decoded.
    const char* aliasOnly =
        "<?xml version=\"1.0\"?>\n<fontconfig>\n  <!-- a <comment> -->\n"
        "  <alias><family>sans-serif</family><prefer><family>Wrong</family></prefer></alias>\n"
        "  <alias binding=\"same\">\n    <family> monospace </family>\n"
        "    <prefer><family>Mono &amp; Co</family><family>Second</family></prefer>\n  </alias>\n</fontconfig>\n";
    const std::vector<std::string> alias = MonospaceFamilies(aliasOnly, true);
    CHECK_EQ(alias.size(), 1u);
    if (!alias.empty()) CHECK_EQ(alias[0], std::string("Mono & Co"));
    CHECK(MonospaceFamilies(aliasOnly, false).empty());

    // An appended family is not the first choice; a match for another family is ignored.
    CHECK(MonospaceFamilies("<fontconfig><match target=\"pattern\"><test name=\"family\"><string>monospace</string>"
                            "</test><edit name=\"family\" mode=\"append\"><string>Late</string></edit></match>"
                            "<match target=\"font\"><test name=\"family\"><string>monospace</string></test>"
                            "<edit name=\"family\"><string>FontTarget</string></edit></match></fontconfig>",
        true)
              .empty());
    // Malformed XML gives nothing: unclosed, mismatched, unterminated attribute, too deep.
    CHECK(MonospaceFamilies("<fontconfig><match target=\"pattern\">", true).empty());
    CHECK(MonospaceFamilies("<fontconfig><alias><family>monospace</family><prefer><family>X</family></accept>"
                            "</alias></fontconfig>",
        true)
              .empty());
    CHECK(MonospaceFamilies("<fontconfig><match target=\"pattern></match></fontconfig>", true).empty());
    std::string deep = "<fontconfig>";
    for (int i = 0; i < 100; i++) deep += "<a>";
    CHECK(MonospaceFamilies(deep, true).empty());
    CHECK(MonospaceFamilies(std::string(2u << 20, ' '), true).empty());
}

#ifndef _WIN32
RGUI_TEST("text.font_index") {
    FontIndex index;
    index.Start();
    const std::vector<FontEntry>& entries = index.Entries();
    std::printf("  font index: %zu faces in %.1f ms (%zu directories)\n", entries.size(), index.ScanMilliseconds(),
        FontIndex::DefaultDirectories().size());
    if (index.Find("DejaVu Sans", 400, false) == nullptr) {
        std::printf("skip text.font_index: DejaVu Sans not installed\n");
        return;
    }
    struct Want {
        const char* family;
        int weight;
        bool italic;
        const char* file;
    } const wants[] = {
        {"DejaVu Sans", 400, false, "/DejaVuSans.ttf"},
        {"dejavu sans", 700, false, "/DejaVuSans-Bold.ttf"},
        {"DejaVu Sans", 400, true, "/DejaVuSans-Oblique.ttf"},
        {"DejaVu Sans", 700, true, "/DejaVuSans-BoldOblique.ttf"},
        {"DejaVu Sans", 200, false, "/DejaVuSans-ExtraLight.ttf"},
        {"DejaVu Sans", 900, false, "/DejaVuSans-Bold.ttf"},
        {"DEJAVU SANS MONO", 400, false, "/DejaVuSansMono.ttf"},
        {"DejaVu Sans Mono", 650, true, "/DejaVuSansMono-BoldOblique.ttf"},
        {"DejaVu Sans Condensed", 400, false, "/DejaVuSansCondensed.ttf"},
    };
    for (const Want& want : wants) {
        const FontEntry* entry = index.Find(want.family, want.weight, want.italic);
        CHECK(entry != nullptr);
        if (entry != nullptr && !EndsWith(entry->path, want.file)) {
            rgui::test::Fail(__FILE__, __LINE__, std::string(want.family) + " " + std::to_string(want.weight) +
                (want.italic ? " italic" : "") + " -> " + entry->path);
        }
    }
    const FontEntry* mono = index.Find("DejaVu Sans Mono", 400, false);
    CHECK(mono != nullptr && mono->fixedPitch);
    CHECK(index.Find("No Such Family", 400, false) == nullptr);
    CHECK(index.Find("DejaVu", 400, false) == nullptr);   // whole names only

    // A directory override, read synchronously when nothing started the scan.
    FontIndex local;
    local.SetDirectories({std::string(rgui::test::SourceDir()) + "/tests/data/text", "/no/such/dir"});
    CHECK(local.Entries().empty());
}

RGUI_TEST("text.font_set") {
    FontIndex index;
    index.SetDirectories({"/usr/share/fonts"});
    if (index.Find("DejaVu Sans Mono", 400, false) == nullptr || index.Find("DejaVu Sans", 400, false) == nullptr) {
        std::printf("skip text.font_set: DejaVu not installed\n");
        return;
    }
    FontSet fonts;
    CHECK(fonts.Load(index, {"Fira Code", "DejaVu Sans Mono"}, {"DejaVu Sans"}));
    CHECK_EQ(fonts.Family(), std::string("DejaVu Sans Mono"));
    CHECK(fonts.Status().empty());
    CHECK(fonts.Face(false).FixedPitch());
    CHECK_EQ(fonts.Face(false).Weight(), 400);
    CHECK_EQ(fonts.Face(true).Weight(), 700);

    // No candidate installed: the first fixed-pitch face, and a status naming the wanted family.
    FontSet missing;
    CHECK(missing.Load(index, {"No Such Mono"}, {}));
    CHECK(missing.Face(false).FixedPitch());
    CHECK_EQ(missing.Status().rfind("Interface font No Such Mono not found; using ", 0), 0u);
    CHECK(missing.Status().find('/') == std::string::npos);   // the file name, not the path

    // A family without a bold face uses its regular face for bold.
    FontSet light;
    CHECK(light.Load(index, {"DejaVu Math TeX Gyre"}, {}));
    if (light.Family() == "DejaVu Math TeX Gyre") CHECK(&light.Face(true) == &light.Face(false));

    FontIndex empty;
    empty.SetDirectories({});
    FontSet nothing;
    CHECK(!nothing.Load(empty, {"DejaVu Sans Mono"}, {}));
    CHECK(ShapeRun("abc", nothing, 12.0f).glyphs.empty());
}

// The interface font from its known files: the index is not scanned for it (startup does not
// wait), and serves the fallbacks once it is.
RGUI_TEST("text.font_set_known_files") {
    const std::string dir = "/usr/share/fonts/TTF/";
    MappedFile probe;
    if (!probe.Open(dir + "DejaVuSansMono.ttf") || !MappedFile().Open(dir + "DejaVuSans.ttf")) {
        std::printf("skip text.font_set_known_files: DejaVu not installed\n");
        return;
    }
    FontIndex index;
    index.SetDirectories({"/usr/share/fonts"});
    FontSet fonts;
    const std::vector<KnownFontFiles> files = {
        {"No Such Mono", dir + "NoSuchMono.ttf", ""},
        // The file does not name the family: skipped.
        {"DejaVu Sans Mono", dir + "DejaVuSans.ttf", ""},
        // The bold file is not bold: none.
        {"DejaVu Sans Mono", dir + "DejaVuSansMono.ttf", dir + "DejaVuSansMono.ttf"},
    };
    CHECK(fonts.LoadKnown(index, files, {"DejaVu Sans"}));
    CHECK(!index.Scanned());
    CHECK_EQ(fonts.Family(), std::string("DejaVu Sans Mono"));
    CHECK(fonts.Status().empty());
    CHECK(fonts.Face(false).FixedPitch());
    CHECK(&fonts.Face(true) == &fonts.Face(false));
    CHECK(!index.Scanned());
    // A code point DejaVu Sans has and the family lacks: its fallback needs the index, which has
    // not scanned, so Resolve passes it over without waiting (and without caching the answer)...
    MappedFile sansFile;
    FontFace mono, sans;
    CHECK(sansFile.Open(dir + "DejaVuSans.ttf") && sans.Open(sansFile.Data(), sansFile.Size(), 0));
    CHECK(mono.Open(probe.Data(), probe.Size(), 0));
    uint32_t cp = 0;
    for (uint32_t c = 0x100; c < 0x3000 && cp == 0; c++) {
        if (mono.GlyphForCodePoint(c) == 0 && sans.GlyphForCodePoint(c) != 0) cp = c;
    }
    CHECK(cp != 0);
    FontSet::Resolved resolved = fonts.Resolve(cp, false);
    CHECK_EQ(resolved.glyph, uint16_t(0));
    CHECK(!index.Scanned());
    CHECK(fonts.WaitingForIndex());
    // ... and finds it once the scan has ended.
    index.Wait();
    resolved = fonts.Resolve(cp, false);
    CHECK(resolved.glyph != 0);
    CHECK(resolved.face != &fonts.Face(false));

    FontSet withBold;
    CHECK(withBold.LoadKnown(index, {{"DejaVu Sans Mono", dir + "DejaVuSansMono.ttf", dir + "DejaVuSansMono-Bold.ttf"}}, {}));
    CHECK_EQ(withBold.Face(true).Weight(), 700);
    FontSet none;
    CHECK(!none.LoadKnown(index, {{"DejaVu Sans Mono", dir + "NoSuchMono.ttf", ""}}, {}));
    // Linux takes its family from fontconfig: no known files.
#ifndef _WIN32
    CHECK(KnownInterfaceFontFiles().empty());
#endif
}

RGUI_TEST("text.shape_run") {
    FontIndex index;
    index.SetDirectories({"/usr/share/fonts"});
    if (index.Find("DejaVu Sans Mono", 400, false) == nullptr || index.Find("DejaVu Sans", 400, false) == nullptr) {
        std::printf("skip text.shape_run: DejaVu not installed\n");
        return;
    }
    FontSet fonts;
    CHECK(fonts.Load(index, {"DejaVu Sans Mono"}, {"No Such Fallback", "DejaVu Sans"}));
    const FontFace& mono = fonts.Face(false);
    // A code point DejaVu Sans has and DejaVu Sans Mono lacks.
    FontSet sansOnly;
    CHECK(sansOnly.Load(index, {"DejaVu Sans"}, {}));
    const FontFace& sans = sansOnly.Face(false);
    uint32_t missingCp = 0;
    for (uint32_t cp = 0x100; cp < 0x3000 && missingCp == 0; cp++) {
        if (mono.GlyphForCodePoint(cp) == 0 && sans.GlyphForCodePoint(cp) != 0) missingCp = cp;
    }
    CHECK(missingCp != 0);
    std::string text = "a";
    char encoded[4];
    const int n = missingCp < 0x800 ? 2 : 3;
    if (n == 2) {
        encoded[0] = static_cast<char>(0xC0 | (missingCp >> 6));
        encoded[1] = static_cast<char>(0x80 | (missingCp & 0x3F));
    } else {
        encoded[0] = static_cast<char>(0xE0 | (missingCp >> 12));
        encoded[1] = static_cast<char>(0x80 | ((missingCp >> 6) & 0x3F));
        encoded[2] = static_cast<char>(0x80 | (missingCp & 0x3F));
    }
    text.append(encoded, static_cast<size_t>(n));
    text += "b\xFF\xF4\x8F\xBF\xBD";   // b, a stray byte, U+10FFFD (in no font)
    const GlyphRun run = ShapeRun(text, fonts, 20.0f);
    CHECK_EQ(run.glyphs.size(), 5u);
    if (run.glyphs.size() == 5) {
        const PositionedGlyph* g = run.glyphs.data();
        CHECK(g[0].face == &mono);
        CHECK(g[1].face != &mono);
        CHECK(g[1].face != nullptr && g[1].face->GlyphForCodePoint(missingCp) == g[1].glyph);
        CHECK(g[1].glyph != 0);
        CHECK(g[2].face == &mono);
        CHECK_EQ(g[2].byteOffset, static_cast<uint32_t>(1 + n));
        CHECK_EQ(g[3].byteOffset, static_cast<uint32_t>(2 + n));
        CHECK_EQ(g[4].face, &mono);   // in no font: glyph 0 of the main face
        CHECK_EQ(g[4].glyph, 0);
        const float cell = mono.AdvanceWidth(mono.GlyphForCodePoint('a')) * 20.0f / mono.UnitsPerEm();
        CHECK_NEAR(g[0].advance, cell, 1e-4);
        CHECK_NEAR(g[1].x, cell, 1e-4);
        float x = 0;
        for (const PositionedGlyph& p : run.glyphs) {
            CHECK_NEAR(p.x, x, 1e-3);
            x += p.advance;
        }
        CHECK_NEAR(run.advance, x, 1e-3);
    }
    // Bold runs use the bold face.
    const GlyphRun bold = ShapeRun("a", fonts, 20.0f, true);
    CHECK(!bold.glyphs.empty() && bold.glyphs[0].face == &fonts.Face(true));

    // Throughput: a mixed line, shaped over and over (the per-code-point cache warm).
    const std::string line = "The quick brown fox jumps over the lazy dog: zażółć gęślą jaźń 0123456789 {}[]()<>";
    GlyphRun reuse;
    size_t glyphs = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 20000; i++) {
        ShapeRun(line, fonts, 14.0f, false, reuse);
        glyphs += reuse.glyphs.size();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::printf("  ShapeRun: %zu glyphs in %.1f ms = %.0f glyphs/ms\n", glyphs, ms, glyphs / ms);
}
#endif

RGUI_TEST("text.font_index_on_scanned") {
    // The callback runs once the scan has ended (on its thread), or at once after it.
    const std::string empty = std::string(rgui::test::BuildDir()) + "/test-no-fonts";
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::u8path(empty), error);
    FontIndex index;
    index.SetDirectories({empty});
    std::atomic<int> calls{0};
    index.OnScanned([&calls] { calls++; });
    CHECK_EQ(calls.load(), 0);
    index.Start();
    index.Wait();
    for (int i = 0; i < 1000 && calls.load() == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK_EQ(calls.load(), 1);
    index.OnScanned([&calls] { calls++; });
    CHECK_EQ(calls.load(), 2);
}

RGUI_TEST("text.fallback_known_files") {
#ifdef _WIN32
    // The default fallbacks open from their known files: with an index of a folder of no fonts,
    // a code point the interface font lacks still finds its face, without the index's scan.
    if (!MappedFile().Open(FontIndex::DefaultDirectories().front() + "\\msyh.ttc")) {
        std::printf("skip text.fallback_known_files: no msyh.ttc\n");
        return;
    }
    const std::string empty = std::string(rgui::test::BuildDir()) + "\\test-no-fonts";
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::u8path(empty), error);
    FontIndex index;
    index.SetDirectories({empty});
    FontSet fonts;
    CHECK(fonts.LoadKnown(index, KnownInterfaceFontFiles(), DefaultFallbackFamilies()));
    const FontSet::Resolved cjk = fonts.Resolve(0x65E5, false);   // 日, from Microsoft YaHei UI
    CHECK(cjk.glyph != 0);
    CHECK(cjk.face != &fonts.Face(false));
    CHECK(!index.Scanned());
#else
    std::printf("skip text.fallback_known_files: Windows only\n");
#endif
}

RGUI_TEST("text.fuzz_glyf") {
    FuzzFont(kSans, "text.fuzz_glyf", 3000);
}

RGUI_TEST("text.fuzz_cff") {
    FuzzFont(kCff, "text.fuzz_cff", 3000);
}

RGUI_TEST("text.fuzz_bare_cff") {
    const std::vector<uint8_t> table = TableBytes(ReadBytes(kCff), "CFF ");
    if (table.empty()) {
        std::printf("skip text.fuzz_bare_cff: %s not found\n", kCff != nullptr ? kCff : "a CFF font");
        return;
    }
    std::mt19937 rng(777);
    uint32_t checksum = 0;
    std::vector<uint8_t> bytes;
    gfx::Path path;
    for (int i = 0; i < 2000; i++) {
        bytes = table;
        // Mostly the first 8 KB (header, INDEXes, Top DICT, charset), sometimes anywhere.
        const size_t span = rng() % 4 == 0 ? bytes.size() : std::min<size_t>(bytes.size(), 8192);
        for (int k = 0; k < 2; k++) bytes[rng() % span] = static_cast<uint8_t>(rng());
        const size_t length = rng() % 8 == 0 ? rng() % bytes.size() : bytes.size();
        CffOutlines cff;
        if (!cff.Open(bytes.data(), length)) continue;
        for (int code = 0; code < 256; code += 7) checksum += cff.Encoding().GlyphFor(static_cast<uint8_t>(code));
        for (uint32_t g = 0; g < 16; g++) {
            const uint16_t glyph = static_cast<uint16_t>((g * 2654435761u + static_cast<uint32_t>(i)) % cff.GlyphCount());
            double advance = 0;
            path.Clear();
            if (cff.Outline(glyph, path, cff.GlyphMatrix(glyph), &advance)) checksum += static_cast<uint32_t>(path.Verbs().size());
            checksum += cff.Charset().IdOf(glyph);
        }
    }
    std::printf("  text.fuzz_bare_cff: 2000 mutations (checksum %u)\n", checksum);
}
