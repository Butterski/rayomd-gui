// SPDX-License-Identifier: Apache-2.0
// The editor's Markdown helpers (ui/markdownedit.h) on any text: the headings a preview build
// collects stay within their caps; every line's prefix, continuation, indentation and completion
// trigger give edits inside the bytes they read; and Enter and Tab through a TextEditor, all
// undone, give the text back.
#include "harness.h"

#include "ui/markdownedit.h"
#include "ui/texteditor.h"

#include <string>
#include <string_view>

namespace rgui::fuzz {

namespace {

namespace md = ui::markdown;

// Lines and keys tried through the editor; the text it gets.
constexpr size_t kEditorLines = 64;
constexpr size_t kEditorBytes = 16384;

void CheckEdit(const md::TextEdit& edit, size_t lineSize, const char* what) {
    if (edit.pos > lineSize || edit.len > lineSize - edit.pos) Fail(what);
    if (edit.caret > edit.text.size() + lineSize) Fail(what);
}

} // namespace

int FuzzMarkdown(const uint8_t* data, size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    const std::vector<std::string> headings = md::DocumentHeadings(text);
    if (headings.size() > md::kMaxHeadings) Fail("too many headings");
    size_t total = 0;
    for (const std::string& heading : headings) {
        if (heading.empty() || heading.size() > md::kMaxHeadingBytes) Fail("a heading's length");
        if (heading.find('\n') != std::string::npos) Fail("a heading spans lines");
        total += heading.size();
    }
    if (total > md::kMaxHeadingTotal) Fail("headings over their total");

    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view whole = text.substr(start, end - start);
        const std::string_view line = whole.substr(0, md::kLinePrefixBytes);
        const md::LinePrefix prefix = md::ParseLinePrefix(line);
        if (prefix.quoteEnd > prefix.markerEnd || prefix.markerEnd > line.size()) Fail("prefix bounds");
        for (const size_t caret : {prefix.markerEnd, line.size()}) {
            md::TextEdit edit;
            if (md::ContinueOnEnter(line, whole.size() == line.size(), caret, edit)) CheckEdit(edit, line.size(), "enter");
            if (md::IndentListItem(line, caret, false, edit)) CheckEdit(edit, line.size(), "indent");
            if (md::IndentListItem(line, caret, true, edit)) CheckEdit(edit, line.size(), "outdent");
        }
        const md::Completion kind = md::CompletionTrigger(line, true);
        if (kind != md::Completion::None && md::TriggerText(kind).size() > line.size()) Fail("trigger bounds");
        (void)md::FilterValid(md::Completion::Fence, line);
        (void)md::MatchRank(line, text.substr(0, 8));
        start = end + 1;
    }

    // Enter, Tab and Shift+Tab at line ends, then every edit undone: the text as it was.
    ui::TextEditor editor;
    editor.SetText(text.substr(0, kEditorBytes));
    const std::string before = editor.CopyText();
    size_t tried = 0;
    for (size_t line = 0; line < editor.Buffer().LineCount() && tried < kEditorLines; line++, tried++) {
        const size_t at = editor.Buffer().LineEnd(line);
        editor.Select(at, at);
        const Key key = line % 3 == 0 ? Key::Enter : Key::Tab;
        const uint8_t mods = line % 3 == 2 ? kModShift : 0;
        editor.HandleKey(key, mods, nullptr);
    }
    while (editor.Undo()) {
    }
    if (editor.CopyText() != before) Fail("undo does not give the text back");
    return 0;
}

} // namespace rgui::fuzz
