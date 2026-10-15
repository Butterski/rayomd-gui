// SPDX-License-Identifier: Apache-2.0
// The Markdown editor: a TextBuffer on a cell grid (the interface family, soft-wrapped), with
// caret, selection, word motions, mouse selection, clipboard, undo/redo and IME preedit. No
// highlighting: the preview shows the formatting. Metrics follow omawrite (src/Main.qml): 140 %
// line height, 24 px side margins, 16 px top margin, `# Start writing` as the placeholder.
#pragma once

#include "text/textlayout.h"
#include "ui/textbuffer.h"
#include "ui/textwrap.h"
#include "ui/ui.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::ui {

// The editor's text size ladder (times the `title` font token).
constexpr float kEditorZoomLadder[] = {0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};
constexpr int kEditorZoomCount = 8;
constexpr int kEditorZoomDefault = 2;

// Text padding inside the pane, logical px (omawrite).
constexpr float kEditorPadX = 24;
constexpr float kEditorPadTop = 16;

class TextEditor {
public:
    // Edits merge into one undo group while they follow each other within this time.
    static constexpr double kMergeMs = 1000;
    static constexpr size_t kUndoLimitBytes = size_t(64) << 20;

    TextEditor();
    TextEditor(const TextEditor&) = delete;
    TextEditor& operator=(const TextEditor&) = delete;

    // ---- The widget -------------------------------------------------------------------------
    // Runs the editor for a frame in `rect` (logical px): takes focus on a press, handles the
    // frame's keys, text, preedit, pointer and wheel while focused (Ctrl+wheel steps the text
    // size), keeps the IME's text-input rectangle on the caret and draws the visible rows.
    // Keys it does not use (the app's shortcuts) stay unconsumed. The font is ctx.Fonts() at
    // the `title` token times the zoom; colours are the tokens' derived colours.
    void Frame(Context& ctx, RectF rect);
    // The id it registers for focus and the clipboard ("##texteditor" under the id stack).
    Id WidgetId() const { return id_; }

    // The text size: an index into kEditorZoomLadder.
    int ZoomIndex() const { return zoom_; }
    void SetZoomIndex(int index);
    // The font size of the grid at the current zoom, logical px.
    static float FontSize(const Tokens& tokens, int zoomIndex);
    // Rewraps every line (the interface font changed).
    void InvalidateLayout();

    // ---- Text -------------------------------------------------------------------------------
    // Replaces the text (CRLF and CR become LF, malformed UTF-8 U+FFFD); clears undo, the
    // selection and the scroll. Does not count as a change (no OnChanged).
    void SetText(std::string_view text);
    // The text in place (moves the buffer's gap; valid until the next edit).
    std::string_view Text() { return buffer_.View(); }
    std::string CopyText() const { return buffer_.CopyText(); }
    const TextBuffer& Buffer() const { return buffer_; }
    // Counts every change of the text (edits, undo, redo; not SetText).
    uint64_t Revision() const { return revision_; }
    // Called after every change of the text.
    void SetOnChanged(std::function<void()> callback) { onChanged_ = std::move(callback); }

    // ---- Geometry (logical px) --------------------------------------------------------------
    // The pane's size and the grid: the column width (the advance of 'M') and the line height.
    // Rewraps at floor((width - 48) / columnWidth) cells when that changes.
    void SetLayout(float paneWidth, float paneHeight, float columnWidth, float lineHeight);
    float ColumnWidth() const { return colWidth_; }
    float LineHeight() const { return lineHeight_; }
    int WrapCells() const { return wrap_.WrapCells(); }
    const WrapLayout& Wrap() const { return wrap_; }
    // Rows that fit the pane below the top padding (at least 1).
    int VisibleRows() const;
    // Scroll offset of the text, logical px, and its range.
    float ScrollY() const { return scrollY_; }
    float MaxScrollY() const;
    void SetScrollY(float y);

    // The clock for undo merging and auto-scroll, ms; the frame sets it.
    void SetNow(double ms) { now_ = ms; }

    // ---- Caret and selection ----------------------------------------------------------------
    size_t Caret() const { return caret_; }
    size_t Anchor() const { return anchor_; }
    bool HasSelection() const { return caret_ != anchor_; }
    size_t SelectionStart() const { return caret_ < anchor_ ? caret_ : anchor_; }
    size_t SelectionEnd() const { return caret_ < anchor_ ? anchor_ : caret_; }
    std::string SelectedText() const { return buffer_.Copy(SelectionStart(), SelectionEnd() - SelectionStart()); }
    // Selects [anchor, caret) (clamped to code point boundaries); scrolls to the caret.
    void Select(size_t anchor, size_t caret);
    // The caret's visual row and its column in cells.
    size_t CaretRow() const;
    int CaretColumn() const;
    // The caret rectangle in pane coordinates (logical px): 1 px wide, a line high.
    gfx::RectF CaretRect() const;

    // ---- Input ------------------------------------------------------------------------------
    // A key press; false when the editor does not use it (the app's shortcuts). `ctx` serves
    // the clipboard (Context::SetClipboardText, RequestClipboard; a paste arrives in a later
    // Frame) and may be null (no clipboard).
    bool HandleKey(Key key, uint8_t mods, Context* ctx);
    // Committed text (typing, Compose, IME): replaces the selection; clears the preedit.
    void HandleText(std::string_view utf8);
    // The IME's preedit, drawn at the caret; not in the buffer.
    void SetPreedit(std::string text, int cursor);
    const std::string& Preedit() const { return preedit_; }

    // The pointer, in pane coordinates (logical px). `clicks` is 1, 2 or 3 (and on: 3).
    void PointerDown(float x, float y, int clicks, bool shift);
    void PointerDrag(float x, float y);
    void PointerUp();
    bool Dragging() const { return dragging_; }
    // While dragging outside the text area, scrolls toward the pointer (call every 16 ms);
    // true while it scrolled.
    bool AutoScroll();
    // The text position nearest a pane point.
    size_t PositionAt(float x, float y, bool* atRowEnd = nullptr) const;

    // ---- Editing ----------------------------------------------------------------------------
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    size_t UndoBytes() const { return undoBytes_; }
    size_t UndoGroups() const { return undo_.size(); }
    void SelectAll();
    void Copy(Context& ctx) const;
    void Cut(Context& ctx);
    // Asks for the clipboard; Frame inserts it when it arrives.
    void Paste(Context& ctx);
    // Inserts as one paste: its own undo group.
    void PasteText(std::string_view utf8);

    // ---- Word classes -----------------------------------------------------------------------
    // 0: a word code point (ASCII alphanumerics, '_', any code point >= U+0080 outside
    // White_Space); 1: white space; 2: anything else (punctuation); 3: '\n'.
    static int CharClass(uint32_t cp);
    // Ctrl+Left / Ctrl+Right targets from `pos`.
    size_t WordLeft(size_t pos) const;
    size_t WordRight(size_t pos) const;

private:
    struct EditRecord {
        size_t pos = 0;
        std::string removed;
        std::string inserted;
    };
    struct UndoGroup {
        std::vector<EditRecord> edits;
        size_t caretBefore = 0, anchorBefore = 0;
        size_t caretAfter = 0, anchorAfter = 0;
        size_t bytes = 0;
    };
    enum class Merge : uint8_t { None, Typing, Backspace, Delete };

    // Replaces [pos, pos + len) by `bytes` in the buffer and the wrap layout.
    void Apply(size_t pos, size_t len, std::string_view bytes);
    // A recorded edit: replaces [pos, pos + len) by `bytes`, puts the caret after it.
    void Edit(size_t pos, size_t len, std::string_view bytes, Merge merge);
    void Changed();
    void PushGroup(UndoGroup group);
    void TrimUndo();
    void EndGroup() { groupOpen_ = false; }

    // Moves the caret (keeping the anchor when `extend`), ends the undo group, scrolls.
    void MoveTo(size_t pos, bool extend, bool atRowEnd = false, bool keepDesired = false);
    void MoveVertical(long rows, bool extend);
    void EnsureCaretVisible();
    void ClampScroll();

    // The cells of [rowStart, pos) on one row.
    int ColumnOf(size_t rowStart, size_t pos) const;
    // The position on visual row `row` nearest column `col` (cells, fractional).
    size_t PositionInRow(size_t row, float col, bool* atRowEnd) const;
    void WordAt(size_t pos, size_t& start, size_t& end) const;
    void LineAt(size_t pos, size_t& start, size_t& end) const;
    void ExtendDragTo(size_t pos, bool atRowEnd);

    // Frame helpers.
    void HandleInput(Context& ctx, RectF rect, bool focused);
    void Draw(Context& ctx, RectF rect, bool focused);
    void DrawRow(Context& ctx, RectF rect, size_t row, float y, bool focused);

    TextBuffer buffer_;
    WrapLayout wrap_;
    uint64_t revision_ = 0;
    std::function<void()> onChanged_;

    float paneWidth_ = 0, paneHeight_ = 0;
    float colWidth_ = 8, lineHeight_ = 20;
    float scrollY_ = 0;
    double now_ = 0;

    size_t caret_ = 0, anchor_ = 0;
    bool atRowEnd_ = false;
    float desiredCol_ = -1;

    std::string preedit_;
    int preeditCursor_ = -1;

    bool dragging_ = false;
    int dragGranularity_ = 1;   // 1 characters, 2 words, 3 lines
    size_t dragOriginStart_ = 0, dragOriginEnd_ = 0;
    float dragX_ = 0, dragY_ = 0;

    std::deque<UndoGroup> undo_;
    std::vector<UndoGroup> redo_;
    size_t undoBytes_ = 0;
    bool groupOpen_ = false;
    Merge lastMerge_ = Merge::None;
    double lastEditTime_ = 0;

    // The widget.
    Id id_ = 0;
    int zoom_ = kEditorZoomDefault;
    float fontSize_ = 14;
    float ascent_ = 0, descent_ = 0;
    SmoothScroll scroll_;
    float zoomWheel_ = 0;
    double lastAutoScroll_ = 0;
    // The glyphs of a row being drawn, normal and selected.
    std::vector<GlyphInstance> glyphs_[2];
    std::string rowScratch_;
};

} // namespace rgui::ui
