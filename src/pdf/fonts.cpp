// SPDX-License-Identifier: Apache-2.0
#include "pdf/fonts.h"

#include "common/text_utils.h"
#include "core/math_layout.h"
#include "text/cff_outlines.h"
#include "text/mapped_file.h"
#include "text/sfnt.h"

#include <algorithm>
#include <array>
#include <vector>

namespace rgui::pdf {
namespace {

class UnsupportedFont final : public PdfFont {
public:
    explicit UnsupportedFont(std::string what) : what_(std::move(what)) {}
    bool Next(std::string_view, size_t&, PdfGlyph&) const override { return false; }
    const text::GlyphSource* Source() const override { return nullptr; }
    std::string_view Unsupported() const override { return what_; }
    size_t Bytes() const override { return sizeof(*this) + what_.size(); }

private:
    std::string what_;
};

std::shared_ptr<const PdfFont> Unsupported(std::string what) {
    return std::make_shared<UnsupportedFont>(std::move(what));
}

// ---- standard fonts --------------------------------------------------------------------

// A system face mapped once for the life of the process (standard-font substitutes: a handful).
struct SystemFace {
    text::MappedFile file;
    text::FontFace face;
};

const text::FontFace* OpenSystemFace(const text::FontEntry& entry) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::unique_ptr<SystemFace>> faces;
    std::string key = entry.path;
    key.push_back('\0');
    key += std::to_string(entry.faceIndex);
    std::lock_guard<std::mutex> lock(mutex);
    auto found = faces.find(key);
    if (found == faces.end()) {
        auto face = std::make_unique<SystemFace>();
        if (!face->file.Open(entry.path) || !face->face.Open(face->file.Data(), face->file.Size(), entry.faceIndex)) {
            face.reset();
        }
        found = faces.emplace(std::move(key), std::move(face)).first;
    }
    return found->second ? &found->second->face : nullptr;
}

bool Contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// /FirstChar /Widths into widths[256] (codes it covers), true when present.
bool ReadSimpleWidths(const PdfFile& file, const PdfValue& dict, std::array<double, 256>& widths,
                      std::array<bool, 256>& has) {
    has.fill(false);
    const PdfValue& list = file.Get(dict, "Widths");
    if (!list.IsArray()) return false;
    const int64_t first = file.Get(dict, "FirstChar").Int(0);
    for (size_t i = 0; i < list.items.size(); i++) {
        const int64_t code = first + static_cast<int64_t>(i);
        if (code < 0 || code > 255) continue;
        double w = 0;
        if (!file.Number(list.items[i], w)) continue;
        widths[static_cast<size_t>(code)] = w;
        has[static_cast<size_t>(code)] = true;
    }
    return true;
}

class StandardFont final : public PdfFont {
public:
    StandardFont(const PdfFile& file, const PdfValue& dict, const text::FontFace& face,
                 TinyPdf::Internal::StandardTextFont metrics)
        : face_(face) {
        std::array<bool, 256> has{};
        ReadSimpleWidths(file, dict, widths_, has);
        for (int code = 0; code < 256; code++) {
            const char byte = static_cast<char>(code);
            if (!has[static_cast<size_t>(code)]) {
                widths_[static_cast<size_t>(code)] =
                    TinyPdf::Internal::StandardTextWidth(std::string_view(&byte, 1), 1000.0, metrics);
            }
            uint32_t cp = static_cast<uint32_t>(code);
            if (code >= 0x80) {
                const std::string utf8 = RayoMd::Text::WinAnsiToUtf8(std::string_view(&byte, 1));
                size_t length = 0;
                if (utf8.empty() || !RayoMd::Text::DecodeUtf8(utf8, 0, cp, length)) cp = 0;
            }
            glyphs_[static_cast<size_t>(code)] = cp ? face.GlyphForCodePoint(cp) : 0;
        }
    }

    bool Next(std::string_view bytes, size_t& at, PdfGlyph& out) const override {
        if (at >= bytes.size()) return false;
        const uint8_t code = static_cast<uint8_t>(bytes[at++]);
        out.glyph = glyphs_[code];
        out.width = widths_[code];
        out.wordSpace = code == 32;
        return true;
    }
    const text::GlyphSource* Source() const override { return &face_; }
    size_t Bytes() const override { return sizeof(*this); }

private:
    const text::FontFace& face_;
    std::array<uint16_t, 256> glyphs_{};
    std::array<double, 256> widths_{};
};

std::shared_ptr<const PdfFont> LoadStandard(const PdfFile& file, const PdfValue& dict, const text::FontIndex* index) {
    std::string_view name = file.Get(dict, "BaseFont").Name();
    if (name.size() > 7 && name[6] == '+') name.remove_prefix(7);  // a subset tag
    static const char* const kSans[] = {"Liberation Sans", "Arimo", "Nimbus Sans", "DejaVu Sans", "Arial"};
    static const char* const kMono[] = {"Liberation Mono", "Cousine", "Nimbus Mono PS", "DejaVu Sans Mono", "Courier New"};
    static const char* const kSerif[] = {"Liberation Serif", "Tinos", "Nimbus Roman", "DejaVu Serif", "Times New Roman"};
    const char* const* families = nullptr;
    std::string_view group;
    if (name.substr(0, 9) == "Helvetica" || name.substr(0, 5) == "Arial") {
        families = kSans;
        group = "Helvetica";
    } else if (name.substr(0, 7) == "Courier") {
        families = kMono;
        group = "Courier";
    } else if (name.substr(0, 5) == "Times") {
        families = kSerif;
        group = "Times";
    } else {
        return Unsupported("font:standard:" + std::string(name.empty() ? std::string_view("?") : name));
    }
    const bool bold = Contains(name, "Bold");
    const bool italic = Contains(name, "Oblique") || Contains(name, "Italic");
    const text::FontFace* face = nullptr;
    if (index) {
        for (int i = 0; i < 5 && !face; i++) {
            if (const text::FontEntry* entry = index->Find(families[i], bold ? 700 : 400, italic)) {
                face = OpenSystemFace(*entry);
            }
        }
    }
    if (!face) return Unsupported("font:no-substitute:" + std::string(group));
    using TinyPdf::Internal::StandardTextFont;
    const StandardTextFont metrics =
        group == "Courier" ? StandardTextFont::Mono : bold ? StandardTextFont::Bold : StandardTextFont::Regular;
    return std::make_shared<StandardFont>(file, dict, *face, metrics);
}

// ---- embedded programs -----------------------------------------------------------------

// A font program's bytes, copied out of the file. False, with `what` set, when the stream is
// missing or filtered.
bool ReadProgram(const PdfFile& file, const PdfValue& stream, std::string& out, std::string& what) {
    std::string_view data;
    std::string error;
    if (!file.StreamData(stream, data, &error)) {
        what = "font:" + (error.empty() ? std::string("program") : error);
        return false;
    }
    out.assign(data.data(), data.size());
    return true;
}

class Type1CFont final : public PdfFont {
public:
    bool Init(const PdfFile& file, const PdfValue& dict, const PdfValue& program, std::string& what) {
        if (!ReadProgram(file, program, program_, what)) return false;
        if (!cff_.Open(reinterpret_cast<const uint8_t*>(program_.data()), program_.size())) {
            what = "font:Type1C-unreadable";
            return false;
        }
        std::array<bool, 256> has{};
        ReadSimpleWidths(file, dict, widths_, has);
        double missing = 0;
        file.Number(file.Get(file.Get(dict, "FontDescriptor"), "MissingWidth"), missing);
        for (size_t code = 0; code < 256; code++) {
            if (!has[code]) widths_[code] = missing;
            glyphs_[code] = cff_.Encoding().GlyphFor(static_cast<uint8_t>(code));
        }
        return true;
    }

    bool Next(std::string_view bytes, size_t& at, PdfGlyph& out) const override {
        if (at >= bytes.size()) return false;
        const uint8_t code = static_cast<uint8_t>(bytes[at++]);
        out.glyph = glyphs_[code];
        out.width = widths_[code];
        out.wordSpace = code == 32;
        return true;
    }
    const text::GlyphSource* Source() const override { return &cff_; }
    size_t Bytes() const override { return sizeof(*this) + program_.size(); }

private:
    std::string program_;
    text::CffOutlines cff_;
    std::array<uint16_t, 256> glyphs_{};
    std::array<double, 256> widths_{};
};

struct WidthRun {
    uint32_t first = 0;
    uint32_t last = 0;
    double width = 0;
};

// /W of a CIDFont: "c [w1 w2 ...]" and "cfirst clast w" entries, at most 65,536 runs.
std::vector<WidthRun> ReadCidWidths(const PdfFile& file, const PdfValue& cidFont) {
    std::vector<WidthRun> runs;
    const PdfValue& list = file.Get(cidFont, "W");
    const auto& items = list.Array();
    constexpr size_t kMaxRuns = 65536;
    for (size_t i = 0; i < items.size() && runs.size() < kMaxRuns;) {
        const int64_t first = file.Resolve(items[i]).Int(-1);
        if (first < 0 || first > 0xFFFF || i + 1 >= items.size()) break;
        const PdfValue& next = file.Resolve(items[i + 1]);
        if (next.IsArray()) {
            for (size_t k = 0; k < next.items.size() && runs.size() < kMaxRuns; k++) {
                const uint32_t cid = static_cast<uint32_t>(first) + static_cast<uint32_t>(k);
                double w = 0;
                if (cid > 0xFFFF) break;
                if (file.Number(next.items[k], w)) runs.push_back({cid, cid, w});
            }
            i += 2;
        } else {
            if (i + 2 >= items.size()) break;
            const int64_t last = next.Int(-1);
            double w = 0;
            if (last >= first && last <= 0xFFFF && file.Number(items[i + 2], w)) {
                runs.push_back({static_cast<uint32_t>(first), static_cast<uint32_t>(last), w});
            }
            i += 3;
        }
    }
    std::stable_sort(runs.begin(), runs.end(), [](const WidthRun& a, const WidthRun& b) { return a.first < b.first; });
    return runs;
}

class CidFont final : public PdfFont {
public:
    // `program` is a FontFile2 (TrueType), or a FontFile3 of subtype CIDFontType0C (bare CFF) or
    // OpenType.
    bool Init(const PdfFile& file, const PdfValue& cidFont, const PdfValue& program, bool cff, std::string& what) {
        if (!ReadProgram(file, program, program_, what)) return false;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(program_.data());
        if (cff) {
            useCff_ = true;
            if (!cff_.Open(data, program_.size())) {
                what = "font:CIDFontType0C-unreadable";
                return false;
            }
        } else if (!face_.Open(data, program_.size())) {
            what = "font:FontFile2-unreadable";
            return false;
        }
        const PdfValue& map = file.Get(cidFont, "CIDToGIDMap");
        if (map.IsStream()) {
            if (!ReadProgram(file, map, cidToGid_, what)) return false;
            identity_ = false;
        }
        widths_ = ReadCidWidths(file, cidFont);
        file.Number(file.Get(cidFont, "DW"), defaultWidth_);
        return true;
    }

    bool Next(std::string_view bytes, size_t& at, PdfGlyph& out) const override {
        if (at + 2 > bytes.size()) {
            at = bytes.size();
            return false;
        }
        const uint32_t cid = (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at])) << 8) |
                             static_cast<uint8_t>(bytes[at + 1]);
        at += 2;
        if (useCff_) {
            out.glyph = cff_.IsCidKeyed() ? cff_.Charset().GlyphFor(static_cast<uint16_t>(cid)) : static_cast<uint16_t>(cid);
        } else if (identity_) {
            out.glyph = static_cast<uint16_t>(cid);
        } else {
            const size_t offset = static_cast<size_t>(cid) * 2;
            out.glyph = offset + 1 < cidToGid_.size()
                            ? static_cast<uint16_t>((static_cast<uint8_t>(cidToGid_[offset]) << 8) |
                                                    static_cast<uint8_t>(cidToGid_[offset + 1]))
                            : 0;
        }
        out.width = Width(cid);
        out.wordSpace = false;
        return true;
    }
    const text::GlyphSource* Source() const override {
        return useCff_ ? static_cast<const text::GlyphSource*>(&cff_) : &face_;
    }
    size_t Bytes() const override {
        return sizeof(*this) + program_.size() + cidToGid_.size() + widths_.size() * sizeof(WidthRun);
    }

private:
    // The run with the greatest start at or below cid, when it reaches cid; else /DW.
    double Width(uint32_t cid) const {
        auto it = std::upper_bound(widths_.begin(), widths_.end(), cid,
                                   [](uint32_t c, const WidthRun& run) { return c < run.first; });
        if (it == widths_.begin()) return defaultWidth_;
        --it;
        return cid <= it->last ? it->width : defaultWidth_;
    }

    std::string program_;
    std::string cidToGid_;
    bool identity_ = true;
    bool useCff_ = false;
    text::FontFace face_;
    text::CffOutlines cff_;
    std::vector<WidthRun> widths_;
    double defaultWidth_ = 1000;
};

std::string NameOr(std::string_view name, std::string_view fallback) {
    return std::string(name.empty() ? fallback : name);
}

std::shared_ptr<const PdfFont> LoadType0(const PdfFile& file, const PdfValue& dict) {
    const PdfValue& encoding = file.Get(dict, "Encoding");
    if (!encoding.IsName("Identity-H") && !encoding.IsName("Identity-V")) return Unsupported("font:Type0-encoding");
    const auto& descendants = file.Get(dict, "DescendantFonts").Array();
    if (descendants.empty()) return Unsupported("font:Type0");
    const PdfValue& cidFont = file.Resolve(descendants[0]);
    const std::string_view subtype = file.Get(cidFont, "Subtype").Name();
    const PdfValue& descriptor = file.Get(cidFont, "FontDescriptor");
    auto font = std::make_shared<CidFont>();
    std::string what;
    if (subtype == "CIDFontType2") {
        const PdfValue& program = file.Get(descriptor, "FontFile2");
        if (!program.IsStream()) return Unsupported("font:CIDFontType2-not-embedded");
        if (!font->Init(file, cidFont, program, false, what)) return Unsupported(what);
        return font;
    }
    if (subtype == "CIDFontType0") {
        const PdfValue& program = file.Get(descriptor, "FontFile3");
        const std::string_view kind = file.Get(program, "Subtype").Name();
        if (!program.IsStream() || (kind != "CIDFontType0C" && kind != "OpenType")) {
            return Unsupported("font:CIDFontType0-" + NameOr(kind, "not-embedded"));
        }
        if (!font->Init(file, cidFont, program, kind == "CIDFontType0C", what)) return Unsupported(what);
        return font;
    }
    return Unsupported("font:" + NameOr(subtype, "Type0"));
}

std::shared_ptr<const PdfFont> LoadType1(const PdfFile& file, const PdfValue& dict, const text::FontIndex* index) {
    const PdfValue& descriptor = file.Get(dict, "FontDescriptor");
    const PdfValue& program = file.Get(descriptor, "FontFile3");
    if (program.IsStream()) {
        const std::string_view kind = file.Get(program, "Subtype").Name();
        if (kind != "Type1C") return Unsupported("font:Type1-" + NameOr(kind, "FontFile3"));
        auto font = std::make_shared<Type1CFont>();
        std::string what;
        if (!font->Init(file, dict, program, what)) return Unsupported(what);
        return font;
    }
    if (!file.Get(descriptor, "FontFile").IsNull() || !file.Get(descriptor, "FontFile2").IsNull()) {
        return Unsupported("font:Type1-embedded");
    }
    return LoadStandard(file, dict, index);
}

} // namespace

std::shared_ptr<const PdfFont> LoadFont(const PdfFile& file, const PdfValue& font, const text::FontIndex* index) {
    const PdfValue& dict = file.Resolve(font);
    if (!dict.IsDict()) return Unsupported("font:missing");
    const std::string_view subtype = file.Get(dict, "Subtype").Name();
    if (subtype == "Type0") return LoadType0(file, dict);
    if (subtype == "Type1") return LoadType1(file, dict, index);
    return Unsupported("font:" + NameOr(subtype, "?"));
}

std::shared_ptr<const PdfFont> FontCache::Load(const PdfFile& file, const PdfValue& entry,
                                               const text::FontIndex* index) {
    // The index chooses the faces of standard fonts: the same dictionary under another index is
    // another font.
    PdfHash key = file.ValueHash(entry);
    key.hi ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(index)) * 0x9E3779B97F4A7C15ull;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = map_.find(key);
        if (found != map_.end()) {
            lru_.splice(lru_.begin(), lru_, found->second);
            return found->second->font;
        }
    }
    std::shared_ptr<const PdfFont> font = LoadFont(file, entry, index);
    const size_t bytes = font->Bytes() + 64;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = map_.find(key);
    if (found != map_.end()) return found->second->font;  // another thread was first
    if (bytes > budget_) return font;
    lru_.push_front(Entry{key, font, bytes});
    map_.emplace(key, lru_.begin());
    bytes_ += bytes;
    while (bytes_ > budget_ && !lru_.empty()) {
        bytes_ -= lru_.back().bytes;
        map_.erase(lru_.back().key);
        lru_.pop_back();
    }
    return font;
}

size_t FontCache::Bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

size_t FontCache::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

void FontCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    map_.clear();
    lru_.clear();
    bytes_ = 0;
}

FontCache& SharedFontCache() {
    static FontCache cache;
    return cache;
}

} // namespace rgui::pdf
