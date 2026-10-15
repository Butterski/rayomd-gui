// SPDX-License-Identifier: Apache-2.0
#include "ui/texteditor.h"

#include "text/unicode.h"

#include <algorithm>

namespace rgui::ui {

namespace {

constexpr int kDefaultWrapCells = 80;

} // namespace

TextEditor::TextEditor() : self_(std::make_shared<TextEditor*>(this)) {
    wrap_.Reset(buffer_, kDefaultWrapCells);
}

// ---- Text -----------------------------------------------------------------------------------

void TextEditor::SetText(std::string_view text) {
    buffer_.SetText(NormalizeEditorText(text));
    wrap_.Reset(buffer_, wrap_.WrapCells());
    caret_ = anchor_ = 0;
    atRowEnd_ = false;
    desiredCol_ = -1;
    preedit_.clear();
    preeditCursor_ = -1;
    dragging_ = false;
    undo_.clear();
    redo_.clear();
    undoBytes_ = 0;
    groupOpen_ = false;
    lastMerge_ = Merge::None;
    scrollY_ = 0;
}

void TextEditor::Apply(size_t pos, size_t len, std::string_view bytes) {
    const size_t firstLine = buffer_.LineOf(pos);
    const size_t oldLastLine = buffer_.LineOf(pos + len);
    buffer_.Erase(pos, len);
    buffer_.Insert(pos, bytes);
    const size_t newLastLine = firstLine + static_cast<size_t>(std::count(bytes.begin(), bytes.end(), '\n'));
    wrap_.OnEdit(buffer_, firstLine, oldLastLine, newLastLine);
}

void TextEditor::Changed() {
    revision_++;
    if (onChanged_) onChanged_();
}

void TextEditor::Edit(size_t pos, size_t len, std::string_view bytes, Merge merge) {
    if (len == 0 && bytes.empty()) return;
    std::string removed = buffer_.Copy(pos, len);
    const size_t caretBefore = caret_;
    const size_t anchorBefore = anchor_;
    Apply(pos, len, bytes);
    const size_t after = pos + bytes.size();

    for (const UndoGroup& group : redo_) undoBytes_ -= group.bytes;
    redo_.clear();

    bool merged = false;
    const bool mergeable = groupOpen_ && merge != Merge::None && merge == lastMerge_ && !undo_.empty() &&
        now_ - lastEditTime_ < kMergeMs;
    if (mergeable) {
        UndoGroup& group = undo_.back();
        EditRecord& last = group.edits.back();
        size_t added = 0;
        if (merge == Merge::Typing && len == 0 && pos == last.pos + last.inserted.size()) {
            last.inserted.append(bytes.data(), bytes.size());
            added = bytes.size();
            merged = true;
        } else if (merge == Merge::Backspace && bytes.empty() && last.inserted.empty() && pos + len == last.pos) {
            last.removed.insert(0, removed);
            last.pos = pos;
            added = removed.size();
            merged = true;
        } else if (merge == Merge::Delete && bytes.empty() && last.inserted.empty() && pos == last.pos) {
            last.removed += removed;
            added = removed.size();
            merged = true;
        }
        if (merged) {
            group.caretAfter = group.anchorAfter = after;
            group.bytes += added;
            undoBytes_ += added;
        }
    }
    if (!merged) {
        UndoGroup group;
        group.caretBefore = caretBefore;
        group.anchorBefore = anchorBefore;
        group.caretAfter = group.anchorAfter = after;
        group.bytes = removed.size() + bytes.size() + sizeof(EditRecord);
        group.edits.push_back(EditRecord{pos, std::move(removed), std::string(bytes)});
        PushGroup(std::move(group));
    }
    TrimUndo();

    caret_ = anchor_ = after;
    atRowEnd_ = false;
    desiredCol_ = -1;
    groupOpen_ = merge != Merge::None;
    lastMerge_ = merge;
    lastEditTime_ = now_;
    Changed();
    EnsureCaretVisible();
}

void TextEditor::PushGroup(UndoGroup group) {
    undoBytes_ += group.bytes;
    undo_.push_back(std::move(group));
}

void TextEditor::TrimUndo() {
    // The newest group stays even when it alone passes the limit.
    while (undoBytes_ > kUndoLimitBytes && undo_.size() > 1) {
        undoBytes_ -= undo_.front().bytes;
        undo_.pop_front();
    }
}

bool TextEditor::Undo() {
    if (undo_.empty()) return false;
    UndoGroup group = std::move(undo_.back());
    undo_.pop_back();
    for (auto it = group.edits.rbegin(); it != group.edits.rend(); ++it) {
        Apply(it->pos, it->inserted.size(), it->removed);
    }
    caret_ = std::min(group.caretBefore, buffer_.Size());
    anchor_ = std::min(group.anchorBefore, buffer_.Size());
    redo_.push_back(std::move(group));
    atRowEnd_ = false;
    desiredCol_ = -1;
    EndGroup();
    Changed();
    EnsureCaretVisible();
    return true;
}

bool TextEditor::Redo() {
    if (redo_.empty()) return false;
    UndoGroup group = std::move(redo_.back());
    redo_.pop_back();
    for (const EditRecord& edit : group.edits) Apply(edit.pos, edit.removed.size(), edit.inserted);
    caret_ = std::min(group.caretAfter, buffer_.Size());
    anchor_ = std::min(group.anchorAfter, buffer_.Size());
    undo_.push_back(std::move(group));
    atRowEnd_ = false;
    desiredCol_ = -1;
    EndGroup();
    Changed();
    EnsureCaretVisible();
    return true;
}

// ---- Geometry -------------------------------------------------------------------------------

void TextEditor::SetLayout(float paneWidth, float paneHeight, float columnWidth, float lineHeight) {
    paneWidth_ = paneWidth;
    paneHeight_ = paneHeight;
    colWidth_ = columnWidth > 0 ? columnWidth : 1;
    lineHeight_ = lineHeight > 0 ? lineHeight : 1;
    const float usable = paneWidth - 2 * kEditorPadX;
    const int cells = usable > colWidth_ ? static_cast<int>(usable / colWidth_) : 1;
    wrap_.SetWrapCells(buffer_, cells);
    ClampScroll();
}

int TextEditor::VisibleRows() const {
    const float h = paneHeight_ - kEditorPadTop;
    const int rows = h > 0 ? static_cast<int>(h / lineHeight_) : 0;
    return std::max(1, rows);
}

float TextEditor::MaxScrollY() const {
    // Room for one row below the last, so the caret keeps its margin there too.
    const float content = static_cast<float>(wrap_.RowCount() + 1) * lineHeight_ + kEditorPadTop;
    return std::max(0.0f, content - paneHeight_);
}

void TextEditor::ClampScroll() {
    scrollY_ = std::min(std::max(scrollY_, 0.0f), MaxScrollY());
}

void TextEditor::SetScrollY(float y) {
    scrollY_ = y;
    ClampScroll();
}

void TextEditor::EnsureCaretVisible() {
    if (paneHeight_ <= 0) return;
    const float top = static_cast<float>(CaretRow()) * lineHeight_;
    const float view = paneHeight_ - kEditorPadTop;
    if (top - lineHeight_ < scrollY_) {
        scrollY_ = top - lineHeight_;
    } else if (top + 2 * lineHeight_ > scrollY_ + view) {
        scrollY_ = top + 2 * lineHeight_ - view;
    }
    ClampScroll();
}

// ---- Caret ----------------------------------------------------------------------------------

int TextEditor::ColumnOf(size_t rowStart, size_t pos) const {
    int col = 0;
    for (size_t p = rowStart; p < pos;) {
        size_t len;
        const uint32_t cp = buffer_.Decode(p, len);
        col += CellsAt(cp, col);
        p += len;
    }
    return col;
}

size_t TextEditor::CaretRow() const {
    const size_t line = buffer_.LineOf(caret_);
    return wrap_.FirstRowOf(line) + wrap_.RowOfPos(buffer_, line, caret_, atRowEnd_);
}

int TextEditor::CaretColumn() const {
    const size_t line = buffer_.LineOf(caret_);
    const size_t r = wrap_.RowOfPos(buffer_, line, caret_, atRowEnd_);
    return ColumnOf(wrap_.RowStart(buffer_, line, r), caret_);
}

gfx::RectF TextEditor::CaretRect() const {
    float x = kEditorPadX + static_cast<float>(CaretColumn()) * colWidth_;
    x = std::min(x, std::max(kEditorPadX, paneWidth_ - kEditorPadX));
    const float y = kEditorPadTop + static_cast<float>(CaretRow()) * lineHeight_ - scrollY_;
    return {x, y, 1, lineHeight_};
}

size_t TextEditor::PositionInRow(size_t row, float col, bool* atRowEnd) const {
    size_t line, r;
    wrap_.LineOfRow(row, line, r);
    const size_t start = wrap_.RowStart(buffer_, line, r);
    const size_t end = wrap_.RowEnd(buffer_, line, r);
    if (atRowEnd) *atRowEnd = false;
    int c = 0;
    for (size_t p = start; p < end;) {
        size_t len;
        const uint32_t cp = buffer_.Decode(p, len);
        const int w = CellsAt(cp, c);
        if (col < static_cast<float>(c) + static_cast<float>(w) * 0.5f) return p;
        c += w;
        p += len;
    }
    if (atRowEnd && r + 1 < wrap_.RowsOf(line)) *atRowEnd = true;
    return end;
}

size_t TextEditor::PositionAt(float x, float y, bool* atRowEnd) const {
    const float rowF = (y - kEditorPadTop + scrollY_) / lineHeight_;
    size_t row = 0;
    if (rowF > 0) row = std::min(static_cast<size_t>(rowF), wrap_.RowCount() - 1);
    return PositionInRow(row, (x - kEditorPadX) / colWidth_, atRowEnd);
}

void TextEditor::MoveTo(size_t pos, bool extend, bool atRowEnd, bool keepDesired) {
    caret_ = std::min(pos, buffer_.Size());
    if (!extend) anchor_ = caret_;
    atRowEnd_ = atRowEnd;
    if (!keepDesired) desiredCol_ = -1;
    EndGroup();
    EnsureCaretVisible();
}

void TextEditor::Select(size_t anchor, size_t caret) {
    anchor_ = std::min(anchor, buffer_.Size());
    MoveTo(caret, true);
}

void TextEditor::SelectAll() {
    anchor_ = 0;
    MoveTo(buffer_.Size(), true);
}

void TextEditor::MoveVertical(long rows, bool extend) {
    if (desiredCol_ < 0) desiredCol_ = static_cast<float>(CaretColumn());
    const long target = static_cast<long>(CaretRow()) + rows;
    if (target < 0) {
        MoveTo(0, extend, false, true);
    } else if (target >= static_cast<long>(wrap_.RowCount())) {
        MoveTo(buffer_.Size(), extend, false, true);
    } else {
        bool end = false;
        const size_t pos = PositionInRow(static_cast<size_t>(target), desiredCol_, &end);
        MoveTo(pos, extend, end, true);
    }
}

// ---- Words ----------------------------------------------------------------------------------

int TextEditor::CharClass(uint32_t cp) {
    if (cp == '\n') return 3;
    if (cp < 0x80) {
        if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_') return 0;
        if (cp == ' ' || cp == '\t' || cp == '\v' || cp == '\f' || cp == '\r') return 1;
        return 2;
    }
    return text::IsWhiteSpace(cp) ? 1 : 0;
}

size_t TextEditor::WordLeft(size_t pos) const {
    size_t len;
    while (pos > 0) {
        const size_t prev = buffer_.Prev(pos);
        if (CharClass(buffer_.Decode(prev, len)) == 0) break;
        pos = prev;
    }
    while (pos > 0) {
        const size_t prev = buffer_.Prev(pos);
        if (CharClass(buffer_.Decode(prev, len)) != 0) break;
        pos = prev;
    }
    return pos;
}

size_t TextEditor::WordRight(size_t pos) const {
    const size_t size = buffer_.Size();
    size_t len;
    while (pos < size && CharClass(buffer_.Decode(pos, len)) != 0) pos += len;
    while (pos < size && CharClass(buffer_.Decode(pos, len)) == 0) pos += len;
    return pos;
}

void TextEditor::WordAt(size_t pos, size_t& start, size_t& end) const {
    const size_t size = buffer_.Size();
    size_t len;
    // The code point under the pointer; at a line's end, the one before it.
    size_t at = pos;
    if (at >= size || buffer_.Decode(at, len) == '\n') {
        if (at == 0) {
            start = end = pos;
            return;
        }
        at = buffer_.Prev(at);
    }
    const int cls = CharClass(buffer_.Decode(at, len));
    if (cls == 3) {
        start = end = pos;
        return;
    }
    start = at;
    while (start > 0) {
        const size_t prev = buffer_.Prev(start);
        if (CharClass(buffer_.Decode(prev, len)) != cls) break;
        start = prev;
    }
    end = at;
    while (end < size && CharClass(buffer_.Decode(end, len)) == cls) end += len;
}

void TextEditor::LineAt(size_t pos, size_t& start, size_t& end) const {
    const size_t line = buffer_.LineOf(pos);
    start = buffer_.LineStart(line);
    end = line + 1 < buffer_.LineCount() ? buffer_.LineStart(line + 1) : buffer_.Size();
}

// ---- Keys and text --------------------------------------------------------------------------

bool TextEditor::HandleKey(Key key, uint8_t mods, Platform* platform) {
    if (mods & kModSuper) return false;
    const bool alt = (mods & kModAlt) != 0;
    // A Ctrl shortcut never fires with Alt held (AltGr is Ctrl+Alt on Windows).
    const bool ctrl = (mods & kModCtrl) != 0;
    if (alt) return false;
    const bool shift = (mods & kModShift) != 0;
    const size_t size = buffer_.Size();

    switch (key) {
    case Key::Left:
        if (ctrl) MoveTo(WordLeft(caret_), shift);
        else if (!shift && HasSelection()) MoveTo(SelectionStart(), false);
        else MoveTo(buffer_.Prev(caret_), shift);
        return true;
    case Key::Right:
        if (ctrl) MoveTo(WordRight(caret_), shift);
        else if (!shift && HasSelection()) MoveTo(SelectionEnd(), false);
        else MoveTo(buffer_.Next(caret_), shift);
        return true;
    case Key::Up:
        if (ctrl) return false;
        MoveVertical(-1, shift);
        return true;
    case Key::Down:
        if (ctrl) return false;
        MoveVertical(1, shift);
        return true;
    case Key::Home:
        if (ctrl) {
            MoveTo(0, shift);
        } else {
            const size_t line = buffer_.LineOf(caret_);
            MoveTo(wrap_.RowStart(buffer_, line, wrap_.RowOfPos(buffer_, line, caret_, atRowEnd_)), shift);
        }
        return true;
    case Key::End:
        if (ctrl) {
            MoveTo(size, shift);
        } else {
            const size_t line = buffer_.LineOf(caret_);
            const size_t r = wrap_.RowOfPos(buffer_, line, caret_, atRowEnd_);
            MoveTo(wrap_.RowEnd(buffer_, line, r), shift, r + 1 < wrap_.RowsOf(line));
        }
        return true;
    case Key::PageUp:
    case Key::PageDown: {
        if (ctrl) return false;
        const long rows = std::max(1, VisibleRows() - 1);
        const long dir = key == Key::PageUp ? -1 : 1;
        scrollY_ += static_cast<float>(dir * rows) * lineHeight_;
        ClampScroll();
        MoveVertical(dir * rows, shift);
        return true;
    }
    case Key::Backspace:
        if (HasSelection()) Edit(SelectionStart(), SelectionEnd() - SelectionStart(), {}, Merge::None);
        else if (ctrl) Edit(WordLeft(caret_), caret_ - WordLeft(caret_), {}, Merge::None);
        else if (caret_ > 0) Edit(buffer_.Prev(caret_), caret_ - buffer_.Prev(caret_), {}, Merge::Backspace);
        return true;
    case Key::Delete:
        if (HasSelection()) Edit(SelectionStart(), SelectionEnd() - SelectionStart(), {}, Merge::None);
        else if (ctrl) Edit(caret_, WordRight(caret_) - caret_, {}, Merge::None);
        else if (caret_ < size) Edit(caret_, buffer_.Next(caret_) - caret_, {}, Merge::Delete);
        return true;
    case Key::Enter:
        if (ctrl) return false;
        HandleText("\n");
        return true;
    case Key::Tab:
        if (ctrl || shift) return false;
        HandleText("\t");
        return true;
    case Key::Insert:
        if (shift && !ctrl) {
            if (platform) Paste(*platform);
            return true;
        }
        return false;
    default:
        break;
    }

    if (!ctrl) return false;
    switch (key) {
    case Key::A:
        if (shift) return false;
        SelectAll();
        return true;
    case Key::C:
        if (shift) return false;
        if (platform) Copy(*platform);
        return true;
    case Key::X:
        if (shift) return false;
        if (platform) Cut(*platform);
        return true;
    case Key::V:
        if (shift) return false;
        if (platform) Paste(*platform);
        return true;
    case Key::Z:
        if (shift) Redo();
        else Undo();
        return true;
    case Key::Y:
        if (shift) return false;
        Redo();
        return true;
    default:
        return false;
    }
}

void TextEditor::HandleText(std::string_view utf8) {
    std::string text = NormalizeEditorText(utf8);
    // Control characters other than LF and tab are not text.
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return (u < 0x20 && c != '\n' && c != '\t') || u == 0x7F;
    }), text.end());
    preedit_.clear();
    preeditCursor_ = -1;
    if (text.empty()) return;
    Edit(SelectionStart(), SelectionEnd() - SelectionStart(), text, Merge::Typing);
}

void TextEditor::SetPreedit(std::string text, int cursor) {
    preedit_ = std::move(text);
    preeditCursor_ = cursor;
}

void TextEditor::PasteText(std::string_view utf8) {
    const std::string text = NormalizeEditorText(utf8);
    if (text.empty() && !HasSelection()) return;
    EndGroup();
    Edit(SelectionStart(), SelectionEnd() - SelectionStart(), text, Merge::None);
}

void TextEditor::Copy(Platform& platform) const {
    if (HasSelection()) platform.SetClipboardText(SelectedText());
}

void TextEditor::Cut(Platform& platform) {
    if (!HasSelection()) return;
    platform.SetClipboardText(SelectedText());
    EndGroup();
    Edit(SelectionStart(), SelectionEnd() - SelectionStart(), {}, Merge::None);
}

void TextEditor::Paste(Platform& platform) {
    std::weak_ptr<TextEditor*> self = self_;
    platform.RequestClipboardText([self](std::string text) {
        if (auto editor = self.lock()) (*editor)->PasteText(text);
    });
}

// ---- Pointer --------------------------------------------------------------------------------

void TextEditor::PointerDown(float x, float y, int clicks, bool shift) {
    bool end = false;
    const size_t pos = PositionAt(x, y, &end);
    clicks = std::min(std::max(clicks, 1), 3);
    dragging_ = true;
    dragX_ = x;
    dragY_ = y;
    desiredCol_ = -1;
    EndGroup();
    if (shift && clicks == 1) {
        dragGranularity_ = 1;
        dragOriginStart_ = dragOriginEnd_ = anchor_;
        caret_ = pos;
        atRowEnd_ = end;
    } else {
        dragGranularity_ = clicks;
        size_t s = pos, e = pos;
        if (clicks == 2) WordAt(pos, s, e);
        else if (clicks == 3) LineAt(pos, s, e);
        dragOriginStart_ = s;
        dragOriginEnd_ = e;
        anchor_ = s;
        caret_ = e;
        atRowEnd_ = clicks == 1 && end;
    }
    EnsureCaretVisible();
}

void TextEditor::ExtendDragTo(size_t pos, bool atRowEnd) {
    if (dragGranularity_ == 1) {
        caret_ = pos;
        atRowEnd_ = atRowEnd;
        return;
    }
    size_t s = pos, e = pos;
    if (dragGranularity_ == 2) WordAt(pos, s, e);
    else LineAt(pos, s, e);
    atRowEnd_ = false;
    if (pos < dragOriginStart_) {
        anchor_ = dragOriginEnd_;
        caret_ = s;
    } else {
        anchor_ = dragOriginStart_;
        caret_ = std::max(e, dragOriginEnd_);
    }
}

void TextEditor::PointerDrag(float x, float y) {
    if (!dragging_) return;
    dragX_ = x;
    dragY_ = y;
    bool end = false;
    const size_t pos = PositionAt(x, y, &end);
    ExtendDragTo(pos, end);
}

void TextEditor::PointerUp() {
    dragging_ = false;
}

bool TextEditor::AutoScroll() {
    if (!dragging_) return false;
    float distance = 0;
    if (dragY_ < 0) distance = dragY_;
    else if (dragY_ > paneHeight_) distance = dragY_ - paneHeight_;
    if (distance == 0) return false;
    // Faster the farther the pointer is, from a quarter row to three rows per tick.
    const float magnitude = std::min(std::max(distance < 0 ? -distance : distance, lineHeight_ * 0.25f),
        lineHeight_ * 3);
    const float before = scrollY_;
    scrollY_ += distance < 0 ? -magnitude : magnitude;
    ClampScroll();
    bool end = false;
    const size_t pos = PositionAt(dragX_, dragY_, &end);
    ExtendDragTo(pos, end);
    return scrollY_ != before;
}

} // namespace rgui::ui
