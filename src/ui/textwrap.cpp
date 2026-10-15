// SPDX-License-Identifier: Apache-2.0
#include "ui/textwrap.h"

#include "common/text_utils.h"
#include "text/unicode.h"

#include <algorithm>

namespace rgui::ui {

namespace {

// The code point at line[i] and its length; malformed bytes are U+FFFD of length 1.
uint32_t DecodeAt(std::string_view line, size_t i, size_t& len) {
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if (c < 0x80) {
        len = 1;
        return c;
    }
    uint32_t cp = 0;
    if (RayoMd::Text::DecodeUtf8(line, i, cp, len)) return cp;
    len = 1;
    return 0xFFFD;
}

} // namespace

int CellsAt(uint32_t cp, int col) {
    if (cp == '\t') return kTabCells - col % kTabCells;
    if (cp < 0x80) return 1;
    return text::CellWidth(cp);
}

uint32_t WrapLine(std::string_view line, int wrapCells, std::vector<uint32_t>& breaks) {
    breaks.clear();
    if (wrapCells < 1) wrapCells = 1;
    const size_t npos = static_cast<size_t>(-1);
    // The unwrapped width first: most lines fit and need no second pass.
    uint32_t total = 0;
    for (size_t i = 0; i < line.size();) {
        size_t len;
        const uint32_t cp = DecodeAt(line, i, len);
        total += static_cast<uint32_t>(CellsAt(cp, static_cast<int>(total)));
        i += len;
    }
    if (total <= static_cast<uint32_t>(wrapCells)) return total;

    int col = 0;
    size_t lastBreak = npos;   // just after the last space of the current row
    for (size_t i = 0; i < line.size();) {
        size_t len;
        const uint32_t cp = DecodeAt(line, i, len);
        const int w = CellsAt(cp, col);
        const bool space = cp == ' ' || cp == '\t';
        if (!space && w > 0 && col > 0 && col + w > wrapCells) {
            const size_t brk = lastBreak != npos ? lastBreak : i;
            breaks.push_back(static_cast<uint32_t>(brk));
            col = 0;
            lastBreak = npos;
            i = brk;
            continue;
        }
        col += w;
        i += len;
        if (space) lastBreak = i;
    }
    return total;
}

void WrapLayout::WrapOne(const TextBuffer& buffer, size_t index) {
    const size_t start = buffer.LineStart(index);
    const std::string_view text = buffer.Range(start, buffer.LineEnd(index) - start, scratch_);
    lines_[index].cells = WrapLine(text, wrapCells_, lines_[index].breaks);
}

void WrapLayout::Rebuild() {
    const size_t n = lines_.size();
    tree_.assign(n + 1, 0);
    totalRows_ = 0;
    for (size_t i = 1; i <= n; i++) {
        const uint32_t rows = static_cast<uint32_t>(lines_[i - 1].breaks.size() + 1);
        totalRows_ += rows;
        tree_[i] += rows;
        const size_t parent = i + (i & (~i + 1));
        if (parent <= n) tree_[parent] += tree_[i];
    }
}

void WrapLayout::Add(size_t line, long delta) {
    if (delta == 0) return;
    totalRows_ = static_cast<size_t>(static_cast<long>(totalRows_) + delta);
    for (size_t i = line + 1; i < tree_.size(); i += i & (~i + 1)) {
        tree_[i] = static_cast<uint32_t>(static_cast<long>(tree_[i]) + delta);
    }
}

void WrapLayout::Reset(const TextBuffer& buffer, int wrapCells) {
    wrapCells_ = std::max(1, wrapCells);
    lines_.assign(buffer.LineCount(), Line{});
    for (size_t i = 0; i < lines_.size(); i++) WrapOne(buffer, i);
    Rebuild();
}

void WrapLayout::SetWrapCells(const TextBuffer& buffer, int wrapCells) {
    wrapCells = std::max(1, wrapCells);
    if (wrapCells == wrapCells_ && lines_.size() == buffer.LineCount()) return;
    if (lines_.size() != buffer.LineCount()) {
        Reset(buffer, wrapCells);
        return;
    }
    const uint32_t narrower = static_cast<uint32_t>(std::min(wrapCells, wrapCells_));
    wrapCells_ = wrapCells;
    for (size_t i = 0; i < lines_.size(); i++) {
        if (lines_[i].cells > narrower) WrapOne(buffer, i);
    }
    Rebuild();
}

void WrapLayout::OnEdit(const TextBuffer& buffer, size_t firstLine, size_t oldLastLine, size_t newLastLine) {
    const size_t oldCount = oldLastLine - firstLine + 1;
    const size_t newCount = newLastLine - firstLine + 1;
    if (oldCount == newCount) {
        for (size_t i = firstLine; i <= newLastLine; i++) {
            const long before = static_cast<long>(lines_[i].breaks.size());
            WrapOne(buffer, i);
            Add(i, static_cast<long>(lines_[i].breaks.size()) - before);
        }
        return;
    }
    if (newCount > oldCount) {
        lines_.insert(lines_.begin() + static_cast<long>(firstLine + oldCount), newCount - oldCount, Line{});
    } else {
        lines_.erase(lines_.begin() + static_cast<long>(firstLine + newCount),
            lines_.begin() + static_cast<long>(firstLine + oldCount));
    }
    for (size_t i = firstLine; i <= newLastLine; i++) WrapOne(buffer, i);
    Rebuild();
}

size_t WrapLayout::FirstRowOf(size_t line) const {
    size_t sum = 0;
    for (size_t i = std::min(line, lines_.size()); i > 0; i -= i & (~i + 1)) sum += tree_[i];
    return sum;
}

void WrapLayout::LineOfRow(size_t row, size_t& line, size_t& rowInLine) const {
    if (lines_.empty()) {
        line = rowInLine = 0;
        return;
    }
    if (row >= totalRows_) {
        line = lines_.size() - 1;
        rowInLine = lines_[line].breaks.size();
        return;
    }
    // The largest prefix of lines whose rows sum to at most `row`.
    size_t pos = 0;
    size_t step = 1;
    while (step * 2 < tree_.size()) step *= 2;
    size_t remaining = row;
    for (; step > 0; step /= 2) {
        if (pos + step < tree_.size() && tree_[pos + step] <= remaining) {
            pos += step;
            remaining -= tree_[pos];
        }
    }
    line = pos;
    rowInLine = remaining;
}

size_t WrapLayout::RowStart(const TextBuffer& buffer, size_t line, size_t r) const {
    const size_t start = buffer.LineStart(line);
    return r == 0 ? start : start + lines_[line].breaks[r - 1];
}

size_t WrapLayout::RowEnd(const TextBuffer& buffer, size_t line, size_t r) const {
    const auto& breaks = lines_[line].breaks;
    return r < breaks.size() ? buffer.LineStart(line) + breaks[r] : buffer.LineEnd(line);
}

size_t WrapLayout::RowOfPos(const TextBuffer& buffer, size_t line, size_t pos, bool atRowEnd) const {
    const auto& breaks = lines_[line].breaks;
    const uint32_t rel = static_cast<uint32_t>(pos - buffer.LineStart(line));
    size_t r = static_cast<size_t>(std::upper_bound(breaks.begin(), breaks.end(), rel) - breaks.begin());
    if (atRowEnd && r > 0 && breaks[r - 1] == rel) r--;
    return r;
}

} // namespace rgui::ui
