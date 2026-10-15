// SPDX-License-Identifier: Apache-2.0
#include "ui/textwrap.h"

#include "common/text_utils.h"
#include "text/unicode.h"

#include <algorithm>

namespace rgui::ui {

namespace {

constexpr size_t kBlockShift = 6;   // 64 lines per block of the row tree
constexpr size_t kBlockLines = size_t(1) << kBlockShift;
constexpr size_t kSpanHeader = 2;   // capacity, count
constexpr uint32_t kMaxCells = 0x7FFFFFFFu;

// The code point at s[i] and its length; malformed bytes are U+FFFD of length 1.
uint32_t DecodeAt(std::string_view s, size_t i, size_t& len) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        len = 1;
        return c;
    }
    uint32_t cp = 0;
    if (RayoMd::Text::DecodeUtf8(s, i, cp, len)) return cp;
    len = 1;
    return 0xFFFD;
}

// A line's bytes in place: the part before the buffer's gap and the part after it.
struct LineBytes {
    std::string_view head, tail;

    size_t Size() const { return head.size() + tail.size(); }
    uint32_t Decode(size_t i, size_t& len) const {
        if (i >= head.size()) return DecodeAt(tail, i - head.size(), len);
        if (static_cast<unsigned char>(head[i]) < 0x80 || head.size() - i >= 4 || tail.empty()) {
            return DecodeAt(head, i, len);
        }
        // A sequence that may straddle the gap.
        char bytes[4];
        size_t n = 0;
        for (size_t k = i; k < Size() && n < 4; k++) bytes[n++] = k < head.size() ? head[k] : tail[k - head.size()];
        return DecodeAt(std::string_view(bytes, n), 0, len);
    }
};

LineBytes BytesOf(const TextBuffer& buffer, size_t line) {
    LineBytes bytes;
    const size_t start = buffer.LineStart(line);
    buffer.Pieces(start, buffer.LineEnd(line) - start, bytes.head, bytes.tail);
    return bytes;
}

// Wraps a line's bytes from `begin`, where a row starts, to the line's end. Every break (the
// offset in the line where a later row starts) goes to `onBreak`, which returns true to stop
// there. Returns the cells of the last row: with `begin` 0 and no break, the line's cells.
//
// A row's end depends only on the text from its start up to the start of the row after the
// next one at most (the code point that did not fit lies before it), and the state at a row's
// start is always the same (column 0, no space seen): OnEdit relies on both.
template <class OnBreak>
uint32_t WrapRows(const LineBytes& line, size_t begin, int wrapCells, OnBreak&& onBreak) {
    const size_t npos = static_cast<size_t>(-1);
    const size_t size = line.Size();
    CellCursor cursor;
    size_t rowStart = begin;
    size_t lastBreak = npos;   // just after the last space of the current row
    for (size_t i = begin; i < size;) {
        size_t len;
        const uint32_t cp = line.Decode(i, len);
        const int w = cursor.Width(cp);
        const bool space = cp == ' ' || cp == '\t';
        size_t brk = npos;
        if (i - rowStart >= kMaxRowBytes) {
            brk = i;
        } else if (!space && w > 0 && cursor.col > 0 && cursor.col + w > wrapCells) {
            brk = lastBreak != npos ? lastBreak : i;
        }
        if (brk != npos) {
            if (onBreak(brk)) return 0;
            cursor = CellCursor{};
            rowStart = i = brk;
            lastBreak = npos;
            continue;
        }
        cursor.Advance(w);
        i += len;
        if (space) lastBreak = i;
    }
    return static_cast<uint32_t>(std::min<long>(cursor.col, kMaxCells));
}

} // namespace

int CellCursor::Width(uint32_t cp) const {
    if (cp == '\t') return kTabCells - col % kTabCells;
    if (cp < 0x80) return 1;
    const int w = text::CellWidth(cp);
    return w == 0 && zeroRun >= kMaxZeroWidthRun ? 1 : w;
}

uint32_t WrapLine(std::string_view text, int wrapCells, std::vector<uint32_t>& breaks) {
    breaks.clear();
    const LineBytes line{text, {}};
    const uint32_t cells = WrapRows(line, 0, std::max(1, wrapCells), [&](size_t brk) {
        breaks.push_back(static_cast<uint32_t>(brk));
        return false;
    });
    if (breaks.empty()) return cells;
    CellCursor cursor;
    for (size_t i = 0; i < text.size();) {
        size_t len;
        cursor.Advance(cursor.Width(line.Decode(i, len)));
        i += len;
    }
    return static_cast<uint32_t>(cursor.col);
}

// ---- Line storage ---------------------------------------------------------------------------

const uint32_t* WrapLayout::Breaks(size_t line, size_t& count) const {
    const uint32_t info = info_[line];
    if (!(info & kWrapped)) {
        count = 0;
        return nullptr;
    }
    const uint32_t* span = arena_.data() + (info & ~kWrapped);
    count = span[1];
    return span + kSpanHeader;
}

void WrapLayout::Release(size_t line) {
    const uint32_t info = info_[line];
    if (info & kWrapped) live_ -= kSpanHeader + arena_[(info & ~kWrapped) + 1];
    info_[line] = 0;
}

void WrapLayout::SetLine(size_t line, const uint32_t* breaks, size_t count, uint32_t cells, bool exact) {
    if (count == 0) {
        Release(line);
        info_[line] = std::min(cells, kMaxCells);
        return;
    }
    const uint32_t info = info_[line];
    if (info & kWrapped) {
        uint32_t* span = arena_.data() + (info & ~kWrapped);
        if (span[0] >= count) {
            live_ = live_ - span[1] + count;
            span[1] = static_cast<uint32_t>(count);
            std::copy(breaks, breaks + count, span + kSpanHeader);
            return;
        }
        Release(line);
    }
    const size_t capacity = exact ? count : count + count / 8 + 4;
    // Spans are indexed by 31 bits (8 GiB of breaks, past any text TextBuffer's 32-bit offsets
    // hold); should the arena still get there, the line stays one row rather than corrupt the
    // index.
    if (arena_.size() + kSpanHeader + capacity > kMaxCells) Compact();
    if (arena_.size() + kSpanHeader + capacity > kMaxCells) {
        info_[line] = kMaxCells;
        return;
    }
    const size_t index = arena_.size();
    arena_.resize(index + kSpanHeader + capacity);
    uint32_t* span = arena_.data() + index;
    span[0] = static_cast<uint32_t>(capacity);
    span[1] = static_cast<uint32_t>(count);
    std::copy(breaks, breaks + count, span + kSpanHeader);
    live_ += kSpanHeader + count;
    info_[line] = kWrapped | static_cast<uint32_t>(index);
}

void WrapLayout::MaybeCompact() {
    // Freed spans and slack stay below the live spans plus 256 KiB.
    if (arena_.size() > 2 * live_ + 65536) Compact();
}

void WrapLayout::Compact() {
    std::vector<uint32_t> packed;
    packed.reserve(live_);
    for (size_t line = 0; line < info_.size(); line++) {
        size_t count;
        const uint32_t* breaks = Breaks(line, count);
        if (count == 0) continue;
        const size_t index = packed.size();
        packed.push_back(static_cast<uint32_t>(count));
        packed.push_back(static_cast<uint32_t>(count));
        packed.insert(packed.end(), breaks, breaks + count);
        info_[line] = kWrapped | static_cast<uint32_t>(index);
    }
    arena_.swap(packed);
}

void WrapLayout::WrapWhole(const TextBuffer& buffer, size_t line, std::vector<uint32_t>& breaks) {
    breaks.clear();
    const size_t start = buffer.LineStart(line);
    uint32_t cells = 0;
    if (buffer.LineEnd(line) > start) {
        cells = WrapRows(BytesOf(buffer, line), 0, wrapCells_, [&](size_t brk) {
            breaks.push_back(static_cast<uint32_t>(brk));
            return false;
        });
    }
    SetLine(line, breaks.data(), breaks.size(), cells, true);
}

// ---- Rows -----------------------------------------------------------------------------------

void WrapLayout::RebuildFrom(size_t line) {
    const size_t lines = info_.size();
    const size_t blocks = (lines + kBlockLines - 1) >> kBlockShift;
    blockRows_.resize(blocks);
    for (size_t b = line >> kBlockShift; b < blocks; b++) {
        const size_t end = std::min(lines, (b + 1) << kBlockShift);
        size_t rows = 0;
        for (size_t l = b << kBlockShift; l < end; l++) rows += RowsOf(l);
        blockRows_[b] = static_cast<uint32_t>(rows);
    }
    tree_.assign(blocks + 1, 0);
    totalRows_ = 0;
    for (size_t i = 1; i <= blocks; i++) {
        totalRows_ += blockRows_[i - 1];
        tree_[i] += blockRows_[i - 1];
        const size_t parent = i + (i & (~i + 1));
        if (parent <= blocks) tree_[parent] += tree_[i];
    }
}

void WrapLayout::Add(size_t line, long delta) {
    if (delta == 0) return;
    totalRows_ = static_cast<size_t>(static_cast<long>(totalRows_) + delta);
    const size_t block = line >> kBlockShift;
    blockRows_[block] = static_cast<uint32_t>(static_cast<long>(blockRows_[block]) + delta);
    for (size_t i = block + 1; i < tree_.size(); i += i & (~i + 1)) {
        tree_[i] = static_cast<uint32_t>(static_cast<long>(tree_[i]) + delta);
    }
}

// ---- Wrapping -------------------------------------------------------------------------------

void WrapLayout::Reset(const TextBuffer& buffer, int wrapCells) {
    wrapCells_ = std::max(1, wrapCells);
    std::vector<uint32_t>(buffer.LineCount(), 0).swap(info_);
    std::vector<uint32_t>().swap(arena_);
    std::vector<uint32_t>().swap(blockRows_);
    std::vector<uint32_t>().swap(tree_);
    live_ = 0;
    pendingLine_ = kNone;
    std::vector<uint32_t> breaks;
    for (size_t i = 0; i < info_.size(); i++) {
        // An empty line is 0 cells and no break, as info_ already says.
        if (buffer.LineEnd(i) > buffer.LineStart(i)) WrapWhole(buffer, i, breaks);
    }
    arena_.shrink_to_fit();
    RebuildFrom(0);
}

void WrapLayout::SetWrapCells(const TextBuffer& buffer, int wrapCells) {
    wrapCells = std::max(1, wrapCells);
    if (info_.size() != buffer.LineCount()) {
        Reset(buffer, wrapCells);
        return;
    }
    if (wrapCells == wrapCells_) return;
    const uint32_t narrower = static_cast<uint32_t>(std::min(wrapCells, wrapCells_));
    wrapCells_ = wrapCells;
    std::vector<uint32_t> breaks;
    for (size_t i = 0; i < info_.size(); i++) {
        const uint32_t info = info_[i];
        if ((info & kWrapped) || info > narrower) WrapWhole(buffer, i, breaks);
    }
    pendingLine_ = kNone;   // it wraps, so it was rewrapped whole
    MaybeCompact();
    RebuildFrom(0);
}

void WrapLayout::Splice(size_t line, size_t keep, const std::vector<uint32_t>& fresh, size_t from, uint32_t shift,
    uint32_t cells) {
    size_t count;
    const uint32_t* old = Breaks(line, count);
    const size_t tail = from < count ? count - from : 0;
    const size_t total = keep + fresh.size() + tail;
    const uint32_t info = info_[line];
    if (total > 0 && count > 0 && arena_[info & ~kWrapped] >= total) {
        uint32_t* span = arena_.data() + (info & ~kWrapped);
        uint32_t* breaks = span + kSpanHeader;
        const size_t to = keep + fresh.size();
        if (to < from) {
            for (size_t k = 0; k < tail; k++) breaks[to + k] = breaks[from + k] + shift;
        } else if (to > from) {
            for (size_t k = tail; k > 0; k--) breaks[to + k - 1] = breaks[from + k - 1] + shift;
        } else {
            for (size_t k = 0; k < tail; k++) breaks[to + k] += shift;
        }
        std::copy(fresh.begin(), fresh.end(), breaks + keep);
        live_ = live_ - count + total;
        span[1] = static_cast<uint32_t>(total);
    } else {
        std::vector<uint32_t> all;
        all.reserve(total);
        all.assign(old, old + keep);
        all.insert(all.end(), fresh.begin(), fresh.end());
        for (size_t k = from; k < count; k++) all.push_back(old[k] + shift);
        SetLine(line, all.data(), all.size(), cells, false);
    }
    Add(line, static_cast<long>(total) - static_cast<long>(count));
}

void WrapLayout::EditLine(const TextBuffer& buffer, const Edit& edit) {
    const size_t line = edit.firstLine;
    const size_t start = buffer.LineStart(line);
    const bool pending = pendingLine_ == line;
    // Only one line may hold cuts: with another one pending, this edit is not cut.
    const size_t budget = pendingLine_ == kNone || pending ? editBudget_ : kNone;
    size_t count;
    const uint32_t* old = Breaks(line, count);
    // The rows before the one ahead of the edited row stay; wrapping restarts there (see
    // WrapRows) and stops at the first break that is an old one moved by the edit, or at the
    // first break past the budget (a cut: the old rows after it stay).
    const size_t rel = edit.pos - start;
    const size_t row = static_cast<size_t>(std::upper_bound(old, old + count, static_cast<uint32_t>(rel)) - old);
    const size_t keep = row >= 2 ? row - 2 : 0;
    const size_t begin = keep > 0 ? old[keep - 1] : 0;
    const size_t editEnd = rel + edit.inserted;
    size_t next = static_cast<size_t>(
        std::lower_bound(old, old + count, static_cast<uint32_t>(rel + edit.removed)) - old);
    std::vector<uint32_t> fresh;
    bool synced = false, cut = false;
    uint32_t cells = 0;
    if (buffer.LineEnd(line) > start) {
        cells = WrapRows(BytesOf(buffer, line), begin, wrapCells_, [&](size_t brk) {
            fresh.push_back(static_cast<uint32_t>(brk));
            if (brk < editEnd) return false;
            const size_t was = brk + edit.removed - edit.inserted;
            while (next < count && old[next] < was) next++;
            synced = next < count && old[next] == was;
            // The row at a cut ends at the old break after it: within one old row of text.
            cut = !synced && brk - editEnd >= budget;
            return synced || cut;
        });
    }
    // Where the line's cuts may lie now (see pendingFirst_), before the old breaks move.
    size_t first = kNone, last = 0;
    if (pending) {
        if (pendingFirst_ < begin) {
            first = pendingFirst_;
            last = std::min(pendingLast_, begin);
        }
        // Old cuts past the stop move with the rows after it.
        if ((synced || cut) && pendingLast_ >= fresh.back() + edit.removed - edit.inserted) {
            first = std::min<size_t>(first, fresh.back());
            last = std::max(last, pendingLast_ + edit.inserted - edit.removed);
        }
    }
    if (cut) {
        first = std::min<size_t>(first, fresh.back());
        last = std::max<size_t>(last, fresh.back());
    }
    Splice(line, keep, fresh, synced ? next + 1 : cut ? next : count,
        static_cast<uint32_t>(edit.inserted - edit.removed), cells);
    if (first != kNone) {
        pendingLine_ = line;
        pendingFirst_ = first;
        pendingLast_ = last;
    } else if (pending) {
        pendingLine_ = kNone;
    }
    MaybeCompact();
}

bool WrapLayout::Settle(const TextBuffer& buffer, size_t bytes) {
    if (pendingLine_ == kNone) return false;
    const size_t line = pendingLine_;
    const size_t begin = pendingFirst_;   // a row start, with no cut before it
    size_t count;
    const uint32_t* old = Breaks(line, count);
    const size_t keep = static_cast<size_t>(std::upper_bound(old, old + count, static_cast<uint32_t>(begin)) - old);
    size_t next = keep;
    std::vector<uint32_t> fresh;
    bool synced = false, cut = false;
    uint32_t cells = 0;
    if (buffer.LineEnd(line) > buffer.LineStart(line)) {
        cells = WrapRows(BytesOf(buffer, line), begin, wrapCells_, [&](size_t brk) {
            fresh.push_back(static_cast<uint32_t>(brk));
            while (next < count && old[next] < brk) next++;
            // Past the last cut, every row is what wrapping from its start makes: a break in
            // common means the rest is as a full wrap makes it.
            synced = brk > pendingLast_ && next < count && old[next] == brk;
            cut = !synced && brk - begin >= bytes;
            return synced || cut;
        });
    }
    size_t from = count;
    if (synced || cut) from = next < count && old[next] == fresh.back() ? next + 1 : next;
    Splice(line, keep, fresh, from, 0, cells);
    if (cut) {
        pendingFirst_ = fresh.back();
        pendingLast_ = std::max<size_t>(pendingLast_, fresh.back());
    } else {
        pendingLine_ = kNone;
    }
    MaybeCompact();
    return Pending();
}

void WrapLayout::OnEdit(const TextBuffer& buffer, const Edit& edit) {
    const size_t first = edit.firstLine;
    const size_t oldCount = edit.oldLastLine - first + 1;
    const size_t newCount = edit.newLastLine - first + 1;
    if (edit.oldLastLine == first && edit.newLastLine == first) {
        EditLine(buffer, edit);
        return;
    }

    // The pending line, when the edit replaces it, is rewrapped from before its first cut and
    // lines up with its old breaks only past its last one; it is then as a full wrap makes it.
    const size_t touched = pendingLine_ != kNone && pendingLine_ >= first && pendingLine_ <= edit.oldLastLine
        ? pendingLine_ : kNone;
    if (touched != kNone) pendingLine_ = kNone;
    else if (pendingLine_ != kNone && pendingLine_ > edit.oldLastLine) pendingLine_ = pendingLine_ + newCount - oldCount;

    // The first line's rows before the one ahead of the edited row stay; wrapping restarts at
    // the start of that row (see WrapRows).
    std::vector<uint32_t> breaks;
    size_t begin = 0;
    {
        size_t count;
        const uint32_t* old = Breaks(first, count);
        const uint32_t rel = static_cast<uint32_t>(edit.pos - buffer.LineStart(first));
        const size_t row = static_cast<size_t>(std::upper_bound(old, old + count, rel) - old);
        size_t keep = row >= 2 ? row - 2 : 0;
        if (touched == first) {
            const uint32_t cut = static_cast<uint32_t>(pendingFirst_);
            keep = std::min(keep, static_cast<size_t>(std::upper_bound(old, old + count, cut) - old));
        }
        if (keep > 0) {
            breaks.assign(old, old + keep);
            begin = old[keep - 1];
        }
    }
    // The old last line's breaks after the edit, as new absolute offsets: once wrapping breaks
    // at one of them again, the rest follow as they were.
    std::vector<uint32_t> after;
    {
        size_t count;
        const uint32_t* old = Breaks(edit.oldLastLine, count);
        const size_t editEnd = edit.pos + edit.removed;
        size_t from = editEnd - edit.oldLastStart;
        if (touched == edit.oldLastLine) from = std::max(from, pendingLast_ + 1);
        const uint32_t fromRel = static_cast<uint32_t>(std::min<size_t>(from, kMaxCells));
        for (const uint32_t* b = std::lower_bound(old, old + count, fromRel); b != old + count; ++b) {
            after.push_back(static_cast<uint32_t>(edit.oldLastStart + *b - edit.removed + edit.inserted));
        }
    }

    if (newCount < oldCount) {
        for (size_t l = first + newCount; l < first + oldCount; l++) Release(l);
        info_.erase(info_.begin() + static_cast<long>(first + newCount), info_.begin() + static_cast<long>(first + oldCount));
    } else if (newCount > oldCount) {
        info_.insert(info_.begin() + static_cast<long>(first + oldCount), newCount - oldCount, 0u);
    }

    const bool sameCount = newCount == oldCount;
    const size_t editEndNew = edit.pos + edit.inserted;
    size_t next = 0;   // the first candidate in `after` not yet passed
    for (size_t line = first; line <= edit.newLastLine; line++) {
        const size_t start = buffer.LineStart(line);
        const bool last = line == edit.newLastLine;
        if (line != first) {
            breaks.clear();
            begin = 0;
        }
        bool synced = false;
        uint32_t cells = 0;
        if (buffer.LineEnd(line) > start) {
            cells = WrapRows(BytesOf(buffer, line), begin, wrapCells_, [&](size_t brk) {
                breaks.push_back(static_cast<uint32_t>(brk));
                const size_t abs = start + brk;
                if (!last || abs < editEndNew) return false;
                while (next < after.size() && after[next] < abs) next++;
                synced = next < after.size() && after[next] == abs;
                return synced;
            });
        }
        if (synced) {
            for (size_t k = next + 1; k < after.size(); k++) breaks.push_back(static_cast<uint32_t>(after[k] - start));
        }
        const size_t rowsBefore = sameCount ? RowsOf(line) : 0;
        SetLine(line, breaks.data(), breaks.size(), cells, false);
        if (sameCount) Add(line, static_cast<long>(RowsOf(line)) - static_cast<long>(rowsBefore));
    }
    MaybeCompact();
    if (!sameCount) RebuildFrom(first);
}

// ---- Queries --------------------------------------------------------------------------------

size_t WrapLayout::FirstRowOf(size_t line) const {
    line = std::min(line, info_.size());
    const size_t block = line >> kBlockShift;
    size_t sum = 0;
    for (size_t i = block; i > 0; i -= i & (~i + 1)) sum += tree_[i];
    for (size_t l = block << kBlockShift; l < line; l++) sum += RowsOf(l);
    return sum;
}

void WrapLayout::LineOfRow(size_t row, size_t& line, size_t& rowInLine) const {
    if (info_.empty()) {
        line = rowInLine = 0;
        return;
    }
    if (row >= totalRows_) {
        line = info_.size() - 1;
        rowInLine = RowsOf(line) - 1;
        return;
    }
    // The largest prefix of blocks whose rows sum to at most `row`, then the lines of the next.
    size_t block = 0;
    size_t step = 1;
    while (step * 2 < tree_.size()) step *= 2;
    size_t remaining = row;
    for (; step > 0; step /= 2) {
        if (block + step < tree_.size() && tree_[block + step] <= remaining) {
            block += step;
            remaining -= tree_[block];
        }
    }
    size_t l = block << kBlockShift;
    for (size_t rows = RowsOf(l); remaining >= rows; rows = RowsOf(l)) {
        remaining -= rows;
        l++;
    }
    line = l;
    rowInLine = remaining;
}

size_t WrapLayout::RowStart(const TextBuffer& buffer, size_t line, size_t r) const {
    const size_t start = buffer.LineStart(line);
    if (r == 0) return start;
    size_t count;
    const uint32_t* breaks = Breaks(line, count);
    return start + breaks[r - 1];
}

size_t WrapLayout::RowEnd(const TextBuffer& buffer, size_t line, size_t r) const {
    size_t count;
    const uint32_t* breaks = Breaks(line, count);
    return r < count ? buffer.LineStart(line) + breaks[r] : buffer.LineEnd(line);
}

size_t WrapLayout::RowOfPos(const TextBuffer& buffer, size_t line, size_t pos, bool atRowEnd) const {
    size_t count;
    const uint32_t* breaks = Breaks(line, count);
    const uint32_t rel = static_cast<uint32_t>(pos - buffer.LineStart(line));
    size_t r = static_cast<size_t>(std::upper_bound(breaks, breaks + count, rel) - breaks);
    if (atRowEnd && r > 0 && breaks[r - 1] == rel) r--;
    return r;
}

size_t WrapLayout::MemoryBytes() const {
    return (info_.capacity() + arena_.capacity() + blockRows_.capacity() + tree_.capacity()) * sizeof(uint32_t);
}

} // namespace rgui::ui
