// SPDX-License-Identifier: Apache-2.0
#include "app/editorpopup.h"

#include "ui/textbuffer.h"
#include "ui/widgets.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <utility>

namespace rgui {

namespace {

namespace md = ui::markdown;

constexpr const char* kPopupId = "##editorPopup";

// An entry of the insert list: `block` goes on lines of its own with the caret at `caret` in it
// (kBeforeClose: at the end of the line before the block's last), or below it with `after`.
constexpr size_t kBeforeClose = static_cast<size_t>(-1);
struct InsertEntry {
    const char* label;
    const char* block;
    size_t caret;
    bool after;
};
const InsertEntry kInsertEntries[] = {
    {"Page break", "\\pagebreak", 0, true},
    {"Table of contents", "[TOC]", 0, true},
    {"Table", "|   |   |   |\n|---|---|---|\n|   |   |   |\n|   |   |   |", 2, false},
    {"Callout\xE2\x80\xA6", nullptr, 0, false},
    {"Mermaid flowchart",
     "```mermaid\nflowchart TD\n    A[Start] --> B{Choice}\n    B -->|Yes| C[Done]\n    B -->|No| A\n```", kBeforeClose,
     false},
    {"Mermaid sequence diagram", "```mermaid\nsequenceDiagram\n    Alice->>Bob: Hello\n    Bob-->>Alice: Hi\n```",
     kBeforeClose, false},
    {"Math block", "$$\n\n$$", 3, false},
    {"Code block", "```\n\n```", 3, false},
    {"Front matter", nullptr, 0, false},
    // `***`, not `---`: on the first line `---` would open front matter.
    {"Horizontal rule", "***", 0, true},
};
constexpr size_t kCalloutEntry = 3;
constexpr size_t kCodeBlockEntry = 7;
constexpr size_t kFrontMatterEntry = 8;

const char* const kCalloutLabels[] = {"Note", "Tip", "Important", "Warning", "Caution"};

// Whether the line starting at `lineStart` lies in a fenced code block (so a fence typed there
// closes it or is code): md::InsideFence over at most kFenceScanBytes before the line, from the
// first whole line in them. Past that window the scan starts as if outside a block.
bool InFence(const ui::TextBuffer& buffer, size_t lineStart) {
    const size_t from = lineStart > md::kFenceScanBytes ? lineStart - md::kFenceScanBytes : 0;
    std::string before = buffer.Copy(from, lineStart - from);
    if (from > 0) {
        const size_t nl = before.find('\n');
        before.erase(0, nl == std::string::npos ? before.size() : nl + 1);
    }
    return md::InsideFence(before);
}

// Whether [from, to) holds text other than blanks; past kLinePrefixBytes it counts as text.
bool TextIn(const ui::TextBuffer& buffer, size_t from, size_t to) {
    if (to <= from) return false;
    if (to - from > md::kLinePrefixBytes) return true;
    return md::HasText(buffer.Copy(from, to - from));
}

std::string Today() {
    char stamp[16] = "";
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    const bool haveTime = localtime_s(&local, &now) == 0;
#else
    const bool haveTime = localtime_r(&now, &local) != nullptr;
#endif
    if (haveTime) std::strftime(stamp, sizeof stamp, "%Y-%m-%d", &local);
    return stamp;
}

} // namespace

void EditorPopup::SetHeadings(std::vector<std::string> headings) {
    headings_ = std::move(headings);
    if (mode_ == Mode::Completion && kind_ == md::Completion::WikiLink) {
        all_ = headings_;
        Filter(filter_);
    }
}

void EditorPopup::OpenInsert() {
    mode_ = Mode::Insert;
    kind_ = md::Completion::None;
    all_.clear();
    for (const InsertEntry& entry : kInsertEntries) all_.emplace_back(entry.label);
    filter_.clear();
    Filter(filter_);
    reset_ = true;
}

void EditorPopup::Close() {
    mode_ = Mode::Closed;
    kind_ = md::Completion::None;
    all_.clear();
    items_.clear();
    rows_.clear();
    filter_.clear();
}

void EditorPopup::Reset(ui::Context& ctx) {
    if (!reset_) return;
    ui::ResetPopupList(ctx, kPopupId);
    reset_ = false;
}

void EditorPopup::Filter(const std::string& filter) {
    items_.clear();
    rows_.clear();
    // Exact matches first, then those that start with the filter, then the rest; in list order.
    for (int rank = 0; rank <= 2; rank++) {
        for (size_t i = 0; i < all_.size(); i++) {
            if (md::MatchRank(all_[i], filter) != rank) continue;
            items_.push_back(all_[i]);
            rows_.push_back(i);
        }
    }
}

void EditorPopup::BeforeEditor(ui::Context& ctx, ui::TextEditor& editor, bool editorFocused) {
    revisionBefore_ = editor.Revision();
    handled_ = false;
    if (!editorFocused) {
        if (IsOpen()) Close();
        return;
    }
    if (mode_ == Mode::Closed) {
        if (!ctx.ConsumeKey(Key::I, kModCtrl)) return;
        OpenInsert();
    }
    ui::PopupEvent event;
    if (mode_ == Mode::Insert || mode_ == Mode::Callout) {
        // The typed text filters the list; Backspace takes it back, and in the callout types
        // with nothing typed returns to the insert list.
        const std::vector<InputEvent>& events = ctx.Events();
        for (size_t i = 0; i < events.size(); i++) {
            if (ctx.IsConsumed(i)) continue;
            const InputEvent& e = events[i];
            if (e.type == InputEvent::Type::Text) {
                if (filter_.size() + e.text.size() <= md::kMaxFilterBytes) filter_ += e.text;
            } else if (e.type == InputEvent::Type::KeyDown && e.key == Key::Backspace && e.mods == 0) {
                if (!filter_.empty()) {
                    while (!filter_.empty() && (static_cast<unsigned char>(filter_.back()) & 0xC0) == 0x80) filter_.pop_back();
                    if (!filter_.empty()) filter_.pop_back();
                } else if (mode_ == Mode::Callout) {
                    OpenInsert();
                }
            } else {
                continue;
            }
            ctx.ConsumeEvent(i);
            handled_ = true;
            Filter(filter_);
            reset_ = true;
        }
        Reset(ctx);
        event = ui::PopupListKeys(ctx, kPopupId, static_cast<int>(items_.size()), true);
        // Enter with no row to pick does nothing behind the list.
        ctx.ConsumeKey(Key::Enter);
    } else {
        Reset(ctx);
        if (items_.empty()) return;
        event = ui::PopupListKeys(ctx, kPopupId, static_cast<int>(items_.size()), !filter_.empty());
        // The code block's fence without a language: Enter goes into the block.
        if (event.action == ui::PopupAction::None && fenceTemplate_ && ctx.ConsumeKey(Key::Enter)) {
            handled_ = true;
            const size_t caret = editor.Caret();
            Close();
            editor.Replace(caret, 0, "", 1);
            return;
        }
    }
    if (event.action != ui::PopupAction::None) handled_ = true;
    if (event.action == ui::PopupAction::Pick) Pick(event.index, editor);
    else if (event.action == ui::PopupAction::Close) Close();
}

void EditorPopup::AfterEditor(ui::Context& ctx, ui::TextEditor& editor, ui::RectF editorRect, bool editorFocused) {
    // Typing the editor took in this frame may have completed a trigger at the caret. Not text
    // the list took as its filter, nor an entry it put in, nor text another widget took.
    bool typed = false;
    for (const InputEvent& e : ctx.Events()) typed |= e.type == InputEvent::Type::Text;
    if (typed && editorFocused && !handled_ && editor.Revision() != revisionBefore_ && mode_ != Mode::Insert &&
        mode_ != Mode::Callout && !editor.HasSelection()) {
        const ui::TextBuffer& buffer = editor.Buffer();
        const size_t caret = editor.Caret();
        const size_t lineStart = buffer.LineStart(buffer.LineOf(caret));
        if (caret - lineStart <= md::kLinePrefixBytes) {
            const md::Completion kind = md::CompletionTrigger(buffer.Copy(lineStart, caret - lineStart), wikiLinks_);
            if (kind != md::Completion::None && !(kind == md::Completion::Fence && InFence(buffer, lineStart))) {
                OpenCompletion(kind, caret, editor, false);
            }
        }
    }
    if (mode_ == Mode::Completion) {
        std::string filter;
        if (!CompletionFilter(editor, filter)) {
            Close();
        } else if (filter != filter_) {
            filter_ = std::move(filter);
            Filter(filter_);
            reset_ = true;
        }
    }
    if (mode_ == Mode::Closed) return;
    Reset(ctx);
    if (mode_ == Mode::Completion && items_.empty()) return;
    const gfx::RectF caret = editor.CaretRect();
    const ui::RectF anchor{editorRect.x + caret.x, editorRect.y + caret.y, caret.w, caret.h};
    const bool insert = mode_ == Mode::Insert || mode_ == Mode::Callout;
    const ui::PopupEvent event = ui::PopupList(ctx, kPopupId, items_, anchor, insert ? "No match" : "");
    if (event.action == ui::PopupAction::Pick) {
        Pick(event.index, editor);
        ctx.RequestAnimation();
    } else if (event.action == ui::PopupAction::Close) {
        Close();
        ctx.RequestAnimation();
    }
}

void EditorPopup::OpenCompletion(md::Completion kind, size_t start, ui::TextEditor& editor, bool fenceTemplate) {
    mode_ = Mode::Completion;
    kind_ = kind;
    start_ = start;
    fenceTemplate_ = fenceTemplate;
    const ui::TextBuffer& buffer = editor.Buffer();
    closeFence_ = kind == md::Completion::Fence && !fenceTemplate && buffer.LineEnd(buffer.LineOf(start)) == start;
    switch (kind) {
    case md::Completion::Fence: all_ = md::FenceNames(); break;
    case md::Completion::Alert: all_ = md::AlertTypes(); break;
    case md::Completion::WikiLink: all_ = headings_; break;
    case md::Completion::None: all_.clear(); break;
    }
    filter_.clear();
    Filter(filter_);
    reset_ = true;
}

bool EditorPopup::CompletionFilter(const ui::TextEditor& editor, std::string& filter) const {
    const ui::TextBuffer& buffer = editor.Buffer();
    const size_t caret = editor.Caret();
    const std::string_view trigger = md::TriggerText(kind_);
    if (editor.HasSelection() || caret < start_ || caret - start_ > md::kMaxFilterBytes || start_ < trigger.size() ||
        start_ > buffer.Size()) {
        return false;
    }
    if (buffer.Copy(start_ - trigger.size(), trigger.size()) != trigger) return false;
    filter = buffer.Copy(start_, caret - start_);
    return md::FilterValid(kind_, filter);
}

void EditorPopup::Pick(int row, ui::TextEditor& editor) {
    if (row < 0 || static_cast<size_t>(row) >= rows_.size()) return;
    const size_t index = rows_[static_cast<size_t>(row)];
    switch (mode_) {
    case Mode::Insert:
        PickInsert(index, editor);
        break;
    case Mode::Callout: {
        const std::string type = md::AlertTypes()[index];
        Close();
        const std::string block = "> [!" + type + "]\n> ";
        InsertBlock(editor, block, block.size(), false);
        break;
    }
    case Mode::Completion: {
        const std::string item = all_[index];
        PickCompletion(item, editor);
        break;
    }
    case Mode::Closed:
        break;
    }
}

void EditorPopup::PickInsert(size_t entry, ui::TextEditor& editor) {
    if (entry == kCalloutEntry) {
        mode_ = Mode::Callout;
        all_.assign(std::begin(kCalloutLabels), std::end(kCalloutLabels));
        filter_.clear();
        Filter(filter_);
        reset_ = true;
        return;
    }
    Close();
    if (entry == kFrontMatterEntry) {
        InsertFrontMatter(editor);
        return;
    }
    const InsertEntry& e = kInsertEntries[entry];
    const std::string_view block = e.block;
    const size_t caret = e.caret == kBeforeClose ? block.rfind('\n') : e.caret;
    InsertBlock(editor, block, caret, e.after);
    // The code block asks for its language next.
    if (entry == kCodeBlockEntry) OpenCompletion(md::Completion::Fence, editor.Caret(), editor, true);
}

void EditorPopup::PickCompletion(const std::string& item, ui::TextEditor& editor) {
    std::string filter;
    if (!CompletionFilter(editor, filter)) {
        Close();
        return;
    }
    const ui::TextBuffer& buffer = editor.Buffer();
    const size_t caret = editor.Caret();
    const size_t lineStart = buffer.LineStart(buffer.LineOf(start_));
    const bool restEmpty = buffer.LineEnd(buffer.LineOf(caret)) == caret;
    const size_t trigger = md::TriggerText(kind_).size();
    // The line before the trigger (its container markers), read as the trigger was.
    const size_t headLength = std::min(start_ - trigger - lineStart, md::kLinePrefixBytes);
    const std::string head = buffer.Copy(lineStart, headLength);
    std::string text;
    size_t caretOffset = 0;
    switch (kind_) {
    case md::Completion::Fence:
        text = item;
        caretOffset = text.size();
        if (fenceTemplate_) {
            caretOffset = text.size() + 1;   // the empty line inside the inserted block
        } else if (closeFence_ && restEmpty) {
            // The code goes in a quote or list item as the fence does: quote markers stay,
            // list markers become spaces.
            std::string indent = head;
            for (char& c : indent) {
                if (c != '>' && c != ' ' && c != '\t') c = ' ';
            }
            text += "\n" + indent;
            caretOffset = text.size();
            text += "\n" + indent + "```";
        }
        break;
    case md::Completion::Alert:
        text = item + "]";
        if (restEmpty) text += "\n" + head;
        caretOffset = text.size();
        break;
    case md::Completion::WikiLink:
        text = "#" + item + "]]";
        caretOffset = text.size();
        break;
    case md::Completion::None:
        return;
    }
    const size_t start = start_;
    Close();
    editor.Replace(start, caret - start, text, caretOffset);
}

void EditorPopup::InsertBlock(ui::TextEditor& editor, std::string_view block, size_t caretInBlock, bool caretAfter) {
    const ui::TextBuffer& buffer = editor.Buffer();
    size_t from = editor.SelectionStart();
    size_t to = editor.SelectionEnd();
    const size_t first = buffer.LineOf(from);
    const size_t last = buffer.LineOf(to);
    const size_t lineStart = buffer.LineStart(first);
    const size_t lineEnd = buffer.LineEnd(last);
    md::BlockSite site;
    site.textBefore = TextIn(buffer, lineStart, from);
    site.textAfter = TextIn(buffer, to, lineEnd);
    site.textAbove = first > 0 && TextIn(buffer, buffer.LineStart(first - 1), buffer.LineEnd(first - 1));
    site.textBelow = last + 1 < buffer.LineCount() && TextIn(buffer, buffer.LineStart(last + 1), buffer.LineEnd(last + 1));
    site.atEnd = last + 1 == buffer.LineCount();
    // Blanks alone around the caret go: the block takes the line.
    if (!site.textBefore) from = lineStart;
    if (!site.textAfter) to = lineEnd;
    const md::TextEdit edit = md::PlaceBlock(block, caretInBlock, caretAfter, site);
    editor.Replace(from, to - from, edit.text, edit.caret);
}

void EditorPopup::InsertFrontMatter(ui::TextEditor& editor) {
    const ui::TextBuffer& buffer = editor.Buffer();
    // The engine reads front matter at the top only; a document that has one gets the caret in it.
    if (buffer.Copy(0, 4) == "---\n") {
        editor.Replace(0, 0, "", 4);
        return;
    }
    std::string text = "---\ntitle: \nauthor: \ndate: " + Today() + "\n---\n";
    if (buffer.Size() > 0 && buffer.At(0) != '\n') text += "\n";
    editor.Replace(0, 0, text, 11);   // after "title: "
}

} // namespace rgui
