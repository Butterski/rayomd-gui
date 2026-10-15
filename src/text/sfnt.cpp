// SPDX-License-Identifier: Apache-2.0
// Table layouts from the OpenType specification 1.9 (head, hhea, hmtx, maxp, OS/2, post, name,
// cmap formats 4 and 12, the table directory and the collection header).
#include "text/sfnt.h"

#include <atomic>

namespace rgui::text {
namespace {

constexpr uint32_t Tag(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16 |
        static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8 | static_cast<uint8_t>(d);
}

uint16_t U16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}
int16_t I16(const uint8_t* p) {
    return static_cast<int16_t>(U16(p));
}
uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

struct Directory {
    TableSpan head, hhea, hmtx, maxp, os2, post, name, cmap, glyf, loca, cff;
};

// The table records of face `faceIndex`. A table that reaches past the data is left absent.
bool ReadDirectory(const uint8_t* data, size_t size, uint32_t faceIndex, Directory& dir) {
    dir = Directory();
    if (data == nullptr || size < 12) return false;
    size_t faceOffset = 0;
    if (U32(data) == Tag('t', 't', 'c', 'f')) {
        const uint32_t faces = U32(data + 8);
        if (faceIndex >= faces || faceIndex >= kMaxCollectionFaces) return false;
        if (12 + 4 * (static_cast<size_t>(faceIndex) + 1) > size) return false;
        faceOffset = U32(data + 12 + 4 * static_cast<size_t>(faceIndex));
    } else if (faceIndex != 0) {
        return false;
    }
    if (faceOffset > size - 12) return false;
    const uint8_t* const face = data + faceOffset;
    const uint32_t version = U32(face);
    if (version != 0x00010000u && version != Tag('O', 'T', 'T', 'O') && version != Tag('t', 'r', 'u', 'e')) return false;
    const size_t tables = U16(face + 4);
    if (tables * 16 > size - faceOffset - 12) return false;
    for (size_t i = 0; i < tables; i++) {
        const uint8_t* const record = face + 12 + i * 16;
        const uint32_t tag = U32(record);
        TableSpan span{U32(record + 8), U32(record + 12)};
        if (static_cast<uint64_t>(span.offset) + span.length > size || span.length == 0) continue;
        TableSpan* slot = nullptr;
        switch (tag) {
        case Tag('h', 'e', 'a', 'd'): slot = &dir.head; break;
        case Tag('h', 'h', 'e', 'a'): slot = &dir.hhea; break;
        case Tag('h', 'm', 't', 'x'): slot = &dir.hmtx; break;
        case Tag('m', 'a', 'x', 'p'): slot = &dir.maxp; break;
        case Tag('O', 'S', '/', '2'): slot = &dir.os2; break;
        case Tag('p', 'o', 's', 't'): slot = &dir.post; break;
        case Tag('n', 'a', 'm', 'e'): slot = &dir.name; break;
        case Tag('c', 'm', 'a', 'p'): slot = &dir.cmap; break;
        case Tag('g', 'l', 'y', 'f'): slot = &dir.glyf; break;
        case Tag('l', 'o', 'c', 'a'): slot = &dir.loca; break;
        case Tag('C', 'F', 'F', ' '): slot = &dir.cff; break;
        default: break;
        }
        if (slot != nullptr && slot->length == 0) *slot = span;
    }
    return true;
}

// Mac Roman 0x80-0xFF.
constexpr uint16_t kMacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string DecodeUtf16Be(const uint8_t* p, size_t length) {
    std::string out;
    for (size_t i = 0; i + 1 < length; i += 2) {
        uint32_t unit = U16(p + i);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < length) {
            const uint32_t low = U16(p + i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                AppendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                i += 2;
                continue;
            }
        }
        if (unit >= 0xD800 && unit <= 0xDFFF) unit = 0xFFFD;
        if (unit != 0) AppendUtf8(out, unit);
    }
    return out;
}

std::string DecodeMacRoman(const uint8_t* p, size_t length) {
    std::string out;
    for (size_t i = 0; i < length; i++) {
        if (p[i] == 0) continue;
        AppendUtf8(out, p[i] < 0x80 ? p[i] : kMacRoman[p[i] - 0x80]);
    }
    return out;
}

// One string of the name table, by preference: platform 3 encoding 1 (English US first, then
// any language), or platform 1 encoding 0 (English first) when `mac`.
struct NameTable {
    const uint8_t* data = nullptr;
    size_t size = 0;
    TableSpan span;
    size_t count = 0;
    size_t storage = 0;

    bool Init(const uint8_t* d, size_t s, TableSpan t) {
        data = d;
        size = s;
        span = t;
        if (t.length < 6) return false;
        const uint8_t* const p = d + t.offset;
        count = U16(p + 2);
        storage = U16(p + 4);
        if (6 + count * 12 > t.length) count = (t.length - 6) / 12;
        return true;
    }

    // Every string of `nameId` on the platform, English first, appended to `out` if new.
    void Collect(uint16_t nameId, bool mac, std::vector<std::string>& out) const {
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < count; i++) {
                const uint8_t* const r = data + span.offset + 6 + i * 12;
                const uint16_t platform = U16(r), encoding = U16(r + 2), language = U16(r + 4), id = U16(r + 6);
                const size_t length = U16(r + 8), offset = U16(r + 10);
                if (id != nameId) continue;
                const bool english = mac ? language == 0 : language == 0x0409;
                if ((pass == 0) != english) continue;
                if (mac ? (platform != 1 || encoding != 0) : (platform != 3 || encoding != 1)) continue;
                const size_t at = storage + offset;
                if (at > span.length || length > span.length - at || length == 0 || length > 1024) continue;
                const uint8_t* const p = data + span.offset + at;
                std::string text = mac ? DecodeMacRoman(p, length) : DecodeUtf16Be(p, length);
                if (text.empty()) continue;
                bool known = false;
                for (const std::string& s : out) known = known || s == text;
                if (!known) out.push_back(std::move(text));
            }
        }
    }

    bool HasWindowsNames() const {
        for (size_t i = 0; i < count; i++) {
            const uint8_t* const r = data + span.offset + 6 + i * 12;
            if (U16(r) == 3 && U16(r + 2) == 1) return true;
        }
        return false;
    }
};

uint16_t NormalizeWeight(uint16_t weight) {
    if (weight >= 1 && weight <= 9) return static_cast<uint16_t>(weight * 100);
    if (weight == 0) return 400;
    return weight > 1000 ? 1000 : weight;
}

} // namespace

uint32_t NewGlyphSourceId() {
    static std::atomic<uint32_t> counter{0};
    uint32_t id = 0;
    do {
        id = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    } while (id == 0);
    return id;
}

uint32_t FaceCount(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 12) return 0;
    const uint32_t tag = U32(data);
    if (tag == Tag('t', 't', 'c', 'f')) {
        uint32_t faces = U32(data + 8);
        if (faces > kMaxCollectionFaces) faces = kMaxCollectionFaces;
        if (12 + 4 * static_cast<size_t>(faces) > size) faces = static_cast<uint32_t>((size - 12) / 4);
        return faces;
    }
    return tag == 0x00010000u || tag == Tag('O', 'T', 'T', 'O') || tag == Tag('t', 'r', 'u', 'e') ? 1 : 0;
}

bool ReadFaceInfo(const uint8_t* data, size_t size, uint32_t faceIndex, FaceInfo& info) {
    info = FaceInfo();
    Directory dir;
    if (!ReadDirectory(data, size, faceIndex, dir)) return false;
    // A face without outlines we draw (CFF2 variable fonts, bitmap-only fonts) is left out.
    if ((dir.glyf.length == 0 || dir.loca.length == 0) && dir.cff.length == 0) return false;
    NameTable names;
    if (!names.Init(data, size, dir.name)) return false;
    const bool mac = !names.HasWindowsNames();
    names.Collect(16, mac, info.families);
    names.Collect(1, mac, info.families);
    if (info.families.empty()) return false;
    std::vector<std::string> styles;
    names.Collect(17, mac, styles);
    if (styles.empty()) names.Collect(2, mac, styles);
    if (!styles.empty()) info.style = styles.front();

    uint16_t macStyle = 0;
    if (dir.head.length >= 54) macStyle = U16(data + dir.head.offset + 44);
    info.weight = (macStyle & 1) != 0 ? 700 : 400;
    info.italic = (macStyle & 2) != 0;
    if (dir.os2.length >= 64) {
        const uint8_t* const os2 = data + dir.os2.offset;
        info.weight = NormalizeWeight(U16(os2 + 4));
        const uint16_t width = U16(os2 + 6);
        if (width >= 1 && width <= 9) info.widthClass = width;
        info.italic = info.italic || (U16(os2 + 62) & 1) != 0;
    }
    if (dir.post.length >= 16) info.fixedPitch = U32(data + dir.post.offset + 12) != 0;
    return true;
}

bool FontFace::Open(const uint8_t* data, size_t size, uint32_t faceIndex) {
    *this = FontFace();
    Directory dir;
    if (!ReadDirectory(data, size, faceIndex, dir)) return false;
    if (dir.head.length < 54 || dir.maxp.length < 6) return false;
    const uint8_t* const head = data + dir.head.offset;
    const uint16_t unitsPerEm = U16(head + 18);
    if (unitsPerEm < 16 || unitsPerEm > 16384) return false;
    const uint16_t glyphCount = U16(data + dir.maxp.offset + 4);
    if (glyphCount == 0) return false;

    FontFace face;
    face.data_ = data;
    face.size_ = size;
    face.unitsPerEm_ = unitsPerEm;
    face.glyphCount_ = glyphCount;
    face.macStyle_ = U16(head + 44);
    const int16_t locFormat = I16(head + 50);
    if (dir.glyf.length > 0 && dir.loca.length > 0 && (locFormat == 0 || locFormat == 1)) {
        face.outlines_ = Outlines::TrueType;
        face.glyf_ = dir.glyf;
        face.loca_ = dir.loca;
        face.longLoca_ = locFormat == 1;
    } else if (dir.cff.length > 0) {
        face.outlines_ = Outlines::Cff;
        if (!face.cff_.Open(data + dir.cff.offset, dir.cff.length, glyphCount)) return false;
    } else {
        return false;
    }

    face.weight_ = (face.macStyle_ & 1) != 0 ? 700 : 400;
    face.italic_ = (face.macStyle_ & 2) != 0;
    if (dir.hhea.length >= 36) {
        const uint8_t* const hhea = data + dir.hhea.offset;
        face.ascender_ = I16(hhea + 4);
        face.descender_ = I16(hhea + 6);
        face.lineGap_ = I16(hhea + 8);
        uint32_t metrics = U16(hhea + 34);
        if (metrics > dir.hmtx.length / 4) metrics = dir.hmtx.length / 4;
        if (metrics > glyphCount) metrics = glyphCount;
        face.hMetrics_ = static_cast<uint16_t>(metrics);
        face.hmtx_ = dir.hmtx;
    }
    if (dir.os2.length >= 64) {
        const uint8_t* const os2 = data + dir.os2.offset;
        face.weight_ = NormalizeWeight(U16(os2 + 4));
        face.italic_ = face.italic_ || (U16(os2 + 62) & 1) != 0;
        if (dir.os2.length >= 74) {
            face.ascender_ = I16(os2 + 68);
            face.descender_ = I16(os2 + 70);
            face.lineGap_ = I16(os2 + 72);
        }
    }
    if (dir.post.length >= 16) face.fixedPitch_ = U32(data + dir.post.offset + 12) != 0;

    if (dir.cmap.length >= 4) {
        const uint8_t* const cmap = data + dir.cmap.offset;
        size_t records = U16(cmap + 2);
        if (4 + records * 8 > dir.cmap.length) records = (dir.cmap.length - 4) / 8;
        static constexpr struct {
            uint16_t platform, encoding;
        } kPreference[] = {{3, 10}, {0, 4}, {0, 6}, {3, 1}, {0, 3}};
        const uint32_t cmapEnd = dir.cmap.offset + dir.cmap.length;
        for (const auto& want : kPreference) {
            for (size_t i = 0; i < records && face.cmapFormat_ == 0; i++) {
                const uint8_t* const r = cmap + 4 + i * 8;
                if (U16(r) != want.platform || U16(r + 2) != want.encoding) continue;
                const uint32_t offset = U32(r + 4);
                if (offset > dir.cmap.length || dir.cmap.length - offset < 16) continue;
                const uint32_t at = dir.cmap.offset + offset;
                const uint16_t format = U16(data + at);
                if (format == 4) {
                    const uint32_t segCount = U16(data + at + 6) / 2u;
                    if (segCount == 0 || static_cast<uint64_t>(at) + 16 + 8ull * segCount > cmapEnd) continue;
                    face.cmapCount_ = segCount;
                } else if (format == 12) {
                    const uint32_t groups = U32(data + at + 12);
                    if (static_cast<uint64_t>(at) + 16 + 12ull * groups > cmapEnd) continue;
                    face.cmapCount_ = groups;
                } else {
                    continue;
                }
                face.cmapFormat_ = format;
                face.cmapOffset_ = at;
                face.cmapEnd_ = cmapEnd;
            }
            if (face.cmapFormat_ != 0) break;
        }
    }
    face.id_ = NewGlyphSourceId();
    *this = face;
    return true;
}

uint16_t FontFace::GlyphForCodePoint(uint32_t cp) const {
    const uint8_t* const base = data_ + cmapOffset_;
    uint32_t glyph = 0;
    if (cmapFormat_ == 4) {
        if (cp > 0xFFFF) return 0;
        const uint32_t segs = cmapCount_;
        const uint8_t* const ends = base + 14;
        uint32_t low = 0, high = segs;
        while (low < high) {
            const uint32_t mid = (low + high) / 2;
            if (U16(ends + 2 * mid) < cp) low = mid + 1;
            else high = mid;
        }
        if (low == segs) return 0;
        const uint32_t start = U16(base + 16 + 2 * segs + 2 * low);
        if (start > cp) return 0;
        const uint16_t delta = U16(base + 16 + 4 * segs + 2 * low);
        const uint32_t rangeAt = cmapOffset_ + 16 + 6 * segs + 2 * low;
        const uint16_t rangeOffset = U16(data_ + rangeAt);
        if (rangeOffset == 0) {
            glyph = (cp + delta) & 0xFFFF;
        } else {
            const uint64_t at = static_cast<uint64_t>(rangeAt) + rangeOffset + 2ull * (cp - start);
            if (at + 2 > cmapEnd_) return 0;
            const uint16_t value = U16(data_ + at);
            glyph = value == 0 ? 0 : (value + delta) & 0xFFFF;
        }
    } else if (cmapFormat_ == 12) {
        const uint8_t* const groups = base + 16;
        uint32_t low = 0, high = cmapCount_;
        while (low < high) {
            const uint32_t mid = low + (high - low) / 2;
            if (U32(groups + 12ull * mid + 4) < cp) low = mid + 1;
            else high = mid;
        }
        if (low == cmapCount_) return 0;
        const uint8_t* const g = groups + 12ull * low;
        const uint32_t start = U32(g);
        if (start > cp) return 0;
        const uint64_t value = static_cast<uint64_t>(U32(g + 8)) + (cp - start);
        glyph = value > 0xFFFF ? 0 : static_cast<uint32_t>(value);
    }
    return glyph < glyphCount_ ? static_cast<uint16_t>(glyph) : 0;
}

uint16_t FontFace::AdvanceWidth(uint16_t glyph) const {
    if (hMetrics_ == 0 || glyph >= glyphCount_) return 0;
    const uint32_t index = glyph < hMetrics_ ? glyph : hMetrics_ - 1u;
    return U16(data_ + hmtx_.offset + 4 * index);
}

bool FontFace::GlyphPath(uint16_t glyph, gfx::Path& out) const {
    return GlyphOutline(*this, glyph, out);
}

gfx::Matrix FontFace::UnitsToEm(uint16_t) const {
    const double s = unitsPerEm_ > 0 ? 1.0 / unitsPerEm_ : 0.0;
    return gfx::Matrix::Scale(s, s);
}

} // namespace rgui::text
