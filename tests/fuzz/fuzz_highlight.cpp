// SPDX-License-Identifier: Apache-2.0
// The editor's Markdown syntax colours (ui/markdownhighlight.h) on any text and any edits: the
// input up to its first NUL is the text, the bytes after it edits (position, removed bytes,
// inserted bytes, a scan budget), each followed by a scan of that budget. Settled, every line's
// state and runs must be a fresh scan's, the runs well formed and within their caps, and a view's
// runs the line's own.
#include "harness.h"

#include "ui/markdownhighlight.h"
#include "ui/textbuffer.h"
#include "ui/textwrap.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::fuzz {

namespace {

using ui::MarkdownHighlighter;
using ui::SyntaxRun;

constexpr size_t kMaxTextBytes = 64 * 1024;
constexpr size_t kMaxEdits = 64;

ui::WrapLayout::Edit Apply(ui::TextBuffer& buffer, size_t pos, size_t len, std::string_view bytes) {
    ui::WrapLayout::Edit edit;
    edit.pos = std::min(pos, buffer.Size());
    edit.removed = std::min(len, buffer.Size() - edit.pos);
    edit.inserted = bytes.size();
    edit.firstLine = buffer.LineOf(edit.pos);
    edit.oldLastLine = buffer.LineOf(edit.pos + edit.removed);
    edit.oldLastStart = buffer.LineStart(edit.oldLastLine);
    buffer.Erase(edit.pos, edit.removed);
    buffer.Insert(edit.pos, bytes);
    edit.newLastLine = edit.firstLine + static_cast<size_t>(std::count(bytes.begin(), bytes.end(), '\n'));
    return edit;
}

void Settle(MarkdownHighlighter& h, const ui::TextBuffer& buffer) {
    while (h.Settle(buffer, MarkdownHighlighter::kFrameBudget)) {
    }
}

void CheckRuns(const std::vector<SyntaxRun>& runs, size_t length) {
    if (runs.size() > MarkdownHighlighter::kMaxLineRuns) Fail("more runs than kMaxLineRuns");
    for (size_t k = 0; k < runs.size(); k++) {
        if (k == 0 ? runs[k].start != 0 : runs[k].start <= runs[k - 1].start) Fail("runs out of order");
        if (runs[k].start >= std::max<size_t>(length, 1)) Fail("a run past its line");
        if (static_cast<int>(runs[k].role) >= ui::kSyntaxRoleCount) Fail("a role out of range");
    }
}

bool SameRuns(const std::vector<SyntaxRun>& a, const SyntaxRun* b, size_t count) {
    if (a.size() != count) return false;
    for (size_t k = 0; k < count; k++) {
        if (a[k].start != b[k].start || a[k].role != b[k].role) return false;
    }
    return true;
}

} // namespace

int FuzzHighlight(const uint8_t* data, size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const size_t nul = input.find('\0');
    const std::string_view text = input.substr(0, std::min({nul, input.size(), kMaxTextBytes}));
    std::string_view script = nul == std::string_view::npos ? std::string_view() : input.substr(nul + 1);
    const bool wiki = !script.empty() && (script[0] & 1);

    ui::TextBuffer buffer;
    buffer.SetText(std::string(text));
    MarkdownHighlighter h;
    h.SetWikiLinks(wiki);
    h.Reset(buffer);
    for (size_t edits = 0; script.size() >= 5 && edits < kMaxEdits; edits++) {
        const auto byte = [&](size_t i) { return static_cast<uint8_t>(script[i]); };
        const size_t pos = buffer.Size() ? (static_cast<size_t>(byte(0)) << 8 | byte(1)) % (buffer.Size() + 1) : 0;
        const size_t removed = byte(2) % 64;
        const size_t insertLength = std::min<size_t>(byte(3) % 32, script.size() - 5);
        const size_t budget = 1 + static_cast<size_t>(byte(4)) * 8;
        const std::string_view insert = script.substr(5, insertLength);
        script.remove_prefix(5 + insertLength);
        if (buffer.Size() - std::min(removed, buffer.Size()) + insert.size() > kMaxTextBytes) break;
        h.OnEdit(buffer, Apply(buffer, pos, removed, insert));
        h.Settle(buffer, budget);
        if (budget & 8) {
            const size_t first = pos % buffer.LineCount();
            h.PrepareView(buffer, first, first + 30, budget * 16);
        }
    }
    Settle(h, buffer);

    MarkdownHighlighter fresh;
    fresh.SetWikiLinks(wiki);
    fresh.Reset(buffer);
    Settle(fresh, buffer);
    std::vector<SyntaxRun> a, b;
    const size_t lines = buffer.LineCount();
    for (size_t line = 0; line <= lines; line++) {
        if (h.StateOf(line) != fresh.StateOf(line)) Fail("an incremental state differs from a fresh scan's");
        if (line == lines) break;
        if (!h.LineRuns(buffer, line, a) || !fresh.LineRuns(buffer, line, b)) Fail("a settled line is unknown");
        const size_t length = buffer.LineEnd(line) - buffer.LineStart(line);
        CheckRuns(a, length);
        if (!SameRuns(a, b.data(), b.size())) Fail("incremental runs differ from a fresh scan's");
    }
    // A view: each line's runs are its own, or none for lines past the budget.
    const size_t first = lines / 2;
    h.PrepareView(buffer, first, first + 40, MarkdownHighlighter::kViewBudget);
    for (size_t line = first; line < std::min(lines, first + 41); line++) {
        size_t count = 0;
        const SyntaxRun* runs = h.Runs(line, count);
        if (!runs) continue;
        fresh.LineRuns(buffer, line, b);
        if (!SameRuns(b, runs, count)) Fail("a view's runs differ from the line's");
    }
    return 0;
}

} // namespace rgui::fuzz
