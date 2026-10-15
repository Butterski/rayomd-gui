// SPDX-License-Identifier: Apache-2.0
// PDF fonts of the preview (plan Phase 6 step 3): from a font dictionary to glyphs and widths.
//
// - Standard Type1 fonts without a font program: Helvetica*, Courier* and Times* (and Arial*)
//   drawn with a system face from the font index (Helvetica: Liberation Sans, Arimo, Nimbus Sans,
//   DejaVu Sans, Arial; Courier: Liberation Mono, Cousine, Nimbus Mono PS, DejaVu Sans Mono,
//   Courier New; Times: Liberation Serif, Tinos, Nimbus Roman, DejaVu Serif, Times New Roman),
//   weight 700 for *Bold*, italic for *Oblique / *Italic. Codes are WinAnsi, mapped to code
//   points (RayoMd::Text::WinAnsiToUtf8) and through the face's cmap. Widths are /Widths, else
//   the engine's StandardTextWidth (Courier: Mono, *Bold*: Bold, else Regular).
// - Type0 / CIDFontType2 / FontFile2: TrueType outlines; CID -> glyph through /CIDToGIDMap (a
//   stream of big-endian u16, or /Identity); widths /W, /DW (default 1000).
// - Type0 / CIDFontType0 / FontFile3 /CIDFontType0C: a bare CFF; CID -> glyph through its
//   charset (a CFF that is not CID-keyed takes the CID as the glyph); widths /W, /DW.
// - Type1 / FontFile3 /Type1C: a bare CFF; code -> glyph through its encoding; widths
//   /FirstChar /Widths (/MissingWidth of the descriptor elsewhere).
// Type0 fonts take two-byte codes (/Identity-H, /Identity-V), the others one byte. Anything else
// is unsupported: its text is not drawn and the renderer reports font:<what>.
//
// Fonts own a copy of their font program, so a cached font outlives the PdfFile it came from and
// the same font in the next preview build (same ValueHash) keeps its glyph-source id, and so its
// glyphs in the glyph caches.
#pragma once

#include "pdf/reader.h"
#include "text/fontindex.h"
#include "text/glyphsource.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rgui::pdf {

// One shown code.
struct PdfGlyph {
    uint16_t glyph = 0;
    double width = 0;      // advance in thousandths of text space
    bool wordSpace = false;  // a single-byte code 32: Tw applies
};

class PdfFont {
public:
    virtual ~PdfFont() = default;

    // The next code of `bytes` from `at`, which moves past it. False at the end (a trailing
    // partial code is dropped).
    virtual bool Next(std::string_view bytes, size_t& at, PdfGlyph& out) const = 0;
    // Where the glyphs' outlines come from; nullptr for an unsupported font.
    virtual const text::GlyphSource* Source() const = 0;
    // For an unsupported font, what the renderer reports ("font:Type3", "font:no-substitute:
    // Helvetica", ...); empty otherwise.
    virtual std::string_view Unsupported() const { return {}; }
    // Memory the font holds, for the cache's budget.
    virtual size_t Bytes() const = 0;
};

// Reads the font dictionary `font` (resolved) of `file`. Never nullptr: a font that cannot be
// read is an unsupported one. `index` finds the faces of the standard fonts; nullptr has none.
std::shared_ptr<const PdfFont> LoadFont(const PdfFile& file, const PdfValue& font, const text::FontIndex* index);

// Fonts keyed by PdfFile::ValueHash of the font's entry (its dictionaries and programs, whatever
// the object numbers) and the font index, least recently used evicted beyond a byte budget.
// Safe to use from any number of threads at once.
class FontCache {
public:
    static constexpr size_t kDefaultBudget = 64u << 20;

    explicit FontCache(size_t budgetBytes = kDefaultBudget) : budget_(budgetBytes) {}
    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;

    // The font of `entry` (a font dictionary or a reference to one), from the cache or read now.
    std::shared_ptr<const PdfFont> Load(const PdfFile& file, const PdfValue& entry, const text::FontIndex* index);

    size_t Bytes() const;
    size_t Count() const;
    void Clear();

private:
    struct Entry {
        PdfHash key;
        std::shared_ptr<const PdfFont> font;
        size_t bytes = 0;
    };
    using List = std::list<Entry>;

    mutable std::mutex mutex_;
    size_t budget_;
    size_t bytes_ = 0;
    List lru_;  // most recent first
    std::unordered_map<PdfHash, List::iterator, PdfHashHasher> map_;
};

// The process's font cache, which preview builds share.
FontCache& SharedFontCache();

} // namespace rgui::pdf
