// SPDX-License-Identifier: Apache-2.0
// OpenType and TrueType fonts (sfnt), alone or in a TrueType/OpenType collection (ttcf): the
// table directory and the tables the toolkit uses. Every offset is checked against its table:
// font files are untrusted.
#pragma once

#include "gfx/geometry.h"
#include "gfx/path.h"
#include "text/cff_outlines.h"
#include "text/glyphsource.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rgui::text {

// At most this many faces of a collection are read.
inline constexpr uint32_t kMaxCollectionFaces = 64;

// The number of faces in `data`: 1 for an sfnt (version 0x00010000, 'OTTO' or 'true'), the
// collection's numFonts capped at kMaxCollectionFaces for 'ttcf', 0 for anything else.
uint32_t FaceCount(const uint8_t* data, size_t size);

// What the font index keeps of a face, read from its name, OS/2, head and post tables only.
struct FaceInfo {
    // Distinct family names: nameID 16, then nameID 1, from platform 3 encoding 1 (UTF-16BE)
    // records, or from platform 1 (Mac Roman) ones when the font has no platform 3 names.
    std::vector<std::string> families;
    std::string style;          // nameID 17, else 2
    uint16_t weight = 400;      // OS/2 usWeightClass (no OS/2: 700 for macStyle bold, else 400)
    uint16_t widthClass = 5;    // OS/2 usWidthClass, 5 = normal
    bool italic = false;        // OS/2 fsSelection bit 0 or head macStyle bit 1
    bool fixedPitch = false;    // post isFixedPitch
};

// False when the face does not exist or its tables are malformed.
bool ReadFaceInfo(const uint8_t* data, size_t size, uint32_t faceIndex, FaceInfo& info);

struct TableSpan {
    uint32_t offset = 0;
    uint32_t length = 0;
};

class FontFace final : public GlyphSource {
public:
    enum class Outlines : uint8_t { TrueType, Cff };

    // Reads face `faceIndex` of the sfnt or collection in `data` (a mapped file, or a PDF's
    // embedded font program); the bytes must stay alive and unchanged while the face is used,
    // copies of it included. Needs head, maxp, and glyf + loca or 'CFF '; hhea/hmtx, cmap,
    // OS/2 and post are optional (a PDF subset may lack them). Every Open gives a fresh id.
    bool Open(const uint8_t* data, size_t size, uint32_t faceIndex = 0);
    bool IsOpen() const { return data_ != nullptr; }

    uint16_t UnitsPerEm() const { return unitsPerEm_; }
    uint16_t GlyphCount() const { return glyphCount_; }
    Outlines OutlineFormat() const { return outlines_; }
    // OS/2 typo metrics, or hhea's when the font has no OS/2; in font units, descender < 0.
    int16_t Ascender() const { return ascender_; }
    int16_t Descender() const { return descender_; }
    int16_t LineGap() const { return lineGap_; }
    uint16_t Weight() const { return weight_; }
    bool Italic() const { return italic_; }
    bool FixedPitch() const { return fixedPitch_; }
    uint16_t MacStyle() const { return macStyle_; }

    // The glyph cmap maps `codePoint` to, or 0. The subtable is the first of (3,10), (0,4),
    // (0,6), (3,1), (0,3) that is format 4 or 12.
    uint16_t GlyphForCodePoint(uint32_t codePoint) const;
    bool HasCmap() const { return cmapFormat_ != 0; }
    // hmtx's advance width (the last long metric for glyphs past numberOfHMetrics), 0 without.
    uint16_t AdvanceWidth(uint16_t glyph) const;

    // The raw bytes of the face and a table of it (an empty span when absent).
    const uint8_t* Data() const { return data_; }
    size_t Size() const { return size_; }
    TableSpan Glyf() const { return glyf_; }
    TableSpan Loca() const { return loca_; }
    bool LongLoca() const { return longLoca_; }
    // The CFF program of an OpenType CFF face (closed for TrueType outlines).
    const CffOutlines& Cff() const { return cff_; }

    uint32_t SourceId() const override { return id_; }
    // The outline in font units, y up (see GlyphOutline).
    bool GlyphPath(uint16_t glyph, gfx::Path& out) const override;
    gfx::Matrix UnitsToEm(uint16_t) const override;

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    uint32_t id_ = 0;
    uint16_t unitsPerEm_ = 0;
    uint16_t glyphCount_ = 0;
    Outlines outlines_ = Outlines::TrueType;
    int16_t ascender_ = 0;
    int16_t descender_ = 0;
    int16_t lineGap_ = 0;
    uint16_t weight_ = 400;
    uint16_t macStyle_ = 0;
    bool italic_ = false;
    bool fixedPitch_ = false;
    bool longLoca_ = false;
    uint16_t hMetrics_ = 0;
    TableSpan hmtx_;
    TableSpan glyf_;
    TableSpan loca_;
    uint16_t cmapFormat_ = 0;     // 4, 12, or 0 when there is no usable subtable
    uint32_t cmapOffset_ = 0;     // of the subtable, in data_
    uint32_t cmapEnd_ = 0;        // the end of what the subtable may read
    uint32_t cmapCount_ = 0;      // format 4: segCount; format 12: numGroups
    CffOutlines cff_;
};

// The glyph's outline in font units, y up, into `out` (cleared first). TrueType: contours of
// quadratics with the implicit on-curve midpoints; composites with offsets, scales and 2x2
// transforms, nested at most 8 deep (point-matched components are skipped). CFF: the
// charstring through the FontMatrix, scaled by unitsPerEm. An empty glyph is an empty path
// and true; a malformed one false, with `out` empty.
bool GlyphOutline(const FontFace& face, uint16_t glyph, gfx::Path& out);

} // namespace rgui::text
