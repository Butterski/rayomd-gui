// SPDX-License-Identifier: Apache-2.0
#include "runner.h"

#include "common/file_io.h"
#include "pdf/content.h"
#include "pdf/reader.h"
#include "rayomd/tiny_pdf.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using rgui::pdf::ContentLexer;
using rgui::pdf::PdfFile;
using rgui::pdf::PdfRect;
using rgui::pdf::PdfValue;

double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string TesterPath() {
    return std::string(rgui::test::SourceDir()) + "/external/rayomd/tester.md";
}

// The preview's PDF of `markdown`: built as the app builds previews, uncompressed.
std::string Build(const std::string& markdown, TinyPdf::PdfOptions options = {}) {
    options.compress = false;
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(markdown, options, pdf);
    CHECK(result.Ok());
    return pdf;
}

std::string BuildTester() {
    std::string markdown;
    CHECK(RayoMd::File::ReadAll(TesterPath(), 1u << 24, markdown));
    TinyPdf::PdfOptions options;
    options.sourcePath = TesterPath();
    return Build(markdown, options);
}

// A stream object's body with a correct /Length, or `length` when given.
std::string Stream(const std::string& data, const std::string& extra = "", long length = -1) {
    return "<< /Length " + std::to_string(length >= 0 ? length : static_cast<long>(data.size())) + extra +
           " >>\nstream\n" + data + "\nendstream";
}

// Objects 1..n with these bodies, a classic cross-reference table and a trailer.
std::string MakePdf(const std::vector<std::string>& bodies, const std::string& trailer = "/Root 1 0 R") {
    std::string out = "%PDF-1.7\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < bodies.size(); i++) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(bodies.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t offset : offsets) {
        char entry[32];
        std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", offset);
        out += entry;
    }
    out += "trailer\n<< /Size " + std::to_string(bodies.size() + 1) + " " + trailer + " >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return out;
}

std::string StartXref(const std::string& pdf) {
    const size_t at = pdf.rfind("startxref\n");
    return pdf.substr(at + 10, pdf.find('\n', at + 10) - at - 10);
}

std::string Content(const PdfFile& file, int page) {
    std::string_view content;
    std::string error;
    CHECK(file.PageContent(page, content, &error));
    return std::string(content);
}

struct Op {
    std::string op;
    std::vector<PdfValue> operands;
    std::string inlineData;
};

// The lexer's operands live until its next operator: the tests keep copies in an arena of their
// own, kept for the whole run.
rgui::pdf::PdfArena& KeptValues() {
    static rgui::pdf::PdfArena arena;
    return arena;
}

PdfValue Keep(const PdfValue& value) {
    PdfValue copy = value;
    rgui::pdf::PdfArena& arena = KeptValues();
    if (value.IsString() || value.IsName()) {
        char* bytes = static_cast<char*>(arena.Allocate(value.count + 1));
        std::memcpy(bytes, value.Text().data(), value.count);
        copy.data = bytes;
    } else if (value.IsArray()) {
        PdfValue* items = static_cast<PdfValue*>(arena.Allocate(sizeof(PdfValue) * (value.count + 1)));
        for (size_t i = 0; i < value.count; i++) items[i] = Keep(value.Array()[i]);
        copy.data = items;
    } else if (value.IsDict()) {
        auto* entries = static_cast<rgui::pdf::PdfDictEntry*>(arena.Allocate(sizeof(rgui::pdf::PdfDictEntry) * (value.count + 1)));
        for (size_t i = 0; i < value.count; i++) {
            const rgui::pdf::PdfDictEntry& entry = value.Entries()[i];
            char* key = static_cast<char*>(arena.Allocate(entry.key.size() + 1));
            std::memcpy(key, entry.key.data(), entry.key.size());
            entries[i].key = std::string_view(key, entry.key.size());
            entries[i].value = Keep(entry.value);
        }
        copy.data = entries;
    }
    return copy;
}

std::vector<Op> Lex(std::string_view content) {
    std::vector<Op> ops;
    ContentLexer lexer(content);
    std::string_view op;
    std::vector<PdfValue> operands;
    while (lexer.Next(op, operands)) {
        Op kept{std::string(op), {}, std::string(lexer.InlineImageData())};
        for (const PdfValue& value : operands) kept.operands.push_back(Keep(value));
        ops.push_back(std::move(kept));
    }
    return ops;
}

} // namespace

RGUI_TEST("pdf.page_sizes") {
    struct Case {
        TinyPdf::PdfPageSize size;
        double width;
        double height;
    };
    const Case cases[] = {
        {TinyPdf::PdfPageSize::A4(), 595, 842},
        {{792, 612}, 792, 612},  // Letter landscape, as the app swaps it
        {{300, 500}, 300, 500},
    };
    for (const Case& c : cases) {
        TinyPdf::PdfOptions options;
        options.pageSize = c.size;
        std::string error;
        const auto file = PdfFile::Parse(Build("# Size\n\nText.\n", options), &error);
        CHECK(file);
        if (!file) continue;
        CHECK_EQ(file->PageCount(), 1);
        const PdfRect box = file->PageMediaBox(0);
        CHECK_EQ(box.x0, 0.0);
        CHECK_EQ(box.y0, 0.0);
        CHECK_EQ(box.x1, c.width);
        CHECK_EQ(box.y1, c.height);
    }
    const auto twoPages = PdfFile::Parse(Build("# One\n\nFirst.\n\n\\pagebreak\n\nSecond.\n\n\\newpage\n\nThird.\n"), nullptr);
    CHECK(twoPages && twoPages->PageCount() == 3);
    if (twoPages) {
        CHECK(Content(*twoPages, 1).find("(Second.) Tj") != std::string::npos);
        CHECK(Content(*twoPages, 2).find("(Third.) Tj") != std::string::npos);
    }
}

RGUI_TEST("pdf.page_key_stable") {
    const std::string first = BuildTester();
    const std::string second = BuildTester();
    const auto a = PdfFile::Parse(first, nullptr);
    const auto b = PdfFile::Parse(second, nullptr);
    CHECK(a && b);
    if (!a || !b) return;
    CHECK(a->PageCount() > 2);
    CHECK_EQ(a->PageCount(), b->PageCount());
    std::vector<uint64_t> keys;
    for (int i = 0; i < a->PageCount(); i++) {
        CHECK(a->PageKey(i) == b->PageKey(i));
        CHECK(a->PageKey(i) == a->PageKey(i));
        keys.push_back(a->PageKey(i).lo);
    }
    std::sort(keys.begin(), keys.end());
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());

    // Timings (best of 20): Parse (the copy of the bytes, the cross-reference table and the page
    // tree), every object parsed, PageKey on a fresh file (hashing each resource object once)
    // and again (memoized), every page's content lexed.
    constexpr int kRuns = 20;
    double parse = 1e9;
    double objects = 1e9;
    double keyFirst = 1e9;
    double keyAgain = 1e9;
    double lex = 1e9;
    for (int run = 0; run < kRuns; run++) {
        auto start = std::chrono::steady_clock::now();
        auto file = PdfFile::Parse(first, nullptr);
        parse = std::min(parse, MsSince(start));
        start = std::chrono::steady_clock::now();
        for (int i = 0; i < file->PageCount(); i++) file->PageKey(i);
        keyFirst = std::min(keyFirst, MsSince(start) / file->PageCount());
        start = std::chrono::steady_clock::now();
        for (int i = 0; i < file->PageCount(); i++) file->PageKey(i);
        keyAgain = std::min(keyAgain, MsSince(start) / file->PageCount());
        start = std::chrono::steady_clock::now();
        size_t count = 0;
        std::string_view op;
        std::vector<PdfValue> operands;
        for (int i = 0; i < file->PageCount(); i++) {
            std::string_view content;
            CHECK(file->PageContent(i, content, nullptr));
            ContentLexer lexer(content);
            while (lexer.Next(op, operands)) count++;
        }
        lex = std::min(lex, MsSince(start));
        CHECK(count > 1000);
        file = PdfFile::Parse(first, nullptr);
        start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < file->ObjectCount(); i++) file->Object(i);
        objects = std::min(objects, MsSince(start));
    }
    std::printf("tester.md PDF: %zu bytes, %d pages, %u objects; parse %.3f ms, all objects %.3f ms; PageKey %.3f ms/page "
                "first, %.4f ms/page memoized; lexing all content %.3f ms\n",
                first.size(), a->PageCount(), a->ObjectCount(), parse, objects, keyFirst, keyAgain, lex);
}

RGUI_TEST("pdf.page_key_edit") {
    // Page 1 has a CID font (ż), a math font and an image-free body; page 2 is edited without new
    // glyphs: once with its words swapped, once with a link, whose annotation object comes
    // before the math fonts and renumbers them.
    const std::string head = "# Keys\n\nPage one: żółw and $x^2 + y$.\n\n\\pagebreak\n\n";
    const auto base = PdfFile::Parse(Build(head + "Page two text abc.\n"), nullptr);
    const auto swapped = PdfFile::Parse(Build(head + "Page two text bca.\n"), nullptr);
    const auto linked = PdfFile::Parse(Build(head + "Page two text [abc](https://example.com).\n"), nullptr);
    CHECK(base && swapped && linked);
    if (!base || !swapped || !linked) return;
    CHECK_EQ(base->PageCount(), 2);
    CHECK_EQ(swapped->PageCount(), 2);
    CHECK_EQ(linked->PageCount(), 2);
    CHECK_EQ(base->PageKey(0), swapped->PageKey(0));
    CHECK_EQ(base->PageKey(0), linked->PageKey(0));
    CHECK(base->PageKey(1) != swapped->PageKey(1));
    CHECK(base->PageKey(1) != linked->PageKey(1));
    CHECK(swapped->PageKey(1) != linked->PageKey(1));

    // The link did shift page 1's resource objects: its key holds no object number.
    auto fontNumbers = [](const PdfFile& file) {
        std::vector<uint32_t> numbers;
        for (const auto& entry : file.Get(file.PageResources(0), "Font").Entries()) numbers.push_back(entry.value.refNum);
        return numbers;
    };
    CHECK(!fontNumbers(*base).empty());
    CHECK(fontNumbers(*base) != fontNumbers(*linked));
}

RGUI_TEST("pdf.reader_objects") {
    const std::vector<std::string> bodies = {
        /* 1 */ "<< /Type /Catalog /Pages 2 0 R >>",
        // The tree names itself among its kids: read once.
        /* 2 */ "<< /Type /Pages /Kids [3 0 R 4 0 R 2 0 R 3 0 R] /Count 2 /MediaBox [0 0 200 100] "
                "/Resources << /Font << /F1 9 0 R >> >> >>",
        /* 3 */ "<< /Type /Page /Parent 2 0 R /Contents [5 0 R 6 0 R] >>",
        /* 4 */ "<< /Type /Page /Parent 2 0 R /MediaBox [300 400 0 0] /Contents 7 0 R /Resources 10 0 R >>",
        /* 5 */ "<< /Length 8 0 R >>\nstream\nq 1 0 0 1 0 0 cm\nendstream",
        /* 6 */ Stream("(a\\)b) Tj", "", 999),  // a wrong /Length
        /* 7 */ Stream("xxxxx", " /Filter [/FlateDecode] /DecodeParms << /Predictor 15 >>"),
        /* 8 */ "16",
        /* 9 */ "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Loop 11 0 R >>",
        /* 10 */ "<< /Font << /F1 9 0 R >> /Self 10 0 R >>",
        /* 11 */ "12 0 R",
        /* 12 */ "11 0 R",
        /* 13 */ std::string(65, '[') + std::string(65, ']'),
        /* 14 */ std::string(64, '[') + std::string(64, ']'),
        /* 15 */ "<< /S (a\\(b\\)c\\101\\0612\\\n x\\q) /H <48 65 6C6C 6F7> /N /A#20B#23 /R -.5 /I -12 "
                 "/Big 123456789012345678901234 /T true /Z null /Ref 9 0 R >>",
    };
    const std::string pdf = MakePdf(bodies);
    std::string error;
    const auto file = PdfFile::Parse(pdf, &error);
    CHECK(file);
    if (!file) return;
    CHECK_EQ(file->PageCount(), 2);
    CHECK_EQ(file->ObjectCount(), 16u);

    const PdfRect inherited = file->PageMediaBox(0);
    CHECK(inherited.x0 == 0 && inherited.y0 == 0 && inherited.x1 == 200 && inherited.y1 == 100);
    const PdfRect normalized = file->PageMediaBox(1);
    CHECK(normalized.x0 == 0 && normalized.y0 == 0 && normalized.x1 == 300 && normalized.y1 == 400);
    const PdfValue& font = file->Get(file->Get(file->PageResources(0), "Font"), "F1");
    CHECK(file->Get(font, "BaseFont").IsName("Helvetica"));
    CHECK(file->Get(file->PageResources(1), "Font").IsDict());

    // /Length by reference; a wrong /Length found by "endstream"; joined with a newline.
    CHECK_EQ(Content(*file, 0), std::string("q 1 0 0 1 0 0 cm\n(a\\)b) Tj"));
    std::string_view data;
    CHECK(!file->PageContent(1, data, &error));
    CHECK_EQ(error, std::string("filter:FlateDecode"));
    error.clear();
    CHECK(!file->StreamData(file->Object(1), data, &error));
    CHECK_EQ(error, std::string("stream"));
    rgui::pdf::PdfRawStream raw;
    CHECK(file->RawStream(file->Object(7), raw));
    CHECK_EQ(raw.data, std::string_view("xxxxx"));
    CHECK(raw.filter && raw.filter->IsArray() && raw.filter->Array()[0].IsName("FlateDecode"));
    CHECK(raw.decodeParms && raw.decodeParms->Find("Predictor")->Int() == 15);

    // References: a chain to a cycle resolves to null; out-of-range objects are null.
    CHECK(file->Object(11).IsRef());
    CHECK(file->Resolve(file->Object(11)).IsNull());
    CHECK(file->Object(0).IsNull());
    CHECK(file->Object(99).IsNull());
    CHECK(file->Object(13).IsNull());  // nested 65 deep
    CHECK(file->Object(14).IsArray());

    const PdfValue& values = file->Object(15);
    CHECK_EQ(file->Get(values, "S").String(), std::string_view("a(b)cA12 xq"));
    CHECK_EQ(file->Get(values, "H").String(), std::string_view("Hello\x70", 6));
    CHECK_EQ(file->Get(values, "N").Name(), std::string_view("A B#"));
    CHECK_EQ(file->Get(values, "R").Number(), -0.5);
    CHECK_EQ(file->Get(values, "I").Int(), int64_t{-12});
    CHECK(file->Get(values, "Big").type == PdfValue::Type::Real);
    CHECK_NEAR(file->Get(values, "Big").Number(), 1.2345678901234568e23, 1e9);
    CHECK(file->Get(values, "T").Bool());
    CHECK(file->Get(values, "Z").IsNull());
    CHECK(file->Get(values, "Ref").IsDict());
    double number = 0;
    CHECK(file->Number(file->Object(8), number) && number == 16);

    // Keys: the cycles in the resources end; a second parse gives the same keys.
    const auto again = PdfFile::Parse(pdf, nullptr);
    CHECK_EQ(file->PageKey(0), again->PageKey(0));
    CHECK_EQ(file->PageKey(1), again->PageKey(1));
    CHECK(file->PageKey(0) != file->PageKey(1));
    CHECK_EQ(file->ValueHash(file->Object(10)), again->ValueHash(again->Object(10)));
}

RGUI_TEST("pdf.reader_incremental") {
    // The first trailer's /Prev, out of the file for now, is set to the update below to close a
    // loop.
    std::string pdf = MakePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R >>",
        Stream("(old) Tj"),
    }, "/Root 1 0 R /Prev 999999999");
    const std::string firstXref = StartXref(pdf);
    // An update that replaces object 4 and points back with /Prev.
    const size_t object = pdf.size();
    pdf += "4 0 obj\n" + Stream("(new) Tj") + "\nendobj\n";
    const size_t xref = pdf.size();
    char entry[32];
    std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", object);
    pdf += "xref\n4 1\n" + std::string(entry) + "trailer\n<< /Size 5 /Root 1 0 R /Prev " + firstXref +
           " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    const auto file = PdfFile::Parse(pdf, nullptr);
    CHECK(file);
    if (file) CHECK_EQ(Content(*file, 0), std::string("(new) Tj"));

    // The older section's /Prev points back at the update: the chain ends where it began.
    char padded[32];
    std::snprintf(padded, sizeof padded, "%09zu", xref);
    std::string looped = pdf;
    looped.replace(looped.find("999999999"), 9, padded);
    const auto loopedFile = PdfFile::Parse(looped, nullptr);
    CHECK(loopedFile);
    if (loopedFile) CHECK_EQ(Content(*loopedFile, 0), std::string("(new) Tj"));
}

RGUI_TEST("pdf.reader_errors") {
    std::string error;
    CHECK(!PdfFile::Parse("", &error));
    CHECK_EQ(error, std::string("startxref"));
    CHECK(!PdfFile::Parse("%PDF-1.7\nstartxref\n3\n%%EOF\n", &error));
    CHECK_EQ(error, std::string("xref"));
    CHECK(!PdfFile::Parse(MakePdf({"<< /Type /Catalog >>"}, "/Root 5 0 R"), &error));
    CHECK_EQ(error, std::string("root"));
    CHECK(!PdfFile::Parse(MakePdf({"<< /Type /Catalog >>"}), &error));
    CHECK_EQ(error, std::string("pages"));
    CHECK(!PdfFile::Parse(MakePdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [] >>"}), &error));
    CHECK_EQ(error, std::string("no pages"));
    // An xref entry that points into the middle of another object reads as null.
    std::string shifted = MakePdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] >>",
                                   "<< /Type /Page /Contents 4 0 R >>", Stream("(x) Tj")});
    const size_t entry = shifted.find(" 00000 n ", shifted.find("xref")) - 10;
    char moved[32];
    std::snprintf(moved, sizeof moved, "%010zu", shifted.find("1 0 obj") + 1);  // object 1, one byte on
    shifted.replace(entry, 10, moved);
    CHECK(!PdfFile::Parse(shifted, &error));
    CHECK_EQ(error, std::string("root"));
}

RGUI_TEST("pdf.lexer") {
    const std::string image("a EI b\x00\xff", 8);
    const std::string content =
        "q 1 0 0 1 72.5 -.25 cm % a comment ) [\n"
        "/F1 12 Tf [(A\\)\\(b) -120 <4142> (\\101\\n\\\\) <41 4>] TJ\n"
        "/Name#20X#2f gs (nested (parens) ok) Tj\n"
        "/Span << /MCID 3 /Alt (x) >> BDC EMC true false null d0\n"
        "BI /W 4 /H 2 /BPC 8 /CS /G ID " + image + "\nEI Q\n"
        "BI /W 1 /H 1 /F /AHx /CS /RGB /BPC 8 ID 41>EI1 EI\n"
        "] ) > } 5 x\n"
        "1 2 (open";
    const std::vector<Op> ops = Lex(content);
    const char* expected[] = {"q", "cm", "Tf", "TJ", "gs", "Tj", "BDC", "EMC", "d0", "BI", "Q", "BI", "x"};
    CHECK_EQ(ops.size(), sizeof expected / sizeof expected[0]);
    if (ops.size() != sizeof expected / sizeof expected[0]) return;
    for (size_t i = 0; i < ops.size(); i++) CHECK_EQ(ops[i].op, std::string(expected[i]));

    CHECK(ops[0].operands.empty());
    CHECK_EQ(ops[1].operands.size(), size_t{6});
    CHECK_EQ(ops[1].operands[4].Number(), 72.5);
    CHECK_EQ(ops[1].operands[5].Number(), -0.25);
    CHECK(ops[1].operands[0].type == PdfValue::Type::Int);
    CHECK(ops[2].operands[0].IsName("F1") && ops[2].operands[1].Int() == 12);
    const auto tj = ops[3].operands[0].Array();
    CHECK_EQ(tj.size(), size_t{5});
    if (tj.size() == 5) {
        CHECK_EQ(tj[0].String(), std::string_view("A)(b"));
        CHECK_EQ(tj[1].Int(), int64_t{-120});
        CHECK_EQ(tj[2].String(), std::string_view("AB"));
        CHECK_EQ(tj[3].String(), std::string_view("A\n\\"));
        CHECK_EQ(tj[4].String(), std::string_view("A@"));
    }
    CHECK_EQ(ops[4].operands[0].Name(), std::string_view("Name X/"));
    CHECK_EQ(ops[5].operands[0].String(), std::string_view("nested (parens) ok"));
    CHECK(ops[6].operands.size() == 2 && ops[6].operands[0].IsName("Span"));
    CHECK(ops[6].operands[1].Find("MCID") && ops[6].operands[1].Find("MCID")->Int() == 3);
    CHECK(ops[7].operands.empty());
    CHECK(ops[8].operands.size() == 3 && ops[8].operands[0].Bool() && !ops[8].operands[1].Bool(true) &&
          ops[8].operands[2].IsNull());
    // The image's size (4 x 2 gray bytes) finds its end past the " EI " inside the data.
    CHECK_EQ(ops[9].inlineData, image);
    CHECK(ops[9].operands.size() == 1 && ops[9].operands[0].Find("W")->Int() == 4);
    CHECK(ops[9].operands[0].Find("CS")->IsName("G"));
    // A filtered image ends at the first "EI" between white space.
    CHECK_EQ(ops[11].inlineData, std::string("41>EI1"));
    CHECK(ops[11].operands[0].Find("F")->IsName("AHx"));
    CHECK(ops[12].operands.size() == 1 && ops[12].operands[0].Int() == 5);
    CHECK(ops[10].inlineData.empty());

    // Malformed input: a broken hex string drops its operands; an unterminated inline image
    // takes the rest.
    const std::vector<Op> broken = Lex("1 2 <zz> 3 Td BI /W 2 ID xyz");
    CHECK(!broken.empty() && broken.back().op == "BI" && broken.back().inlineData == "xyz");
    CHECK(broken.size() >= 2 && broken[broken.size() - 2].op == "Td");
    CHECK(broken.size() >= 2 && broken[broken.size() - 2].operands.size() == 1);
    // Too many operands: only the last kMaxOperands stay.
    std::string many;
    for (size_t i = 0; i < ContentLexer::kMaxOperands + 3; i++) many += "1 ";
    const std::vector<Op> crowded = Lex(many + "op");
    CHECK(crowded.size() == 1 && crowded[0].operands.size() == 3);
}

namespace {

// Everything the renderer does with a file: every object, every page's content lexed, keys,
// resources and their streams. Returns a digest so the work cannot be skipped.
uint64_t Exercise(const PdfFile& file) {
    uint64_t digest = 0;
    for (uint32_t i = 0; i < file.ObjectCount(); i++) digest += static_cast<uint64_t>(file.Object(i).type);
    for (int page = 0; page < file.PageCount(); page++) {
        const PdfRect box = file.PageMediaBox(page);
        digest += static_cast<uint64_t>(box.Width() > 0);
        std::string_view content;
        if (file.PageContent(page, content, nullptr)) {
            ContentLexer lexer(content);
            std::string_view op;
            std::vector<PdfValue> operands;
            while (lexer.Next(op, operands)) digest += op.size() + operands.size();
        }
        digest ^= file.PageKey(page).lo ^ file.PageKey(page).hi;
        const PdfValue& resources = file.PageResources(page);
        for (const char* category : {"Font", "XObject", "ExtGState", "Shading"}) {
            for (const auto& entry : file.Get(resources, category).Entries()) {
                const PdfValue& object = file.Resolve(entry.value);
                rgui::pdf::PdfRawStream raw;
                if (file.RawStream(object, raw)) digest += raw.data.size();
                if (file.RawStream(file.Get(object, "SMask"), raw)) digest += raw.data.size();
                const PdfValue& descendants = file.Get(object, "DescendantFonts");
                const PdfValue& cidFont = descendants.IsArray() && !descendants.Array().empty()
                                              ? file.Resolve(descendants.Array()[0])
                                              : object;
                const PdfValue& descriptor = file.Get(cidFont, "FontDescriptor");
                for (const char* program : {"FontFile", "FontFile2", "FontFile3"}) {
                    std::string_view data;
                    if (file.StreamData(file.Get(descriptor, program), data, nullptr)) digest += data.size();
                }
                PdfRect rect;
                if (file.Rect(file.Get(descriptor, "FontBBox"), rect)) digest += 1;
            }
        }
    }
    return digest;
}

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

// Every position where `needle` occurs in `haystack`.
std::vector<size_t> FindAll(const std::string& haystack, std::string_view needle) {
    std::vector<size_t> found;
    for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) found.push_back(at);
    return found;
}

} // namespace

RGUI_TEST("pdf.hardening") {
    const std::string pdf = BuildTester();
    const auto original = PdfFile::Parse(pdf, nullptr);
    CHECK(original);
    if (!original) return;
    const uint64_t baseline = Exercise(*original);
    CHECK(baseline != 0);

    // Positions by what they hit: the cross-reference table and trailer, dictionaries (object
    // headers), strings, stream keywords and lengths, and anywhere.
    const size_t xref = pdf.rfind("xref\n");
    std::vector<size_t> dicts;
    for (size_t at : FindAll(pdf, " 0 obj\n")) {
        const size_t end = std::min(pdf.find("stream\n", at), pdf.find("endobj", at));
        for (size_t i = at; i < end && i < at + 400; i++) dicts.push_back(i);
    }
    std::vector<size_t> strings;
    for (size_t at : FindAll(pdf, "(")) {
        for (size_t i = at; i < at + 6 && i < pdf.size(); i++) strings.push_back(i);
    }
    for (size_t at : FindAll(pdf, ")")) strings.push_back(at);
    std::vector<size_t> streams;
    for (std::string_view keyword : {"stream\n", "endstream", "/Length ", "/Filter", "/Contents"}) {
        for (size_t at : FindAll(pdf, keyword)) {
            for (size_t i = at > 4 ? at - 4 : 0; i < at + keyword.size() + 8 && i < pdf.size(); i++) streams.push_back(i);
        }
    }
    CHECK(xref != std::string::npos && !dicts.empty() && !strings.empty() && !streams.empty());

    // The object each byte belongs to (an entry of the cross-reference table: the object it
    // locates), and the byte ranges of the pages' content streams.
    std::vector<std::pair<size_t, uint32_t>> objectStarts;
    for (size_t at : FindAll(pdf, " 0 obj\n")) {
        size_t begin = at;
        uint32_t num = 0;
        while (begin > 0 && pdf[begin - 1] >= '0' && pdf[begin - 1] <= '9') begin--;
        for (size_t i = begin; i < at; i++) num = num * 10 + static_cast<uint32_t>(pdf[i] - '0');
        objectStarts.push_back({begin, num});
    }
    const size_t xrefEntries = pdf.find('\n', pdf.find('\n', xref) + 1) + 1;  // after "xref\n0 N\n"
    const size_t trailer = pdf.find("trailer", xref);
    auto objectAt = [&](size_t at) {
        if (at >= xrefEntries && at < trailer) return static_cast<uint32_t>((at - xrefEntries) / 20);
        uint32_t num = 0;
        for (const auto& start : objectStarts) {
            if (start.first <= at) num = start.second;
        }
        return num;
    };
    std::vector<std::pair<size_t, size_t>> contentRanges;
    for (int page = 0; page < original->PageCount(); page++) {
        std::string_view content;
        CHECK(original->PageContent(page, content, nullptr));
        const size_t begin = static_cast<size_t>(content.data() - original->Bytes().data());
        contentRanges.push_back({begin, begin + content.size()});
    }

    static const char kBytes[] = "()<>[]{}/%\\ \n\r0123456789.-+RnfeEBIDTj#";
    Random random{0x5eed5eedull};
    double slowest = 0;
    int parsed = 0;
    // Every 20th file, and every one with a changed trailer, goes through all the renderer does
    // (Exercise: every object, page key and resource stream); the others through what the
    // changed byte reaches directly: its object, that object's hash unless it is a stream, and
    // the lexed content of the page whose content holds it. Exercising everything for every
    // mutation would take minutes under the sanitizers.
    auto run = [&](std::string bytes, size_t at, bool full) {
        const auto start = std::chrono::steady_clock::now();
        const auto file = PdfFile::Parse(std::move(bytes), nullptr);
        if (file) {
            parsed++;
            if (full) {
                Exercise(*file);
            } else {
                const PdfValue& object = file->Object(objectAt(at));
                rgui::pdf::PdfRawStream raw;
                file->RawStream(object, raw);
                if (!object.IsStream()) file->ValueHash(object);  // stream data: hashed in full runs
                for (int page = 0; page < file->PageCount() && page < static_cast<int>(contentRanges.size()); page++) {
                    if (at < contentRanges[page].first || at >= contentRanges[page].second) continue;
                    std::string_view content;
                    if (file->PageContent(page, content, nullptr)) {
                        ContentLexer lexer(content);
                        std::string_view op;
                        std::vector<PdfValue> operands;
                        while (lexer.Next(op, operands)) {}
                    }
                }
            }
        }
        slowest = std::max(slowest, MsSince(start));
    };
    constexpr int kMutations = 10000;
    for (int i = 0; i < kMutations; i++) {
        size_t at = 0;
        switch (i % 5) {
            case 0: at = xref + random.Below(pdf.size() - xref); break;
            case 1: at = dicts[random.Below(dicts.size())]; break;
            case 2: at = strings[random.Below(strings.size())]; break;
            case 3: at = streams[random.Below(streams.size())]; break;
            default: at = random.Below(pdf.size()); break;
        }
        std::string mutated = pdf;
        mutated[at] = random.Below(4) == 0 ? static_cast<char>(random.Below(256))
                                           : kBytes[random.Below(sizeof kBytes - 1)];
        run(std::move(mutated), at, (i / 5) % 20 == 0 || at >= trailer);
    }
    // Truncations: anywhere, and within the tail that holds the table and trailer.
    constexpr int kTruncations = 400;
    for (int i = 0; i < kTruncations; i++) {
        const size_t length = i % 2 ? random.Below(pdf.size()) : pdf.size() - 1 - random.Below(std::min<size_t>(pdf.size(), 4000));
        run(pdf.substr(0, length), length > 0 ? length - 1 : 0, i % 8 < 2);
    }
    // Raw mutated content through the lexer: every byte value at each position class.
    std::string_view content;
    CHECK(original->PageContent(0, content, nullptr));
    for (int i = 0; i < 2000; i++) {
        std::string bytes(content.substr(0, std::min<size_t>(content.size(), 4096)));
        for (int k = 0; k < 4; k++) bytes[random.Below(bytes.size())] = static_cast<char>(random.Below(256));
        ContentLexer lexer(bytes);
        std::string_view op;
        std::vector<PdfValue> operands;
        size_t steps = 0;
        while (lexer.Next(op, operands)) steps++;
        CHECK(steps <= bytes.size());
    }
    std::printf("hardening: %d mutations, %d truncations, %d parsed; slowest %.2f ms\n", kMutations, kTruncations,
                parsed, slowest);
    // Linear work per file: none may take more than a generous bound (asan builds included).
    CHECK(slowest < 2000.0);
    CHECK(parsed > kMutations / 2);
}

RGUI_TEST("pdf.concurrent") {
    const std::string pdf = BuildTester();
    const auto reference = PdfFile::Parse(pdf, nullptr);
    CHECK(reference);
    if (!reference) return;
    const uint64_t expected = Exercise(*reference);
    std::vector<rgui::pdf::PdfHash> keys;
    for (int i = 0; i < reference->PageCount(); i++) keys.push_back(reference->PageKey(i));
    for (int round = 0; round < 8; round++) {
        const auto file = PdfFile::Parse(pdf, nullptr);
        std::vector<uint64_t> digests(4, 0);
        std::vector<uint8_t> keysMatch(4, 0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; t++) {
            threads.emplace_back([&, t] {
                // Each thread starts at another page, so first uses collide.
                bool match = true;
                for (int k = 0; k < file->PageCount(); k++) {
                    const int page = (k + t) % file->PageCount();
                    match = match && file->PageKey(page) == keys[static_cast<size_t>(page)];
                }
                keysMatch[static_cast<size_t>(t)] = match;
                digests[static_cast<size_t>(t)] = Exercise(*file);
            });
        }
        for (std::thread& thread : threads) thread.join();
        for (int t = 0; t < 4; t++) {
            CHECK_EQ(digests[static_cast<size_t>(t)], expected);
            CHECK(keysMatch[static_cast<size_t>(t)]);
        }
    }
}
