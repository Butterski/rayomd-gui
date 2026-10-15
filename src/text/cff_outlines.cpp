// SPDX-License-Identifier: Apache-2.0
// The Type 2 charstring interpreter follows Adobe Technical Note #5177 (The Type 2 Charstring
// Format); the charset and encoding formats follow #5176 (The Compact Font Format
// Specification), sections 12 and 13 and Appendix B.
#include "text/cff_outlines.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace rgui::text {
namespace {

namespace Cff = TinyPdf::Internal::Cff;

constexpr size_t kMaxDictEntries = 64;
constexpr int kMaxStack = 48;
constexpr uint32_t kMaxOperators = 65536;

uint16_t U16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

// The Standard encoding's SID of every code (Appendix B), 0 for codes it leaves out.
uint16_t StandardEncodingSid(uint8_t code) {
    if (code >= 32 && code <= 126) return static_cast<uint16_t>(code - 31);
    static constexpr struct {
        uint8_t code;
        uint8_t sid;
    } kHigh[] = {
        {161, 96}, {162, 97}, {163, 98}, {164, 99}, {165, 100}, {166, 101}, {167, 102}, {168, 103},
        {169, 104}, {170, 105}, {171, 106}, {172, 107}, {173, 108}, {174, 109}, {175, 110}, {177, 111},
        {178, 112}, {179, 113}, {180, 114}, {182, 115}, {183, 116}, {184, 117}, {185, 118}, {186, 119},
        {187, 120}, {188, 121}, {189, 122}, {191, 123}, {193, 124}, {194, 125}, {195, 126}, {196, 127},
        {197, 128}, {198, 129}, {199, 130}, {200, 131}, {202, 132}, {203, 133}, {205, 134}, {206, 135},
        {207, 136}, {208, 137}, {225, 138}, {227, 139}, {232, 140}, {233, 141}, {234, 142}, {235, 143},
        {241, 144}, {245, 145}, {248, 146}, {249, 147}, {250, 148}, {251, 149},
    };
    for (const auto& entry : kHigh) {
        if (entry.code == code) return entry.sid;
    }
    return 0;
}

// A FontMatrix DICT entry's six operands, when they make an invertible, finite matrix.
bool ParseMatrix(std::string_view entryBytes, gfx::Matrix& out) {
    if (entryBytes.empty()) return false;
    Cff::DictEntry entry;
    size_t count = 0;
    if (!Cff::ParseDict(entryBytes, &entry, 1, count) || count != 1 || entry.operandCount != 6) return false;
    const double* v = entry.operands;
    for (int i = 0; i < 6; i++) {
        if (!(v[i] > -1e6 && v[i] < 1e6)) return false;
    }
    const gfx::Matrix m{v[0], v[1], v[2], v[3], v[4], v[5]};
    gfx::Matrix inverse;
    if (!m.Invert(inverse)) return false;
    out = m;
    return true;
}

// Runs one desubroutinized charstring.
class Interpreter {
public:
    Interpreter(gfx::Path* path, const gfx::Matrix& m) : path_(path), m_(m) {}

    bool Run(std::string_view code) {
        const uint8_t* const bytes = reinterpret_cast<const uint8_t*>(code.data());
        const size_t size = code.size();
        size_t at = 0;
        uint32_t operators = 0;
        while (at < size) {
            const uint8_t b0 = bytes[at];
            if (b0 == 28 || b0 >= 32) {
                double value = 0;
                if (b0 == 28) {
                    if (size - at < 3) return false;
                    value = static_cast<int16_t>(U16(bytes + at + 1));
                    at += 3;
                } else if (b0 <= 246) {
                    value = b0 - 139;
                    at += 1;
                } else if (b0 <= 254) {
                    if (size - at < 2) return false;
                    value = b0 <= 250 ? (b0 - 247) * 256 + bytes[at + 1] + 108 : -(b0 - 251) * 256 - bytes[at + 1] - 108;
                    at += 2;
                } else {
                    if (size - at < 5) return false;
                    const int32_t fixed = static_cast<int32_t>(static_cast<uint32_t>(bytes[at + 1]) << 24 |
                        static_cast<uint32_t>(bytes[at + 2]) << 16 | static_cast<uint32_t>(bytes[at + 3]) << 8 |
                        bytes[at + 4]);
                    value = fixed / 65536.0;
                    at += 5;
                }
                if (count_ >= kMaxStack) return false;
                stack_[count_++] = value;
                continue;
            }
            if (++operators > kMaxOperators) return false;
            at++;
            switch (b0) {
            case 1:      // hstem
            case 3:      // vstem
            case 18:     // hstemhm
            case 23:     // vstemhm
                TakeWidth((count_ & 1) != 0);
                stems_ += static_cast<uint32_t>(count_) / 2;
                count_ = 0;
                break;
            case 19:     // hintmask
            case 20: {   // cntrmask
                TakeWidth((count_ & 1) != 0);
                stems_ += static_cast<uint32_t>(count_) / 2;
                count_ = 0;
                const size_t maskBytes = (stems_ + 7) / 8;
                if (maskBytes > size - at) return false;
                at += maskBytes;
                break;
            }
            case 21:     // rmoveto
                TakeWidth(count_ > 2);
                if (count_ < 2) return false;
                MoveTo(stack_[0], stack_[1]);
                count_ = 0;
                break;
            case 22:     // hmoveto
                TakeWidth(count_ > 1);
                if (count_ < 1) return false;
                MoveTo(stack_[0], 0);
                count_ = 0;
                break;
            case 4:      // vmoveto
                TakeWidth(count_ > 1);
                if (count_ < 1) return false;
                MoveTo(0, stack_[0]);
                count_ = 0;
                break;
            case 5:      // rlineto
                if (count_ < 2 || (count_ & 1) != 0) return false;
                for (int i = 0; i < count_; i += 2) LineTo(stack_[i], stack_[i + 1]);
                count_ = 0;
                break;
            case 6:      // hlineto
            case 7:      // vlineto
                if (count_ < 1) return false;
                for (int i = 0; i < count_; i++) {
                    const bool horizontal = ((i & 1) == 0) == (b0 == 6);
                    if (horizontal) LineTo(stack_[i], 0);
                    else LineTo(0, stack_[i]);
                }
                count_ = 0;
                break;
            case 8:      // rrcurveto
                if (count_ < 6 || count_ % 6 != 0) return false;
                for (int i = 0; i < count_; i += 6) Curve(i);
                count_ = 0;
                break;
            case 24:     // rcurveline
                if (count_ < 8 || (count_ - 2) % 6 != 0) return false;
                for (int i = 0; i < count_ - 2; i += 6) Curve(i);
                LineTo(stack_[count_ - 2], stack_[count_ - 1]);
                count_ = 0;
                break;
            case 25:     // rlinecurve
                if (count_ < 8 || (count_ - 6) % 2 != 0) return false;
                for (int i = 0; i < count_ - 6; i += 2) LineTo(stack_[i], stack_[i + 1]);
                Curve(count_ - 6);
                count_ = 0;
                break;
            case 26: {   // vvcurveto: dx1? {dya dxb dyb dyc}+
                if (count_ < 4 || count_ % 4 > 1) return false;
                int i = 0;
                double dx1 = 0;
                if (count_ % 4 == 1) dx1 = stack_[i++];
                for (; i < count_; i += 4) {
                    CurveTo(dx1, stack_[i], stack_[i + 1], stack_[i + 2], 0, stack_[i + 3]);
                    dx1 = 0;
                }
                count_ = 0;
                break;
            }
            case 27: {   // hhcurveto: dy1? {dxa dxb dyb dxc}+
                if (count_ < 4 || count_ % 4 > 1) return false;
                int i = 0;
                double dy1 = 0;
                if (count_ % 4 == 1) dy1 = stack_[i++];
                for (; i < count_; i += 4) {
                    CurveTo(stack_[i], dy1, stack_[i + 1], stack_[i + 2], stack_[i + 3], 0);
                    dy1 = 0;
                }
                count_ = 0;
                break;
            }
            case 30:     // vhcurveto
            case 31: {   // hvcurveto: alternating groups of 4, the last with an optional 5th
                if (count_ < 4 || count_ % 4 > 1) return false;
                bool horizontal = b0 == 31;
                for (int i = 0; i + 4 <= count_; i += 4) {
                    const double last = (i + 5 == count_) ? stack_[i + 4] : 0;
                    if (horizontal) CurveTo(stack_[i], 0, stack_[i + 1], stack_[i + 2], last, stack_[i + 3]);
                    else CurveTo(0, stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], last);
                    horizontal = !horizontal;
                }
                count_ = 0;
                break;
            }
            case 14:     // endchar
                TakeWidth(count_ > 0);
                if (count_ > 0) return false;    // seac never survives Desubroutinize
                ClosePath();
                ended_ = true;
                return true;
            case 12: {
                if (at >= size) return false;
                const uint8_t b1 = bytes[at++];
                if (!Escape(b1)) return false;
                count_ = 0;
                break;
            }
            default:     // subroutine calls are inlined; everything else is reserved
                return false;
            }
        }
        return false;   // no endchar
    }

    bool Ended() const { return ended_; }
    bool HasWidth() const { return hasWidth_; }
    double Width() const { return width_; }

private:
    // At the first stack-clearing operator, an extra leading operand is the width.
    void TakeWidth(bool present) {
        if (widthChecked_) return;
        widthChecked_ = true;
        if (present && count_ > 0) {
            hasWidth_ = true;
            width_ = stack_[0];
            // Shift the operands down so each operator reads from 0.
            for (int i = 1; i < count_; i++) stack_[i - 1] = stack_[i];
            count_--;
        }
    }

    void Emit(bool move, double px, double py) {
        if (path_ == nullptr) return;
        double ox = 0, oy = 0;
        m_.Apply(px, py, ox, oy);
        if (move) path_->MoveTo(static_cast<float>(ox), static_cast<float>(oy));
        else path_->LineTo(static_cast<float>(ox), static_cast<float>(oy));
    }

    void ClosePath() {
        if (open_ && path_ != nullptr) path_->Close();
        open_ = false;
    }

    void MoveTo(double dx, double dy) {
        ClosePath();
        x_ += dx;
        y_ += dy;
        Emit(true, x_, y_);
        open_ = true;
    }

    void EnsureOpen() {
        if (!open_) {
            Emit(true, x_, y_);
            open_ = true;
        }
    }

    void LineTo(double dx, double dy) {
        EnsureOpen();
        x_ += dx;
        y_ += dy;
        Emit(false, x_, y_);
    }

    void CurveTo(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
        EnsureOpen();
        const double x1 = x_ + dx1, y1 = y_ + dy1;
        const double x2 = x1 + dx2, y2 = y1 + dy2;
        x_ = x2 + dx3;
        y_ = y2 + dy3;
        if (path_ == nullptr) return;
        double a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
        m_.Apply(x1, y1, a, b);
        m_.Apply(x2, y2, c, d);
        m_.Apply(x_, y_, e, f);
        path_->CubicTo(static_cast<float>(a), static_cast<float>(b), static_cast<float>(c), static_cast<float>(d),
            static_cast<float>(e), static_cast<float>(f));
    }

    void Curve(int i) {
        CurveTo(stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], stack_[i + 4], stack_[i + 5]);
    }

    bool Escape(uint8_t op) {
        const double* s = stack_;
        switch (op) {
        case 0:      // dotsection (deprecated): no effect
            return true;
        case 35:     // flex: two curves and a flex depth
            if (count_ != 13) return false;
            Curve(0);
            Curve(6);
            return true;
        case 34:     // hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6
            if (count_ != 7) return false;
            CurveTo(s[0], 0, s[1], s[2], s[3], 0);
            CurveTo(s[4], 0, s[5], -s[2], s[6], 0);
            return true;
        case 36:     // hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6
            if (count_ != 9) return false;
            CurveTo(s[0], s[1], s[2], s[3], s[4], 0);
            CurveTo(s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
            return true;
        case 37: {   // flex1: dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6
            if (count_ != 11) return false;
            const double dx = s[0] + s[2] + s[4] + s[6] + s[8];
            const double dy = s[1] + s[3] + s[5] + s[7] + s[9];
            const double adx = dx < 0 ? -dx : dx;
            const double ady = dy < 0 ? -dy : dy;
            CurveTo(s[0], s[1], s[2], s[3], s[4], s[5]);
            if (adx > ady) CurveTo(s[6], s[7], s[8], s[9], s[10], -dy);
            else CurveTo(s[6], s[7], s[8], s[9], -dx, s[10]);
            return true;
        }
        default:     // the arithmetic and storage operators: not in fonts we read
            return false;
        }
    }

    gfx::Path* path_;
    const gfx::Matrix& m_;
    double stack_[kMaxStack] = {};
    int count_ = 0;
    uint32_t stems_ = 0;
    double x_ = 0;
    double y_ = 0;
    bool open_ = false;
    bool ended_ = false;
    bool widthChecked_ = false;
    bool hasWidth_ = false;
    double width_ = 0;
};

} // namespace

bool CffCharset::Read(const uint8_t* data, size_t size, uint32_t offset, uint16_t glyphCount) {
    ids_.assign(glyphCount, 0);
    byId_.clear();
    if (offset <= 2) {
        // The predefined charsets: ISOAdobe maps glyph i to SID i (the Expert ones read the same).
        for (uint32_t glyph = 0; glyph < glyphCount; glyph++) ids_[glyph] = static_cast<uint16_t>(glyph);
    } else {
        if (offset >= size) return false;
        const uint8_t format = data[offset];
        size_t at = static_cast<size_t>(offset) + 1;
        uint32_t glyph = 1;
        if (format == 0) {
            if (glyphCount > 0 && (static_cast<size_t>(glyphCount) - 1) * 2 > size - at) return false;
            for (; glyph < glyphCount; glyph++, at += 2) ids_[glyph] = U16(data + at);
        } else if (format == 1 || format == 2) {
            const size_t rangeBytes = format == 1 ? 3 : 4;
            while (glyph < glyphCount) {
                if (rangeBytes > size - at) return false;
                const uint32_t first = U16(data + at);
                const uint32_t left = format == 1 ? data[at + 2] : U16(data + at + 2);
                at += rangeBytes;
                for (uint32_t i = 0; i <= left && glyph < glyphCount; i++, glyph++) {
                    ids_[glyph] = static_cast<uint16_t>(first + i);
                }
            }
        } else {
            return false;
        }
    }
    byId_.reserve(glyphCount);
    for (uint32_t glyph = 0; glyph < glyphCount; glyph++) byId_.emplace_back(ids_[glyph], static_cast<uint16_t>(glyph));
    std::sort(byId_.begin(), byId_.end());
    return true;
}

uint16_t CffCharset::GlyphFor(uint16_t id) const {
    const auto it = std::lower_bound(byId_.begin(), byId_.end(), std::make_pair(id, static_cast<uint16_t>(0)));
    return it != byId_.end() && it->first == id ? it->second : 0;
}

bool CffEncoding::Read(const uint8_t* data, size_t size, uint32_t offset, uint16_t glyphCount,
    const CffCharset& charset) {
    glyphs_.fill(0);
    if (offset == 0) {
        for (int code = 0; code < 256; code++) {
            const uint16_t sid = StandardEncodingSid(static_cast<uint8_t>(code));
            if (sid != 0) glyphs_[code] = charset.GlyphFor(sid);
        }
        return true;
    }
    if (offset == 1) return true;   // Expert: nothing RayoMD's fonts use
    if (offset >= size) return false;
    const uint8_t format = data[offset];
    size_t at = static_cast<size_t>(offset) + 1;
    if (at >= size) return false;
    uint32_t glyph = 1;
    if ((format & 0x7f) == 0) {
        const uint32_t codes = data[at++];
        if (codes > size - at) return false;
        for (uint32_t i = 0; i < codes; i++, glyph++) {
            if (glyph < glyphCount) glyphs_[data[at + i]] = static_cast<uint16_t>(glyph);
        }
        at += codes;
    } else if ((format & 0x7f) == 1) {
        const uint32_t ranges = data[at++];
        if (ranges * 2 > size - at) return false;
        for (uint32_t r = 0; r < ranges; r++) {
            const uint32_t first = data[at + r * 2];
            const uint32_t left = data[at + r * 2 + 1];
            for (uint32_t code = first; code <= first + left && code < 256; code++, glyph++) {
                if (glyph < glyphCount) glyphs_[code] = static_cast<uint16_t>(glyph);
            }
        }
        at += ranges * 2;
    } else {
        return false;
    }
    if ((format & 0x80) != 0) {
        if (at >= size) return false;
        const uint32_t supplements = data[at++];
        if (supplements * 3 > size - at) return false;
        for (uint32_t s = 0; s < supplements; s++) {
            const uint8_t code = data[at + s * 3];
            const uint16_t sid = U16(data + at + s * 3 + 1);
            const uint16_t mapped = charset.GlyphFor(sid);
            if (mapped != 0) glyphs_[code] = mapped;
        }
    }
    return true;
}

bool CffOutlines::Open(const uint8_t* data, size_t size, uint16_t glyphCount) {
    *this = CffOutlines();
    if (data == nullptr || size < 4 || data[0] != 1) return false;
    const size_t headerSize = data[2];
    if (headerSize < 4 || headerSize > size) return false;
    Cff::Index names, topDicts;
    size_t next = 0;
    if (!Cff::ReadIndex(data, size, headerSize, names, next) || !Cff::ReadIndex(data, size, next, topDicts, next) ||
        topDicts.count == 0) {
        return false;
    }
    const std::string_view top = Cff::IndexItem(data, topDicts, 0);
    if (top.data() == nullptr) return false;
    std::vector<Cff::DictEntry> entries(kMaxDictEntries);
    size_t count = 0;
    if (!Cff::ParseDict(top, entries.data(), entries.size(), count)) return false;
    uint32_t charsetOffset = 0, encodingOffset = 0;
    bool haveCharStrings = false;
    uint32_t charStringsOffset = 0;
    for (size_t i = 0; i < count; i++) {
        const Cff::DictEntry& entry = entries[i];
        if (entry.operandCount < 1) continue;
        const double v = entry.operands[0];
        const bool offset = v >= 0 && v <= 2147483647.0 && static_cast<double>(static_cast<uint32_t>(v)) == v;
        if (entry.op == 15 && offset) charsetOffset = static_cast<uint32_t>(v);
        else if (entry.op == 16 && offset) encodingOffset = static_cast<uint32_t>(v);
        else if (entry.op == 17 && offset) {
            charStringsOffset = static_cast<uint32_t>(v);
            haveCharStrings = true;
        }
    }
    if (glyphCount == 0) {
        // Bare CFF: as many glyphs as the CharStrings INDEX holds.
        Cff::Index charStrings;
        if (!haveCharStrings || !Cff::ReadIndex(data, size, charStringsOffset, charStrings, next) ||
            charStrings.count == 0 || charStrings.count > 65535) {
            return false;
        }
        glyphCount = static_cast<uint16_t>(charStrings.count);
    }
    Cff::CffFont font;
    if (!Cff::ParseCff(data, size, glyphCount, font)) return false;

    std::vector<FdInfo> fds(font.fds.size());
    for (size_t index = 0; index < font.fds.size(); index++) {
        const Cff::FontDict& fd = font.fds[index];
        FdInfo& info = fds[index];
        if (!Cff::ParseDict(fd.privateDict, entries.data(), entries.size(), count)) return false;
        for (size_t i = 0; i < count; i++) {
            const Cff::DictEntry& entry = entries[i];
            if (entry.operandCount != 1 || !(entry.operands[0] > -1e6 && entry.operands[0] < 1e6)) continue;
            if (entry.op == 20) info.defaultWidthX = entry.operands[0];
            else if (entry.op == 21) info.nominalWidthX = entry.operands[0];
        }
        info.hasMatrix = font.cidKeyed && ParseMatrix(fd.fontMatrix, info.matrix);
    }
    hasTopMatrix_ = ParseMatrix(font.fontMatrix, topMatrix_);
    if (!hasTopMatrix_) topMatrix_ = gfx::Matrix{0.001, 0, 0, 0.001, 0, 0};
    if (!charset_.Read(data, size, charsetOffset, glyphCount)) return false;
    if (!font.cidKeyed && !encoding_.Read(data, size, encodingOffset, glyphCount, charset_)) return false;
    font_ = font;
    fds_ = std::move(fds);
    id_ = NewGlyphSourceId();
    return true;
}

gfx::Matrix CffOutlines::GlyphMatrix(uint16_t glyph) const {
    if (font_.cidKeyed && glyph < font_.glyphCount) {
        const uint8_t fd = font_.FdFor(glyph);
        if (fd < fds_.size() && fds_[fd].hasMatrix) {
            return hasTopMatrix_ ? fds_[fd].matrix.Multiply(topMatrix_) : fds_[fd].matrix;
        }
    }
    return topMatrix_;
}

bool CffOutlines::Outline(uint16_t glyph, gfx::Path& out, const gfx::Matrix& m, double* advance) const {
    if (!IsOpen() || glyph >= font_.glyphCount) return false;
    std::string code;
    if (!Cff::Desubroutinize(font_, glyph, code)) return false;
    Interpreter interpreter(&out, m);
    if (!interpreter.Run(code)) return false;
    if (advance != nullptr) {
        const uint8_t fd = font_.FdFor(glyph);
        const FdInfo& info = fds_[fd < fds_.size() ? fd : 0];
        *advance = interpreter.HasWidth() ? info.nominalWidthX + interpreter.Width() : info.defaultWidthX;
    }
    return true;
}

bool CffOutlines::Advance(uint16_t glyph, double& width) const {
    if (!IsOpen() || glyph >= font_.glyphCount) return false;
    std::string code;
    if (!Cff::Desubroutinize(font_, glyph, code)) return false;
    const gfx::Matrix identity;
    Interpreter interpreter(nullptr, identity);
    if (!interpreter.Run(code)) return false;
    const uint8_t fd = font_.FdFor(glyph);
    const FdInfo& info = fds_[fd < fds_.size() ? fd : 0];
    width = interpreter.HasWidth() ? info.nominalWidthX + interpreter.Width() : info.defaultWidthX;
    return true;
}

bool CffOutlines::GlyphPath(uint16_t glyph, gfx::Path& out) const {
    out.Clear();
    if (Outline(glyph, out, gfx::Matrix())) return true;
    out.Clear();
    return false;
}

} // namespace rgui::text
