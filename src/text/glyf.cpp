// SPDX-License-Identifier: Apache-2.0
// TrueType outlines ('glyf' and 'loca', OpenType specification 1.9), and the dispatch to CFF.
#include "text/sfnt.h"

#include <vector>

namespace rgui::text {
namespace {

constexpr int kMaxDepth = 8;
// Bounds on the total work of one glyph, composites included.
constexpr uint32_t kMaxPoints = 1u << 20;
constexpr uint32_t kMaxComponents = 4096;

uint16_t U16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}
int16_t I16(const uint8_t* p) {
    return static_cast<int16_t>(U16(p));
}

enum : uint8_t { kOnCurve = 0x01, kXShort = 0x02, kYShort = 0x04, kRepeat = 0x08, kXSame = 0x10, kYSame = 0x20 };
enum : uint16_t {
    kArgsAreWords = 0x0001,
    kArgsAreXy = 0x0002,
    kHaveScale = 0x0008,
    kMoreComponents = 0x0020,
    kXyScale = 0x0040,
    kTwoByTwo = 0x0080,
};

struct Budget {
    uint32_t points = 0;
    uint32_t components = 0;
};

class GlyfReader {
public:
    explicit GlyfReader(const FontFace& face) : face_(face) {}

    // The glyph's bytes within glyf; false when loca is out of range.
    bool Locate(uint16_t glyph, uint32_t& start, uint32_t& end) const {
        const TableSpan loca = face_.Loca();
        const TableSpan glyf = face_.Glyf();
        const uint8_t* const p = face_.Data() + loca.offset;
        if (face_.LongLoca()) {
            if ((static_cast<uint64_t>(glyph) + 2) * 4 > loca.length) return false;
            const uint8_t* const q = p + 4 * static_cast<size_t>(glyph);
            start = static_cast<uint32_t>(q[0]) << 24 | static_cast<uint32_t>(q[1]) << 16 | static_cast<uint32_t>(q[2]) << 8 | q[3];
            end = static_cast<uint32_t>(q[4]) << 24 | static_cast<uint32_t>(q[5]) << 16 | static_cast<uint32_t>(q[6]) << 8 | q[7];
        } else {
            if ((static_cast<uint64_t>(glyph) + 2) * 2 > loca.length) return false;
            start = 2u * U16(p + 2 * static_cast<size_t>(glyph));
            end = 2u * U16(p + 2 * static_cast<size_t>(glyph) + 2);
        }
        return start <= end && end <= glyf.length;
    }

    bool Emit(uint16_t glyph, const gfx::Matrix& m, int depth, gfx::Path& out) {
        if (glyph >= face_.GlyphCount()) return false;
        uint32_t start = 0, end = 0;
        if (!Locate(glyph, start, end)) return false;
        if (start == end) return true;   // no outline (a space)
        const uint8_t* const p = face_.Data() + face_.Glyf().offset + start;
        const size_t size = end - start;
        if (size < 10) return false;
        const int16_t contours = I16(p);
        if (contours >= 0) return Simple(p, size, static_cast<uint32_t>(contours), m, out);
        return Composite(p, size, m, depth, out);
    }

private:
    bool Simple(const uint8_t* p, size_t size, uint32_t contours, const gfx::Matrix& m, gfx::Path& out) {
        if (contours == 0) return true;
        size_t at = 10;
        if (contours * 2 > size - at) return false;
        endPoints_.resize(contours);
        int32_t previous = -1;
        for (uint32_t c = 0; c < contours; c++) {
            const int32_t e = U16(p + at + 2 * c);
            if (e <= previous) return false;
            endPoints_[c] = static_cast<uint16_t>(e);
            previous = e;
        }
        at += contours * 2;
        const uint32_t points = static_cast<uint32_t>(previous) + 1;
        budget_.points += points;
        if (budget_.points > kMaxPoints) return false;
        if (size - at < 2) return false;
        const size_t instructions = U16(p + at);
        at += 2;
        if (instructions > size - at) return false;
        at += instructions;

        flags_.resize(points);
        for (uint32_t i = 0; i < points;) {
            if (at >= size) return false;
            const uint8_t flag = p[at++];
            flags_[i++] = flag;
            if ((flag & kRepeat) != 0) {
                if (at >= size) return false;
                uint32_t repeat = p[at++];
                while (repeat-- > 0 && i < points) flags_[i++] = flag;
            }
        }
        xs_.resize(points);
        ys_.resize(points);
        int32_t value = 0;
        for (uint32_t i = 0; i < points; i++) {
            const uint8_t flag = flags_[i];
            if ((flag & kXShort) != 0) {
                if (at >= size) return false;
                value += (flag & kXSame) != 0 ? p[at] : -static_cast<int32_t>(p[at]);
                at += 1;
            } else if ((flag & kXSame) == 0) {
                if (size - at < 2) return false;
                value += I16(p + at);
                at += 2;
            }
            xs_[i] = value;
        }
        value = 0;
        for (uint32_t i = 0; i < points; i++) {
            const uint8_t flag = flags_[i];
            if ((flag & kYShort) != 0) {
                if (at >= size) return false;
                value += (flag & kYSame) != 0 ? p[at] : -static_cast<int32_t>(p[at]);
                at += 1;
            } else if ((flag & kYSame) == 0) {
                if (size - at < 2) return false;
                value += I16(p + at);
                at += 2;
            }
            ys_[i] = value;
        }

        uint32_t first = 0;
        for (uint32_t c = 0; c < contours; c++) {
            const uint32_t last = endPoints_[c];
            Contour(first, last, m, out);
            first = last + 1;
        }
        return true;
    }

    gfx::PointF Point(uint32_t i, const gfx::Matrix& m) const {
        return m.Apply(gfx::PointF{static_cast<float>(xs_[i]), static_cast<float>(ys_[i])});
    }

    static gfx::PointF Mid(gfx::PointF a, gfx::PointF b) {
        return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    }

    // One contour of quadratics: consecutive off-curve points have an on-curve point between them.
    void Contour(uint32_t first, uint32_t last, const gfx::Matrix& m, gfx::Path& out) const {
        const uint32_t n = last - first + 1;
        if (n < 2) return;
        // Start on an on-curve point: the first, else the last, else the middle of the two.
        gfx::PointF start;
        uint32_t begin = 0;   // the index (relative) after the start point
        uint32_t count = n;   // points still to visit
        const bool firstOn = (flags_[first] & kOnCurve) != 0;
        const bool lastOn = (flags_[last] & kOnCurve) != 0;
        if (firstOn) {
            start = Point(first, m);
            begin = 1;
            count = n - 1;
        } else if (lastOn) {
            start = Point(last, m);
            begin = 0;
            count = n - 1;
        } else {
            start = Mid(Point(first, m), Point(last, m));
            begin = 0;
            count = n;
        }
        out.MoveTo(start.x, start.y);
        bool havePending = false;
        gfx::PointF pending;
        for (uint32_t k = 0; k < count; k++) {
            const uint32_t i = first + (begin + k) % n;
            const gfx::PointF pt = Point(i, m);
            if ((flags_[i] & kOnCurve) != 0) {
                if (havePending) out.QuadTo(pending.x, pending.y, pt.x, pt.y);
                else out.LineTo(pt.x, pt.y);
                havePending = false;
            } else {
                if (havePending) {
                    const gfx::PointF mid = Mid(pending, pt);
                    out.QuadTo(pending.x, pending.y, mid.x, mid.y);
                }
                pending = pt;
                havePending = true;
            }
        }
        if (havePending) out.QuadTo(pending.x, pending.y, start.x, start.y);
        out.Close();
    }

    bool Composite(const uint8_t* p, size_t size, const gfx::Matrix& m, int depth, gfx::Path& out) {
        if (depth >= kMaxDepth) return false;
        size_t at = 10;
        for (;;) {
            if (++budget_.components > kMaxComponents) return false;
            if (size - at < 4) return false;
            const uint16_t flags = U16(p + at);
            const uint16_t component = U16(p + at + 2);
            at += 4;
            double dx = 0, dy = 0;
            if ((flags & kArgsAreWords) != 0) {
                if (size - at < 4) return false;
                dx = I16(p + at);
                dy = I16(p + at + 2);
                at += 4;
            } else {
                if (size - at < 2) return false;
                dx = static_cast<int8_t>(p[at]);
                dy = static_cast<int8_t>(p[at + 1]);
                at += 2;
            }
            double a = 1, b = 0, c = 0, d = 1;
            if ((flags & kHaveScale) != 0) {
                if (size - at < 2) return false;
                a = d = I16(p + at) / 16384.0;
                at += 2;
            } else if ((flags & kXyScale) != 0) {
                if (size - at < 4) return false;
                a = I16(p + at) / 16384.0;
                d = I16(p + at + 2) / 16384.0;
                at += 4;
            } else if ((flags & kTwoByTwo) != 0) {
                if (size - at < 8) return false;
                a = I16(p + at) / 16384.0;
                b = I16(p + at + 2) / 16384.0;
                c = I16(p + at + 4) / 16384.0;
                d = I16(p + at + 6) / 16384.0;
                at += 8;
            }
            // Point-matched components (anchor points instead of offsets) are skipped.
            if ((flags & kArgsAreXy) != 0) {
                const gfx::Matrix local{a, b, c, d, dx, dy};
                // The component's own point arrays are reused, so decode it with a fresh reader.
                GlyfReader child(face_);
                child.budget_ = budget_;
                if (!child.Emit(component, local.Multiply(m), depth + 1, out)) return false;
                budget_ = child.budget_;
            }
            if ((flags & kMoreComponents) == 0) break;
        }
        return true;
    }

    const FontFace& face_;
    Budget budget_;
    std::vector<uint16_t> endPoints_;
    std::vector<uint8_t> flags_;
    std::vector<int32_t> xs_;
    std::vector<int32_t> ys_;
};

} // namespace

bool GlyphOutline(const FontFace& face, uint16_t glyph, gfx::Path& out) {
    out.Clear();
    if (!face.IsOpen() || glyph >= face.GlyphCount()) return false;
    bool ok = false;
    if (face.OutlineFormat() == FontFace::Outlines::TrueType) {
        GlyfReader reader(face);
        ok = reader.Emit(glyph, gfx::Matrix(), 0, out);
    } else {
        const double upem = face.UnitsPerEm();
        const gfx::Matrix toUnits = face.Cff().GlyphMatrix(glyph).Multiply(gfx::Matrix::Scale(upem, upem));
        ok = face.Cff().Outline(glyph, out, toUnits);
    }
    if (!ok) out.Clear();
    return ok;
}

} // namespace rgui::text
