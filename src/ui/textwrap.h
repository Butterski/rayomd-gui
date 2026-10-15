// SPDX-License-Identifier: Apache-2.0
// Soft wrapping for the text editor, in cells (text::CellWidth: 0, 1 or 2 per code point; a tab
// advances to the next multiple of 4). A row breaks after the last space that fits; a word
// longer than the row is cut at the row width; spaces past the edge hang on their row. Every
// line keeps its breaks; a Fenwick tree over the rows per line maps visual rows to lines in
// O(log n).
#pragma once

#include "ui/textbuffer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::ui {

constexpr int kTabCells = 4;

// The cells `cp` takes at column `col`.
int CellsAt(uint32_t cp, int col);

// Wraps one line (without its '\n') at `wrapCells` (>= 1): the byte offsets where its second
// and later rows start go to `breaks` (cleared first). Returns the line's cells unwrapped.
uint32_t WrapLine(std::string_view line, int wrapCells, std::vector<uint32_t>& breaks);

class WrapLayout {
public:
    // Wraps every line of `buffer` at `wrapCells` (at least 1).
    void Reset(const TextBuffer& buffer, int wrapCells);
    // A new width for the same text: rewraps only the lines that can change, those wider than
    // the narrower of the two widths.
    void SetWrapCells(const TextBuffer& buffer, int wrapCells);
    int WrapCells() const { return wrapCells_; }

    // After an edit that replaced the old lines [firstLine, oldLastLine] by the lines
    // [firstLine, newLastLine] of `buffer`: rewraps those lines only.
    void OnEdit(const TextBuffer& buffer, size_t firstLine, size_t oldLastLine, size_t newLastLine);

    size_t LineCount() const { return lines_.size(); }
    size_t RowCount() const { return totalRows_; }
    size_t RowsOf(size_t line) const { return lines_[line].breaks.size() + 1; }
    // The visual row of the first row of `line`.
    size_t FirstRowOf(size_t line) const;
    // The line holding visual row `row` (clamped to the last row) and the row within it.
    void LineOfRow(size_t row, size_t& line, size_t& rowInLine) const;

    // The bytes of row `r` of `line`, as absolute offsets [start, end); end excludes the '\n'.
    size_t RowStart(const TextBuffer& buffer, size_t line, size_t r) const;
    size_t RowEnd(const TextBuffer& buffer, size_t line, size_t r) const;
    // The row of `line` showing `pos`. A position at a soft break shows at the end of the row
    // before when `atRowEnd`, else at the start of the next one.
    size_t RowOfPos(const TextBuffer& buffer, size_t line, size_t pos, bool atRowEnd) const;

private:
    struct Line {
        uint32_t cells = 0;
        std::vector<uint32_t> breaks;   // relative to the line start
    };
    void WrapOne(const TextBuffer& buffer, size_t index);
    void Rebuild();
    void Add(size_t line, long delta);

    int wrapCells_ = 0;
    std::vector<Line> lines_;
    std::vector<uint32_t> tree_;   // Fenwick tree of rows per line, 1-based
    size_t totalRows_ = 0;
    std::string scratch_;
};

} // namespace rgui::ui
