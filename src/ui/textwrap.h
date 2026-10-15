// SPDX-License-Identifier: Apache-2.0
// Soft wrapping for the text editor, in cells (CellCursor). A row breaks after the last space
// that fits; a word longer than the row is cut at the row width; spaces past the edge hang on
// their row. Whatever the text, a row's work stays small: a row ends after kMaxRowBytes bytes,
// and a zero-width code point takes a cell after kMaxZeroWidthRun zero-width ones in a row.
//
// Memory per line is 4 bytes, plus 8 bytes and 4 per break for a line that wraps; rows per line
// are summed per block of 64 lines in a Fenwick tree, which maps visual rows to lines in
// O(log n + 64).
//
// An edit rewraps from the row before the edited one until a break lines up with an old one,
// which in text is a row or two. Text can be made so that it never does (a long line repeating
// a pattern as wide as a row reflows to its end on every keystroke), so an edit inside a line
// rewraps at most a budget of bytes past the edit, then keeps the old rows after that point:
// every row is still a run of the line's text no wider than kMaxRowBytes, the row at the cut
// may be too wide or too narrow, and Settle (called once per frame) rewraps the rest of that
// line a budget at a time until it is as a full wrap would make it.
#pragma once

#include "ui/textbuffer.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rgui::ui {

constexpr int kTabCells = 4;
// A zero-width code point after this many zero-width ones in a row takes a cell, so a run of
// them cannot pile up in one cell.
constexpr int kMaxZeroWidthRun = 8;
// A row ends after this many bytes, so no row is costly to lay out or draw.
constexpr size_t kMaxRowBytes = 4096;

// The cells of a row's code points, left to right from the row's start: text::CellWidth (0, 1
// or 2), a tab to the next multiple of kTabCells, and 1 for a zero-width code point after
// kMaxZeroWidthRun zero-width ones.
struct CellCursor {
    int col = 0;
    int zeroRun = 0;

    // The cells `cp` takes at the cursor.
    int Width(uint32_t cp) const;
    void Advance(int cells) {
        col += cells;
        zeroRun = cells == 0 ? zeroRun + 1 : 0;
    }
};

// Wraps one line (without its '\n') at `wrapCells` (>= 1): the byte offsets where its second
// and later rows start go to `breaks` (cleared first). Returns the line's cells unwrapped.
uint32_t WrapLine(std::string_view line, int wrapCells, std::vector<uint32_t>& breaks);

class WrapLayout {
public:
    // An edit of the buffer: [pos, pos + removed) replaced by `inserted` bytes.
    struct Edit {
        size_t pos = 0;
        size_t removed = 0;
        size_t inserted = 0;
        size_t firstLine = 0;      // the line holding pos
        size_t oldLastLine = 0;    // the line that held pos + removed before the edit
        size_t oldLastStart = 0;   // where that line started before the edit
        size_t newLastLine = 0;    // the line holding pos + inserted after it
    };

    // Wraps every line of `buffer` at `wrapCells` (at least 1).
    void Reset(const TextBuffer& buffer, int wrapCells);
    // A new width for the same text: rewraps only the lines that can change, those wider than
    // the narrower of the two widths.
    void SetWrapCells(const TextBuffer& buffer, int wrapCells);
    int WrapCells() const { return wrapCells_; }

    // After `edit` of `buffer`: rewraps the lines it touched, from the row before the edited
    // one until the rows line up with the old ones again, or, for an edit inside one line, up
    // to the edit budget past it (see above).
    void OnEdit(const TextBuffer& buffer, const Edit& edit);
    // Bytes an edit inside a line rewraps past its end before deferring the rest (tests set
    // small budgets).
    static constexpr size_t kEditBudgetBytes = 64 << 10;
    void SetEditBudget(size_t bytes) { editBudget_ = bytes; }
    // Whether a line still has rows left by a cut, and rewrapping up to `bytes` more of it;
    // returns whether rows are still left.
    bool Pending() const { return pendingLine_ != kNone; }
    bool Settle(const TextBuffer& buffer, size_t bytes);

    size_t LineCount() const { return info_.size(); }
    size_t RowCount() const { return totalRows_; }
    size_t RowsOf(size_t line) const {
        const uint32_t info = info_[line];
        return info & kWrapped ? arena_[(info & ~kWrapped) + 1] + size_t(1) : 1;
    }
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

    // The bytes the layout holds (capacities included).
    size_t MemoryBytes() const;

private:
    static constexpr uint32_t kWrapped = 0x80000000u;
    static constexpr size_t kNone = static_cast<size_t>(-1);

    // The breaks of `line` (relative to its start) and their count.
    const uint32_t* Breaks(size_t line, size_t& count) const;
    // Stores the breaks of `line`, or its cells when it has none. `exact` sizes a new span to
    // the count; otherwise it gets room to grow.
    void SetLine(size_t line, const uint32_t* breaks, size_t count, uint32_t cells, bool exact);
    void Release(size_t line);
    // Packs the spans when freed ones and slack outgrow the live ones.
    void MaybeCompact();
    void Compact();
    void WrapWhole(const TextBuffer& buffer, size_t line, std::vector<uint32_t>& breaks);
    // OnEdit for an edit inside one line: splices the line's breaks in place.
    void EditLine(const TextBuffer& buffer, const Edit& edit);
    // Replaces the breaks of `line` after the first `keep` by `fresh`, then old[from, count)
    // plus `shift` (modulo 2^32), in place when the span has room; updates the rows.
    void Splice(size_t line, size_t keep, const std::vector<uint32_t>& fresh, size_t from, uint32_t shift,
        uint32_t cells);
    // Recounts the rows of the blocks from `line`'s on and rebuilds the tree.
    void RebuildFrom(size_t line);
    void Add(size_t line, long delta);

    int wrapCells_ = 0;
    // Per line: its cells (no break), or kWrapped | the index of its span in arena_.
    std::vector<uint32_t> info_;
    // Spans of the lines that wrap: capacity, count, then `capacity` slots for the breaks.
    std::vector<uint32_t> arena_;
    size_t live_ = 0;                   // arena words of spans in use, slack excluded
    std::vector<uint32_t> blockRows_;   // rows per block of lines
    std::vector<uint32_t> tree_;        // Fenwick tree over blockRows_, 1-based
    size_t totalRows_ = 0;
    size_t editBudget_ = kEditBudgetBytes;
    // The one line whose rows may differ from a full wrap, because an edit or Settle stopped at
    // a budget ("cut") there: rows before pendingFirst_ are as a full wrap makes them, no cut
    // lies past pendingLast_ (offsets in the line), and every row other than one starting at a
    // cut is what wrapping from its own start makes.
    size_t pendingLine_ = kNone;
    size_t pendingFirst_ = 0;
    size_t pendingLast_ = 0;
};

} // namespace rgui::ui
