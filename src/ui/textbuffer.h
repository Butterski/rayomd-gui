// SPDX-License-Identifier: Apache-2.0
// The editor's text: UTF-8 in a gap buffer, with the byte offset where every line starts.
// Lines end at '\n' only (the app turns CRLF into LF on load; the editor normalises what it
// inserts). Positions are byte offsets into the text, the gap excluded.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::ui {

class TextBuffer {
public:
    TextBuffer() = default;

    // Replaces the whole text (taking `text`'s storage when it can); the gap goes to the end.
    void SetText(std::string text);

    size_t Size() const { return data_.size() - (gapEnd_ - gapStart_); }
    bool Empty() const { return Size() == 0; }
    char At(size_t pos) const { return pos < gapStart_ ? data_[pos] : data_[pos + (gapEnd_ - gapStart_)]; }

    // Inserts `bytes` at `pos` (clamped to the size).
    void Insert(size_t pos, std::string_view bytes);
    // Erases up to `len` bytes from `pos`.
    void Erase(size_t pos, size_t len);

    std::string CopyText() const { return Copy(0, Size()); }
    std::string Copy(size_t pos, size_t len) const;
    // The bytes [pos, pos + len) (clamped): in place when they do not straddle the gap, else
    // copied into `scratch`. The view lives until the next edit (or the next use of scratch).
    std::string_view Range(size_t pos, size_t len, std::string& scratch) const;
    // The bytes [pos, pos + len) (clamped) in place, without copying: `head` before the gap and
    // `tail` after it (`tail` is empty when the range does not straddle the gap). Valid until
    // the next edit.
    void Pieces(size_t pos, size_t len, std::string_view& head, std::string_view& tail) const;
    // The whole text in place, moving the gap to the end first (a memmove of the bytes after
    // the gap). Valid until the next edit.
    std::string_view View();

    size_t LineCount() const { return lineStarts_.size(); }
    size_t LineStart(size_t line) const { return StartOf(line); }
    // The end of `line`, before its '\n' (or the end of the text for the last line).
    size_t LineEnd(size_t line) const {
        return line + 1 < lineStarts_.size() ? StartOf(line + 1) - 1 : Size();
    }
    // The line holding byte `pos` (a '\n' belongs to the line it ends).
    size_t LineOf(size_t pos) const;
    // The bytes the line index holds (capacity included): 4 per line.
    size_t LineIndexBytes() const { return lineStarts_.capacity() * sizeof(uint32_t); }

    // The code point starting at `pos` and its length; a byte that does not start well-formed
    // UTF-8 decodes as U+FFFD of length 1. `pos` must be < Size().
    uint32_t Decode(size_t pos, size_t& len) const;
    // The start of the code point before `pos` (0 at 0), and after the one at `pos`.
    size_t Prev(size_t pos) const;
    size_t Next(size_t pos) const;

private:
    void MoveGap(size_t pos);
    void Reserve(size_t extra);
    uint32_t StartOf(size_t line) const { return lineStarts_[line] + (line >= shiftFrom_ ? shift_ : 0); }
    // The first line whose start is above `pos`.
    size_t FirstLineAfter(size_t pos) const;
    // Applies the pending shift to the entries between shiftFrom_ and `line`, so it holds from
    // `line` on.
    void MoveShift(size_t line);

    std::vector<char> data_;
    size_t gapStart_ = 0;
    size_t gapEnd_ = 0;
    // Line starts. The entries from shiftFrom_ on lack shift_ (modulo 2^32): an edit moves the
    // starts after it by changing shift_, touching only the entries between its line and the
    // previous edit's, so typing costs O(1) in the number of lines, not O(lines below).
    std::vector<uint32_t> lineStarts_{0};
    size_t shiftFrom_ = 1;
    uint32_t shift_ = 0;
};

// `text` with CRLF and lone CR turned into LF and bytes that are not well-formed UTF-8 into
// U+FFFD: what the editor inserts.
std::string NormalizeEditorText(std::string_view text);

} // namespace rgui::ui
