// SPDX-License-Identifier: Apache-2.0
#include "app/sourcemap.h"

#include <algorithm>
#include <cstdint>

#include <emmintrin.h>

namespace rgui {

namespace {

// `value` within [lo, hi]; NaN gives lo.
double Within(double value, double lo, double hi) {
    if (!(value > lo)) return lo;
    if (!(value < hi)) return hi;
    return value;
}

// The value at `x` of the segment (x0, y0)-(x1, y1), x held to the segment; y0 when the segment
// has no extent in x.
double Interpolate(double x, double x0, double y0, double x1, double y1) {
    if (!(x1 > x0)) return y0;
    return y0 + (y1 - y0) * Within((x - x0) / (x1 - x0), 0, 1);
}

// The line of `buffer` where span `s` begins (Lb), and the one after it (Le).
double BeginLine(const ui::TextBuffer& buffer, const SourceSpan& s) {
    return static_cast<double>(buffer.LineOf(std::min(s.begin, buffer.Size())));
}

double EndLine(const ui::TextBuffer& buffer, const SourceSpan& s) {
    return s.end >= buffer.Size() ? static_cast<double>(buffer.LineCount()) : static_cast<double>(buffer.LineOf(s.end));
}

// The byte offsets where lines of a text start, read forward 16 bytes at a time: one compare
// (SSE2) finds the newlines of a block, which are then counted off one by one.
class LineCursor {
public:
    explicit LineCursor(std::string_view text) : text_(text) {}

    // Where line `target` starts: after the text's target-th '\n', its size past the last line.
    // `target` never goes below the one asked for before.
    size_t Offset(uint32_t target) {
        if (line_ >= target) return offset_;
        const char* p = text_.data();
        const size_t n = text_.size();
        const __m128i lf = _mm_set1_epi8('\n');
        for (; n - scan_ >= 16; scan_ += 16) {
            const __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + scan_));
            for (uint32_t mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(block, lf))); mask != 0;
                 mask &= mask - 1) {
                if (line_ + 1 == target) return Found(scan_ + static_cast<size_t>(__builtin_ctz(mask)), target);
                line_++;
            }
        }
        for (; scan_ < n; scan_++) {
            if (p[scan_] != '\n') continue;
            if (line_ + 1 == target) return Found(scan_, target);
            line_++;
        }
        return n;
    }

private:
    size_t Found(size_t newline, uint32_t target) {
        line_ = target;
        offset_ = newline + 1;
        scan_ = offset_;
        return offset_;
    }

    std::string_view text_;
    uint32_t line_ = 0;   // the newlines before scan_; line_ starts at offset_ after a Found
    size_t offset_ = 0;
    size_t scan_ = 0;
};

} // namespace

SourceMap SourceMap::FromEngine(const std::vector<TinyPdf::SourceBlock>& blocks, std::string_view text,
                                const std::vector<double>& pageHeightsPt) {
    SourceMap map;
    if (pageHeightsPt.empty()) return map;
    map.pages_ = static_cast<double>(pageHeightsPt.size());
    const size_t lastPage = pageHeightsPt.size() - 1;
    const auto position = [&](uint32_t page, float y) {
        const size_t p = std::min<size_t>(page, lastPage);
        const double height = pageHeightsPt[p] > 0 ? pageHeightsPt[p] : 842.0;
        return static_cast<double>(p) + Within(static_cast<double>(y) / height, 0, 1);
    };
    LineCursor lines(text);
    uint32_t firstLine = 0, endLine = 0;   // the last span's lines
    map.spans_.reserve(blocks.size());
    for (const TinyPdf::SourceBlock& block : blocks) {
        if (block.endLine < block.firstLine) continue;
        const double start = position(block.page, block.top);
        const double stop = position(block.endPage, block.bottom);
        if (!map.spans_.empty()) {
            SourceSpan& last = map.spans_.back();
            // The blocks of one HTML block share its lines.
            if (block.firstLine == firstLine && block.endLine == endLine) {
                last.stop = std::max(last.stop, stop);
                continue;
            }
            if (block.firstLine < endLine || start < last.start) continue;
        }
        SourceSpan span;
        span.begin = lines.Offset(block.firstLine);
        span.end = lines.Offset(block.endLine);
        span.start = start;
        span.stop = stop;
        map.spans_.push_back(span);
        firstLine = block.firstLine;
        endLine = block.endLine;
    }
    return map;
}

// The knots, in (line, page position): (0, 0), then (Lb, start) and (Le, stop) of each span, then
// (LineCount, pages_); a line between two knots maps linearly.
double SourceMap::PageOfLine(const ui::TextBuffer& buffer, double line) const {
    const size_t lines = buffer.LineCount();
    if (spans_.empty() || lines == 0) return -1;
    const double x = Within(line, 0, static_cast<double>(lines));
    const size_t at = buffer.LineStart(std::min(static_cast<size_t>(x), lines - 1));
    // The span after the last one that begins at or before the line.
    const auto next = std::upper_bound(spans_.begin(), spans_.end(), at,
                                       [](size_t value, const SourceSpan& s) { return value < s.begin; });
    if (next == spans_.begin()) return Interpolate(x, 0, 0, BeginLine(buffer, spans_.front()), spans_.front().start);
    const SourceSpan& s = *(next - 1);
    const double end = EndLine(buffer, s);
    if (x < end) return Interpolate(x, BeginLine(buffer, s), s.start, end, s.stop);
    if (next == spans_.end()) return Interpolate(x, end, s.stop, static_cast<double>(lines), pages_);
    return Interpolate(x, end, s.stop, BeginLine(buffer, *next), next->start);
}

double SourceMap::LineOfPage(const ui::TextBuffer& buffer, double page) const {
    const size_t lines = buffer.LineCount();
    if (spans_.empty() || lines == 0) return -1;
    const double y = Within(page, 0, pages_);
    // The span after the last one that starts at or above the position.
    const auto next = std::upper_bound(spans_.begin(), spans_.end(), y,
                                       [](double value, const SourceSpan& s) { return value < s.start; });
    if (next == spans_.begin()) return Interpolate(y, 0, 0, spans_.front().start, BeginLine(buffer, spans_.front()));
    const SourceSpan& s = *(next - 1);
    const double end = EndLine(buffer, s);
    if (y < s.stop) return Interpolate(y, s.start, BeginLine(buffer, s), s.stop, end);
    if (next == spans_.end()) return Interpolate(y, s.stop, end, pages_, static_cast<double>(lines));
    return Interpolate(y, s.stop, end, next->start, BeginLine(buffer, *next));
}

} // namespace rgui
