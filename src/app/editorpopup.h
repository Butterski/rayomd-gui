// SPDX-License-Identifier: Apache-2.0
// The editor's popup list at the caret (ui::PopupList), while the editor keeps the focus: the
// insert list (Ctrl+I, the footer's button: page break, table of contents, table, callout with its
// types, Mermaid flowchart and sequence diagram, math block, code block, front matter, rule) and
// the completions that open as Markdown is typed: fence names after ```, alert types after `> [!`
// and, when the caller turns wiki links on, the document's headings after `[[`.
//
// The open list takes Up, Down, Enter and Escape before the editor's frame; the insert list also
// takes the typed text as its filter. A completion filters by the text typed after its trigger,
// which the editor gets as usual; Enter picks only once something was typed or chosen with Up or
// Down, else it stays a line break. A completion without matches hides until the filter matches
// again. Every insertion is one undo group (TextEditor::Replace). Each frame reads at most a
// line's first kLinePrefixBytes and the filter's bytes, never the document; the headings come
// from the preview build's worker.
#pragma once

#include "ui/markdownedit.h"
#include "ui/texteditor.h"
#include "ui/ui.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rgui {

class EditorPopup {
public:
    enum class Mode : uint8_t { Closed, Insert, Callout, Completion };

    // `[[` completes the document's headings (off by default; the caller turns it on).
    void SetWikiLinks(bool on) { wikiLinks_ = on; }
    bool WikiLinks() const { return wikiLinks_; }
    // The document's headings, from the last preview build.
    void SetHeadings(std::vector<std::string> headings);
    const std::vector<std::string>& Headings() const { return headings_; }

    // Opens the insert list at the caret.
    void OpenInsert();
    void Close();
    Mode CurrentMode() const { return mode_; }
    bool IsOpen() const { return mode_ != Mode::Closed; }
    ui::markdown::Completion CompletionKind() const { return kind_; }
    // The rows the list shows now (filtered).
    const std::vector<std::string>& Items() const { return items_; }

    // Before the editor's frame. `editorFocused`: the editor has the keyboard (no prompt is
    // open); without it the list closes. Ctrl+I opens the insert list.
    void BeforeEditor(ui::Context& ctx, ui::TextEditor& editor, bool editorFocused);
    // After it: a trigger typed in this frame opens its completion, the filter follows the
    // text, and the list is drawn at the caret of the editor in `editorRect`.
    // Only text the editor took while focused (`editorFocused` as for BeforeEditor) opens a
    // completion; ``` on a line inside a fenced block (md::InsideFence over the kFenceScanBytes
    // before it) opens none.
    void AfterEditor(ui::Context& ctx, ui::TextEditor& editor, ui::RectF editorRect, bool editorFocused);

private:
    void OpenCompletion(ui::markdown::Completion kind, size_t start, ui::TextEditor& editor, bool fenceTemplate);
    // The completion's typed filter; false when the text no longer belongs to it.
    bool CompletionFilter(const ui::TextEditor& editor, std::string& filter) const;
    void Filter(const std::string& filter);
    void Pick(int row, ui::TextEditor& editor);
    void PickInsert(size_t entry, ui::TextEditor& editor);
    void PickCompletion(const std::string& item, ui::TextEditor& editor);
    // Puts `block` at the caret on lines of its own (markdown::PlaceBlock) as one undo group.
    static void InsertBlock(ui::TextEditor& editor, std::string_view block, size_t caretInBlock, bool caretAfter);
    static void InsertFrontMatter(ui::TextEditor& editor);
    void Reset(ui::Context& ctx);

    Mode mode_ = Mode::Closed;
    bool wikiLinks_ = false;
    bool reset_ = false;            // the list's highlight goes back to its first row
    bool handled_ = false;          // this frame the list took text or keys that changed things
    uint64_t revisionBefore_ = 0;   // the editor's revision before its frame
    std::vector<std::string> headings_;
    // The candidates and, after the filter, the rows and their candidates' indices.
    std::vector<std::string> all_;
    std::vector<std::string> items_;
    std::vector<size_t> rows_;
    std::string filter_;            // the insert list's typed filter; a completion's last filter
    // The completion: its kind, where its filter starts, and how a fence name goes in.
    ui::markdown::Completion kind_ = ui::markdown::Completion::None;
    size_t start_ = 0;
    bool closeFence_ = false;       // a typed ``` at a line's end: the closing fence comes too
    bool fenceTemplate_ = false;    // the insert list's code block: the caret goes into it
};

} // namespace rgui
