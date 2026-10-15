// SPDX-License-Identifier: Apache-2.0
// CFF font programs: the 'CFF ' table of an OpenType font, or a bare CFF (a PDF's FontFile3
// /Type1C or /CIDFontType0C). The tables are checked by the engine's TinyPdf::Internal::Cff
// reader; each glyph is desubroutinized there and run through our Type 2 interpreter here.
// Every read is bounds-checked: font programs are untrusted.
#pragma once

#include "core/cff.h"
#include "gfx/geometry.h"
#include "gfx/path.h"
#include "text/glyphsource.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rgui::text {

// A charset: the SID (name-keyed fonts) or CID (CID-keyed fonts) of every glyph, formats 0, 1
// and 2, or the predefined ISOAdobe charset (offset 0; the Expert charsets 1 and 2 read as it).
class CffCharset {
public:
    bool Read(const uint8_t* data, size_t size, uint32_t offset, uint16_t glyphCount);
    // The first glyph whose SID or CID is `id`, or 0 (.notdef) when none has it.
    uint16_t GlyphFor(uint16_t id) const;
    // The SID or CID of `glyph`, or 0 when it is out of range.
    uint16_t IdOf(uint16_t glyph) const { return glyph < ids_.size() ? ids_[glyph] : 0; }

private:
    std::vector<uint16_t> ids_;                          // glyph -> SID or CID
    std::vector<std::pair<uint16_t, uint16_t>> byId_;    // (id, glyph), sorted by id then glyph
};

// The encoding of a name-keyed font: code -> glyph, formats 0 and 1 with their supplements, or
// the predefined Standard encoding (offset 0) through the charset. The Expert encoding
// (offset 1) maps nothing.
class CffEncoding {
public:
    bool Read(const uint8_t* data, size_t size, uint32_t offset, uint16_t glyphCount, const CffCharset& charset);
    // The glyph of `code`, or 0 when the encoding has none.
    uint16_t GlyphFor(uint8_t code) const { return glyphs_[code]; }

private:
    std::array<uint16_t, 256> glyphs_{};
};

class CffOutlines final : public GlyphSource {
public:
    // Reads the `size` bytes at `data`, which must stay alive and unchanged while this object
    // is used. `glyphCount` 0 takes the count of the CharStrings INDEX (bare CFF); otherwise it
    // must equal that count (OpenType's maxp.numGlyphs).
    bool Open(const uint8_t* data, size_t size, uint16_t glyphCount = 0);
    bool IsOpen() const { return font_.data != nullptr; }

    uint16_t GlyphCount() const { return font_.glyphCount; }
    bool IsCidKeyed() const { return font_.cidKeyed; }
    // The Top DICT's FontMatrix, or [0.001 0 0 0.001 0 0] when it has none.
    const gfx::Matrix& FontMatrix() const { return topMatrix_; }
    // The matrix from charstring units to em of `glyph`: for a CID-keyed font with a FontMatrix
    // in the glyph's font DICT, that matrix followed by the Top DICT's (identity when absent),
    // else FontMatrix().
    gfx::Matrix GlyphMatrix(uint16_t glyph) const;

    // The glyph's outline in charstring units transformed by `m`, appended to `out`; `advance`
    // (if not null) gets its advance width in charstring units. False when the glyph is out of
    // range or its charstring is malformed; `out` may then hold part of the outline.
    bool Outline(uint16_t glyph, gfx::Path& out, const gfx::Matrix& m, double* advance = nullptr) const;
    // The advance width in charstring units: nominalWidthX plus the charstring's width operand,
    // or defaultWidthX without one. False for a malformed glyph.
    bool Advance(uint16_t glyph, double& width) const;

    const CffCharset& Charset() const { return charset_; }
    // Name-keyed fonts only; a CID-keyed font's encoding maps nothing.
    const CffEncoding& Encoding() const { return encoding_; }

    uint32_t SourceId() const override { return id_; }
    bool GlyphPath(uint16_t glyph, gfx::Path& out) const override;
    gfx::Matrix UnitsToEm(uint16_t glyph) const override { return GlyphMatrix(glyph); }

private:
    struct FdInfo {
        double defaultWidthX = 0;
        double nominalWidthX = 0;
        gfx::Matrix matrix;
        bool hasMatrix = false;
    };

    TinyPdf::Internal::Cff::CffFont font_;
    std::vector<FdInfo> fds_;
    gfx::Matrix topMatrix_{0.001, 0, 0, 0.001, 0, 0};
    bool hasTopMatrix_ = false;
    CffCharset charset_;
    CffEncoding encoding_;
    uint32_t id_ = 0;
};

} // namespace rgui::text
