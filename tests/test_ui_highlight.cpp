// SPDX-License-Identifier: Apache-2.0
// The editor's Markdown syntax colours (ui/markdownhighlight.h): the subset and its precedence,
// incremental scans against a fresh scan of the same text, interrupted work, limits, and the
// editor's frames (no syntax work once settled, none for a theme change or a resize).
#include "runner.h"

#include "gfx/surface.h"
#include "text/fontchoice.h"
#include "text/fontindex.h"
#include "text/glyphcache.h"
#include "ui/compositor.h"
#include "ui/icons.h"
#include "ui/markdownhighlight.h"
#include "ui/textbuffer.h"
#include "ui/texteditor.h"
#include "ui/textwrap.h"
#include "ui/theme.h"
#include "ui/tokens.h"
#include "ui/ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using rgui::ui::MarkdownHighlighter;
using rgui::ui::SyntaxRole;
using rgui::ui::SyntaxRun;
using rgui::ui::TextBuffer;
using rgui::ui::WrapLayout;

namespace {

// One letter per role: . text, m marker, h heading, c code, l link, u url, $ math, i info, ! comment.
char Letter(SyntaxRole role) {
    static const char kLetters[] = ".mhclu$i!";
    return kLetters[static_cast<int>(role)];
}

// A line's roles as letters, blanks as blanks (their role draws nothing).
std::string Letters(std::string_view line, const std::vector<SyntaxRun>& runs) {
    std::string out(line.size(), '.');
    for (size_t k = 0; k < runs.size(); k++) {
        const size_t end = k + 1 < runs.size() ? runs[k + 1].start : line.size();
        for (size_t b = runs[k].start; b < end && b < line.size(); b++) out[b] = Letter(runs[k].role);
    }
    for (size_t b = 0; b < line.size(); b++) {
        if (line[b] == ' ' || line[b] == '\t') out[b] = ' ';
    }
    return out;
}

// Every line of `text` as letters, joined by '\n', from a fresh, complete scan.
std::string Highlight(std::string_view text, bool wiki = false) {
    TextBuffer buffer;
    buffer.SetText(std::string(text));
    MarkdownHighlighter h;
    h.SetWikiLinks(wiki);
    h.Reset(buffer);
    while (h.Settle(buffer, MarkdownHighlighter::kFrameBudget)) {
    }
    std::string out;
    std::vector<SyntaxRun> runs;
    for (size_t line = 0; line < buffer.LineCount(); line++) {
        CHECK(h.LineRuns(buffer, line, runs));
        if (line > 0) out += '\n';
        const size_t start = buffer.LineStart(line);
        out += Letters(text.substr(start, buffer.LineEnd(line) - start), runs);
    }
    return out;
}

// An edit as TextEditor::Apply makes it.
WrapLayout::Edit Apply(TextBuffer& buffer, size_t pos, size_t len, std::string_view bytes) {
    WrapLayout::Edit edit;
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

// The incremental highlighter `h` on `buffer`, settled, against a fresh scan: every line's state
// and runs. Returns whether all agree (reporting the first difference).
bool SameAsFresh(MarkdownHighlighter& h, const TextBuffer& buffer, const char* what) {
    while (h.Settle(buffer, MarkdownHighlighter::kFrameBudget)) {
    }
    MarkdownHighlighter fresh;
    fresh.SetWikiLinks(h.WikiLinks());
    fresh.Reset(buffer);
    while (fresh.Settle(buffer, MarkdownHighlighter::kFrameBudget)) {
    }
    std::vector<SyntaxRun> a, b;
    for (size_t line = 0; line <= buffer.LineCount(); line++) {
        if (h.StateOf(line) != fresh.StateOf(line)) {
            std::printf("  %s: line %zu starts in state %#x, a fresh scan says %#x\n", what, line, h.StateOf(line),
                        fresh.StateOf(line));
            return false;
        }
        if (line == buffer.LineCount()) break;
        h.LineRuns(buffer, line, a);
        fresh.LineRuns(buffer, line, b);
        const bool same = a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const SyntaxRun& x, const SyntaxRun& y) {
                              return x.start == y.start && x.role == y.role;
                          });
        if (!same) {
            std::printf("  %s: line %zu's runs differ from a fresh scan's\n", what, line);
            return false;
        }
    }
    return true;
}

std::string Repeat(std::string_view piece, size_t count) {
    std::string out;
    out.reserve(piece.size() * count);
    for (size_t i = 0; i < count; i++) out.append(piece);
    return out;
}

} // namespace

RGUI_TEST("ui.highlight_blocks") {
    CHECK_EQ(Highlight("# Title *em*"), "m hhhhh mhhm");
    CHECK_EQ(Highlight("## T ##"), "mm h mm");
    CHECK_EQ(Highlight("####### seven"), "....... .....");
    CHECK_EQ(Highlight("- [ ] task **b**"), "m m m .... mm.mm");
    CHECK_EQ(Highlight("1. item"), "mm ....");
    CHECK_EQ(Highlight("> quote `code`"), "m ..... mccccm");
    CHECK_EQ(Highlight("> [!NOTE]\n> text"), "m mmhhhhm\nm ....");
    CHECK_EQ(Highlight("***\n\n- - -"), "mmm\n\nm m m");
    // Setext: the underline and the one line above it.
    CHECK_EQ(Highlight("one\nTitle\n==="), "...\nhhhhh\nmmm");
    CHECK_EQ(Highlight("Title\n---"), "hhhhh\nmmm");
    CHECK_EQ(Highlight("- item\n---"), "m ....\nmmm");
    CHECK_EQ(Highlight("===\n"), "...\n");
    // Tables: the delimiter row makes the row above the header.
    // Cells come first: a `|` ends a cell inside a code span too, unless escaped.
    CHECK_EQ(Highlight("a | b\n--|:-:\nc | `|`\n\nd"), "h m h\nmmmmmm\n. m .m.\n\n.");
    CHECK_EQ(Highlight("a | b\n--|--\nc | `a\\|b` | *d*"), "h m h\nmmmmm\n. m mccccm m m.m");
    CHECK_EQ(Highlight("a b\n--|--"), ". .\n.....");
    // Definitions.
    CHECK_EQ(Highlight("[a]: http://x \"t\""), "mlmm uuuuuuuu uuu");
    CHECK_EQ(Highlight("[^1]: note *a*"), "mllmm .... m.m");
    CHECK_EQ(Highlight("text\n[a]: b"), "....\n.... .");
}

RGUI_TEST("ui.highlight_code_and_math_blocks") {
    // Code suppresses every other colour; the language is Info.
    CHECK_EQ(Highlight("```python\nx = `1` *a* [l](u)\n```\n*a*"),
             "mmmiiiiii\nc c ccc ccc cccccc\nmmm\nm.m");
    CHECK_EQ(Highlight("~~~~\n~~~\n~~~~~ \nb"), "mmmm\nccc\nmmmmm \n.");
    CHECK_EQ(Highlight("```a`b\n*x*"), "......\nm.m");
    CHECK_EQ(Highlight("> ```\n> code\n> ```"), "m mmm\nm cccc\nm mmm");
    CHECK_EQ(Highlight("- ```js\n  x\n  ```"), "m mmmii\n  c\n  mmm");
    CHECK_EQ(Highlight("```math\nx^2\n```"), "mmmiiii\n$$$\nmmm");
    // An unclosed fence runs to the end.
    CHECK_EQ(Highlight("```\n# no\n\n*no*"), "mmm\nc cc\n\ncccc");
    // Display math: closed, on one line, ended by a blank line, and `\[ \]`.
    CHECK_EQ(Highlight("$$\nx^2\n$$\n*a*"), "mm\n$$$\nmm\nm.m");
    CHECK_EQ(Highlight("$$ x $$"), "mm $ mm");
    CHECK_EQ(Highlight("$$\nx\n\n*a*"), "mm\n$\n\nm.m");
    CHECK_EQ(Highlight("\\[\nx\n\\]"), "mm\n$\nmm");
    CHECK_EQ(Highlight("\\[ x \\]"), "mm $ mm");
    CHECK_EQ(Highlight("$$\nx\n```\ny"), "mm\n$\nmmm\nc");
}

RGUI_TEST("ui.highlight_front_matter") {
    CHECK_EQ(Highlight("---\ntitle: x\n---\n# H"), "mmm\niiiiii i\nmmm\nm h");
    CHECK_EQ(Highlight("---\na: *b*\n...\n"), "mmm\nii iii\nmmm\n");
    // Unclosed it is none: a rule, then text.
    CHECK_EQ(Highlight("---\ntitle *a*"), "mmm\n..... m.m");
    // Closed past kFrontMatterLines lines, it is none either.
    std::string late = "---\n" + Repeat("a: b\n", MarkdownHighlighter::kFrontMatterLines) + "---\n";
    CHECK_EQ(Highlight(late).substr(0, 8), "mmm\n.. .");
    // Only the first line opens one.
    CHECK_EQ(Highlight("x\n\n---\na\n---"), ".\n\nmmm\nh\nmmm");
}

RGUI_TEST("ui.highlight_inline") {
    CHECK_EQ(Highlight("*a* **b** ~~c~~ ~d~ _e_ ***f***"), "m.m mm.mm mm.mm m.m m.m mmm.mmm");
    // Unmatched and non-flanking delimiters are text.
    CHECK_EQ(Highlight("a * b **c ~~~d~~~ 2*3"), ". . . ... ....... ...");
    CHECK_EQ(Highlight("snake_case_name *a"), "............... ..");
    CHECK_EQ(Highlight("**a*"), ".m.m");
    // Escapes: the backslash is a marker, the byte after it text.
    CHECK_EQ(Highlight("\\*a\\* \\` \\"), "m..m. m. m");
    // Code spans: equal runs, opaque, unmatched runs text.
    CHECK_EQ(Highlight("`*a*` ``b`c`` ``d`"), "mcccm mmcccmm ....");
    CHECK_EQ(Highlight("`a\\`b`"), "mccm..");
    // Formulas, Pandoc's rule.
    CHECK_EQ(Highlight("a $x$ b $5 and $10 $x$5"), ". m$m . .. ... ... ....");
    CHECK_EQ(Highlight("$$x$$ \\( y \\) \\(z\\)"), "mm$mm mm $ mm m..m.");
    CHECK_EQ(Highlight("$a `$` b$"), "m$ $$$ $m");
    // Links, images, references, footnotes, autolinks, bare URLs.
    CHECK_EQ(Highlight("[t](u \"x\") ![a](b) [a][b] [c][]"), "mlmmu uuum mmlmmum mlmmlm mlmmm");
    CHECK_EQ(Highlight("[a [b] c](d)"), "ml lll lmmum");
    CHECK_EQ(Highlight("[a](<b c>) [x](y(z)) [p](q"), "mlmmuu uum mlmmuuuum .....");
    CHECK_EQ(Highlight("[*a*](u) [`c`](u)"), "mmlmmmum mmcmmmum");
    CHECK_EQ(Highlight("[^1] <https://x.y> <a@b.c> <a b>"), "llll uuuuuuuuuuuuu uuuuuuu .. ..");
    CHECK_EQ(Highlight("see https://x.y/(a). www.z.io"), "... uuuuuuuuuuuuuuu. ........");
    CHECK_EQ(Highlight("a <!-- c --> b"), ". !!!! ! !!! .");
    // Inline syntax never spans lines.
    CHECK_EQ(Highlight("*a\nb* `c\nd`"), "..\n.. ..\n..");
    // Heading text keeps its role under inline syntax.
    CHECK_EQ(Highlight("# a `b` [c](d)"), "m h mcm mlmmum");
}

RGUI_TEST("ui.highlight_wiki_links") {
    CHECK_EQ(Highlight("[[note|alias]] ![[img.png]]", true), "mmllllmlllllmm mmmlllllllmm");
    CHECK_EQ(Highlight("[[note|alias]]", false), "..............");
    CHECK_EQ(Highlight("[[]] [[a", true), ".... ...");
}

RGUI_TEST("ui.highlight_unicode") {
    // Flanking reads code points: emphasis around non-ASCII words, after non-ASCII punctuation.
    CHECK_EQ(Highlight("*żółw* “*a*”"), "m.......m ...m.m...");
    // Combining marks and wide characters keep the role of their byte runs.
    CHECK_EQ(Highlight("# e\xCC\x81 中"), "m hhh hhh");
}

RGUI_TEST("ui.highlight_incremental_matches_fresh") {
    const std::vector<std::string> pieces = {
        "", "\n", "\n\n", "```", "```js\n", "~~~", "$$", "$$\n", "\\[\n", "\\]", "---", "---\n", "...\n", "===", "|",
        "a | b\n", "--|--\n", "# ", "> ", "- ", "1. ", "*", "**", "`", "[a](b)", "[[x]]", "$x$", "text ", "x\n",
        "<!-- c -->", "\\", "[^1]", "    ",
    };
    std::mt19937 rng(20261020);
    for (int round = 0; round < 400; round++) {
        TextBuffer buffer;
        std::string text;
        const int lines = 1 + static_cast<int>(rng() % 60);
        for (int i = 0; i < lines; i++) {
            text += pieces[rng() % pieces.size()];
            text += pieces[rng() % pieces.size()];
            text += '\n';
        }
        buffer.SetText(text);
        MarkdownHighlighter h;
        h.SetWikiLinks(round % 2 == 0);
        h.Reset(buffer);
        // Settled at first in half the rounds, scanning when the edits start in the others.
        if (round % 2 == 0) {
            while (h.Settle(buffer, 64)) {
            }
        }
        for (int step = 0; step < 60; step++) {
            const size_t size = buffer.Size();
            const size_t pos = size ? rng() % (size + 1) : 0;
            const size_t len = rng() % 4 == 0 ? rng() % 24 : 0;
            const std::string& insert = pieces[rng() % pieces.size()];
            h.OnEdit(buffer, Apply(buffer, pos, len, insert));
            // Tiny budgets: work interrupted between edits.
            if (rng() % 3 == 0) h.Settle(buffer, 1 + rng() % 200);
            if (rng() % 5 == 0) {
                const size_t first = rng() % buffer.LineCount();
                h.PrepareView(buffer, first, first + rng() % 20, 1 + rng() % 400);
            }
            if (step % 10 == 9 && !SameAsFresh(h, buffer, "random edits")) {
                CHECK(false);
                return;
            }
        }
    }
}

RGUI_TEST("ui.highlight_interrupted_and_reset") {
    // A fence opened at the top, 20,000 lines below it, a frame's scan of 4 KiB at a time.
    std::string text = "```\n" + Repeat("*a* line\n", 20000);
    TextBuffer buffer;
    buffer.SetText(text);
    MarkdownHighlighter h;
    h.Reset(buffer);
    CHECK(h.Known(0));
    CHECK(!h.Known(10000));
    // A view far below: its lines wait, plain.
    CHECK(!h.PrepareView(buffer, 10000, 10040, MarkdownHighlighter::kViewBudget));
    size_t count = 7;
    CHECK(h.Runs(10000, count) == nullptr);
    CHECK_EQ(count, size_t{0});
    int frames = 0;
    const uint64_t before = h.Stats().scannedBytes;
    while (h.Settle(buffer, 4096)) frames++;
    CHECK(frames > 10);
    CHECK(h.Stats().scannedBytes - before >= text.size() - static_cast<size_t>(std::count(text.begin(), text.end(), '\n')));
    CHECK(!h.PrepareView(buffer, 10000, 10040, MarkdownHighlighter::kViewBudget));
    const SyntaxRun* runs = h.Runs(10000, count);
    CHECK(runs != nullptr && count == 1 && runs[0].role == SyntaxRole::Code);

    // Closing the fence at the top: every line below changes, a budget at a time.
    h.OnEdit(buffer, Apply(buffer, 4, 0, "```\n"));
    CHECK(!h.Known(10000));
    CHECK(h.Runs(10000, count) == nullptr);   // stale lines show no colours, even before PrepareView
    CHECK(!h.PrepareView(buffer, 10000, 10040, MarkdownHighlighter::kViewBudget));
    CHECK(h.Runs(10000, count) == nullptr);   // unknown: plain, never the old colours
    CHECK(SameAsFresh(h, buffer, "fence closed"));
    CHECK(!h.PrepareView(buffer, 10000, 10040, MarkdownHighlighter::kViewBudget));
    runs = h.Runs(10001, count);
    CHECK(runs != nullptr && runs[0].role == SyntaxRole::Marker);

    // A typed character converges within its line.
    const uint64_t lines = h.Stats().scannedLines;
    h.OnEdit(buffer, Apply(buffer, buffer.LineStart(5000) + 1, 0, "x"));
    CHECK(!h.Settle(buffer, MarkdownHighlighter::kFrameBudget));
    CHECK(h.Stats().scannedLines - lines <= 2);

    // A new document mid-scan starts over.
    h.OnEdit(buffer, Apply(buffer, 0, 0, "~~~\n"));
    h.Settle(buffer, 100);
    buffer.SetText("# new\n");
    h.Reset(buffer);
    CHECK(!h.Known(1) || h.StateOf(1) == 0);
    CHECK(SameAsFresh(h, buffer, "reset"));
}

RGUI_TEST("ui.highlight_front_matter_edits") {
    TextBuffer buffer;
    buffer.SetText("---\na: b\n---\n# H\n" + Repeat("text\n", 50));
    MarkdownHighlighter h;
    h.Reset(buffer);
    CHECK(SameAsFresh(h, buffer, "front matter"));
    // Removing the closing line: the front matter is gone, line 0 a rule again.
    h.OnEdit(buffer, Apply(buffer, buffer.LineStart(2), 4, ""));
    CHECK(SameAsFresh(h, buffer, "closer removed"));
    CHECK_EQ(h.StateOf(1), 0);
    // And back, typed in the middle.
    h.OnEdit(buffer, Apply(buffer, buffer.LineStart(2), 0, "...\n"));
    CHECK(SameAsFresh(h, buffer, "closer typed"));
    CHECK(h.StateOf(1) != 0);
}

RGUI_TEST("ui.highlight_limits") {
    using Clock = std::chrono::steady_clock;
    // Lines of unmatched openers, each at the inline cap: linear, so each tokenizes fast, and the
    // runs stay within kMaxLineRuns.
    const size_t cap = MarkdownHighlighter::kMaxInlineBytes;
    const std::vector<std::string> hostile = {
        Repeat("[a](", cap / 4), Repeat("$a ", cap / 3), Repeat("<a:", cap / 3), Repeat("**a", cap / 3),
        Repeat("`", cap / 2) + "x" + Repeat("``", cap / 4 - 1), Repeat("[^", cap / 2), Repeat("![[", cap / 3),
        Repeat("<!--", cap / 4), Repeat("[a](b \"", cap / 8), Repeat("_a*", cap / 3), Repeat("*a", cap / 2),
        Repeat("\\(", cap / 2), Repeat("$$a", cap / 3), Repeat("[", cap - 1) + Repeat("](x)", 1),
        Repeat("(", cap / 2) + Repeat(")", cap / 2), Repeat("[a](b(", cap / 6), Repeat("http://a", cap / 8),
    };
    for (const std::string& line : hostile) {
        TextBuffer buffer;
        buffer.SetText(line);
        MarkdownHighlighter h;
        h.SetWikiLinks(true);
        h.Reset(buffer);
        h.Settle(buffer, MarkdownHighlighter::kFrameBudget);
        std::vector<SyntaxRun> runs;
        const auto t0 = Clock::now();
        CHECK(h.LineRuns(buffer, 0, runs));
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        CHECK(runs.size() <= MarkdownHighlighter::kMaxLineRuns);
        // Generous: a linear pass over 16 KiB takes well under a millisecond.
        CHECK(ms < 50);
        if (ms >= 50) std::printf("  %zu-byte line starting %.12s took %.1f ms\n", line.size(), line.c_str(), ms);
    }
    // A line over the inline cap keeps its block colours only and reads no more than the cap.
    {
        TextBuffer buffer;
        buffer.SetText("# " + Repeat("*a* ", 1 << 18));
        MarkdownHighlighter h;
        h.Reset(buffer);
        h.Settle(buffer, MarkdownHighlighter::kFrameBudget);
        std::vector<SyntaxRun> runs;
        const uint64_t before = h.Stats().tokenizedBytes;
        CHECK(h.LineRuns(buffer, 0, runs));
        CHECK(h.Stats().tokenizedBytes - before <= MarkdownHighlighter::kBlockPrefixBytes);
        CHECK_EQ(runs.size(), size_t{2});
        CHECK(runs.size() == 2 && runs[1].role == SyntaxRole::Heading);
        // Its scan read the block prefix only.
        CHECK(h.Stats().scannedBytes <= MarkdownHighlighter::kBlockPrefixBytes);
    }
    // Dense syntax past kMaxLineRuns: block colours only.
    {
        TextBuffer buffer;
        buffer.SetText("> " + Repeat("*a* ", 2000));
        MarkdownHighlighter h;
        h.Reset(buffer);
        h.Settle(buffer, MarkdownHighlighter::kFrameBudget);
        std::vector<SyntaxRun> runs;
        CHECK(h.LineRuns(buffer, 0, runs));
        CHECK_EQ(runs.size(), size_t{2});
    }
    // A frame's scan stays within its budget plus one line (and a front matter's look-ahead).
    {
        TextBuffer buffer;
        buffer.SetText(Repeat("line\n", 100000));
        MarkdownHighlighter h;
        h.Reset(buffer);
        const uint64_t lines = h.Stats().scannedLines;
        h.Settle(buffer, 4096);
        const uint64_t step = h.Stats().scannedLines - lines;
        CHECK(step * (MarkdownHighlighter::kLineCost + 4) <= 4096 + MarkdownHighlighter::kLineCost + 4);
        // Two bytes a line of state, plus a view.
        while (h.Settle(buffer, MarkdownHighlighter::kFrameBudget)) {
        }
        CHECK(h.MemoryBytes() < 2 * (buffer.LineCount() + 1) + 256 * 1024);
    }
    // A view's work stays within its budget: lines past it wait for a later frame.
    {
        TextBuffer buffer;
        buffer.SetText(Repeat("# " + std::string(4000, 'x') + "\n", 100));
        MarkdownHighlighter h;
        h.Reset(buffer);
        while (h.Settle(buffer, MarkdownHighlighter::kFrameBudget)) {
        }
        CHECK(h.PrepareView(buffer, 0, 99, 16 * 1024));
        size_t count = 0;
        CHECK(h.Runs(0, count) != nullptr);
        CHECK(h.Runs(99, count) == nullptr);
        int frames = 1;
        while (h.PrepareView(buffer, 0, 99, 16 * 1024)) frames++;
        CHECK(frames >= 15);
        CHECK(h.Runs(99, count) != nullptr);
    }
}

RGUI_TEST("ui.highlight_editor_frames") {
    const rgui::ui::Tokens dark = rgui::ui::BuildTokens(rgui::ui::BundledPalette(*rgui::ui::FindBundledTheme("tokyo-night")),
                                                        rgui::ui::TokenSources{});
    const rgui::ui::Tokens light = rgui::ui::BuildTokens(
        rgui::ui::BundledPalette(*rgui::ui::FindBundledTheme("catppuccin-latte")), rgui::ui::TokenSources{});
    rgui::ui::Context ctx;
    ctx.SetTokens(&dark);
    rgui::ui::TextEditor editor;
    editor.SetLayout(800, 600, 10, 20);
    editor.SetHighlightBudgets(2048, 4096);
    editor.SetText("```\n" + Repeat("# heading *a* `b`\n", 3000));
    double now = 1000;
    auto frame = [&](float width = 800) {
        ctx.BeginFrame(static_cast<int>(width), 600, 1.0, now);
        editor.Frame(ctx, {0, 0, width, 600});
        now += 16;
        return ctx.EndFrame().wantsFrame;
    };
    // Frames go on while the scan does, then stop.
    int frames = 0;
    while (frame() && frames < 10000) frames++;
    CHECK(frames > 5);
    CHECK(!editor.Highlighter().Pending());
    const MarkdownHighlighter::Counters settled = editor.Highlighter().Stats();
    // Settled: frames, a theme change and a resize do no syntax work and ask for no frame.
    CHECK(!frame());
    ctx.SetTokens(&light);
    CHECK(!frame());
    CHECK(!frame(700));
    CHECK_EQ(editor.Highlighter().Stats().scannedLines, settled.scannedLines);
    CHECK_EQ(editor.Highlighter().Stats().tokenizedLines, settled.tokenizedLines);
    // A typed character: its line again, no other.
    editor.Select(10, 10);
    editor.HandleText("x");
    CHECK(!frame());
    CHECK(editor.Highlighter().Stats().scannedLines - settled.scannedLines <= 2);
    CHECK(editor.Highlighter().Stats().tokenizedLines - settled.tokenizedLines <= 2);
    // Turned off, it holds nothing; on again, it starts over.
    editor.SetSyntaxColors(false);
    CHECK(!editor.Highlighter().Active());
    CHECK(editor.Highlighter().MemoryBytes() < 64);
    CHECK(!frame());
    editor.SetSyntaxColors(true);
    CHECK(editor.Highlighter().Pending());
    while (frame()) {
    }
    CHECK(!editor.Highlighter().Pending());
}

RGUI_TEST("ui.syntax_colors_readable") {
    // Every bundled theme: each syntax colour keeps kMinSyntaxContrast with the background.
    for (const rgui::ui::BundledTheme& theme : rgui::ui::kBundledThemes) {
        const rgui::ui::Tokens t = rgui::ui::BuildTokens(rgui::ui::BundledPalette(theme), rgui::ui::TokenSources{});
        const rgui::ui::DerivedColors& c = t.colors;
        for (const rgui::ui::Rgba color : {c.syntaxMarker, c.syntaxHeading, c.syntaxCode, c.syntaxLink, c.syntaxUrl,
                                           c.syntaxMath, c.syntaxInfo, c.syntaxComment}) {
            const double contrast = rgui::ui::ContrastRatio(color, c.windowBackground);
            CHECK(contrast >= rgui::ui::kMinSyntaxContrast ||
                  contrast >= rgui::ui::ContrastRatio(c.text, c.windowBackground) - 1e-9);
        }
    }
}

namespace {

// `list` with every glyph command spanning the frame's width (its rows as they were): drawn whole,
// each glyph's ink lands wherever it reaches across, whatever left and right bounds the editor gave
// its command.
rgui::ui::DisplayList Widened(const rgui::ui::DisplayList& list) {
    using rgui::ui::CommandType;
    rgui::ui::DisplayList out;
    out.Reset(list.Width(), list.Height(), list.Scale());
    const auto whole = [](const rgui::gfx::RectF& r) {
        return rgui::gfx::RectI{static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.x + r.w),
                                static_cast<int>(r.y + r.h)};
    };
    for (const rgui::ui::Command& c : list.Commands()) {
        switch (c.type) {
        case CommandType::Rect:
            out.FillRect(whole(c.rect), c.color);
            break;
        case CommandType::ClipPush:
            out.PushClip(whole(c.rect));
            break;
        case CommandType::ClipPop:
            out.PopClip();
            break;
        case CommandType::Glyphs: {
            const rgui::gfx::RectI across{0, c.bounds.y0, list.Width(), c.bounds.y1};
            out.Glyphs(list.GlyphArena().data() + c.glyphBegin, c.glyphCount, c.size, c.color, &across);
            break;
        }
        default:
            CHECK(false);   // the editor draws nothing else
            break;
        }
    }
    return out;
}

} // namespace

// The tiles a keystroke redraws hold all of every glyph near them: with syntax colours each
// colour's command is bounded by its own glyphs, so after every edit (in coloured text, before
// combining marks, around fallback glyphs of other widths) the incremental raster must equal a full
// raster of the same glyphs whose commands span the frame.
RGUI_TEST("ui.highlight_tiles_match_full_redraw") {
    rgui::text::FontIndex index;
    index.SetDirectories({"/usr/share/fonts"});
    rgui::text::FontSet fonts;
    if (index.Find("DejaVu Sans Mono", 400, false) == nullptr ||
        !fonts.Load(index, {"DejaVu Sans Mono"}, {"DejaVu Sans"})) {
        std::printf("skip ui.highlight_tiles_match_full_redraw: DejaVu Sans Mono not installed\n");
        return;
    }
    const rgui::ui::Tokens tokens = rgui::ui::BuildTokens(
        rgui::ui::BundledPalette(*rgui::ui::FindBundledTheme("tokyo-night")), rgui::ui::TokenSources{});
    for (const double scale : {1.0, 1.5}) {
        rgui::ui::Context ctx;
        ctx.SetFonts(&fonts);
        ctx.SetTokens(&tokens);
        rgui::ui::TextEditor editor;
        editor.SetText("# Title *em* e\xCC\x81\xCC\x81 `code`\n> quote [link](http://x) \xD8\xA7\xD9\x84\xD8\xB9\n"
                       "- [ ] item **b** \xCE\xB1\xCE\xB2 $x^2$ ~~s~~\n```c\nint x; /* \xE2\x86\x92 */\n```\nplain text\n");
        const int width = static_cast<int>(640 * scale), height = static_cast<int>(400 * scale);
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * height), fresh(pixels.size());
        const rgui::gfx::Surface surface{pixels.data(), width, height, width};
        const rgui::gfx::Surface freshSurface{fresh.data(), width, height, width};
        rgui::ui::Compositor compositor(1);
        rgui::text::GlyphCache glyphs;
        rgui::ui::IconCache icons;
        const uint32_t background = tokens.colors.windowBackground.Premultiplied();
        double now = 1000;
        int mismatches = 0;
        auto frame = [&] {
            ctx.BeginFrame(width, height, scale, now);
            editor.Frame(ctx, {0, 0, 640, 400});
            const rgui::ui::FrameOutput& out = ctx.EndFrame();
            glyphs.BeginFrame();
            compositor.Render(*out.displayList, background, surface, glyphs, icons);
            rgui::ui::Compositor full(1);
            rgui::text::GlyphCache freshGlyphs;
            rgui::ui::IconCache freshIcons;
            freshGlyphs.BeginFrame();
            full.Render(Widened(*out.displayList), background, freshSurface, freshGlyphs, freshIcons);
            if (pixels != fresh) mismatches++;
            now += 50;
        };
        frame();
        // Edits at the starts, middles and ends of coloured runs, combining marks, fallback glyphs.
        const std::vector<std::pair<size_t, std::string>> edits = {
            {2, "X"}, {9, "*"}, {14, "\xCC\x81"}, {11, "\xD8\xA8"}, {0, "#"}, {40, "`"}, {60, "\xCE\xB3"},
            {3, "\n"}, {75, "~~"}, {90, " **"}, {1, ""}, {50, "$"}, {20, "\xE4\xB8\xAD"}, {5, "\n\n"},
        };
        for (const auto& [at, text] : edits) {
            const size_t pos = std::min(at, editor.Buffer().Size());
            if (text.empty()) editor.Replace(pos, 1, "", 0);
            else editor.Replace(pos, 0, text, text.size());
            frame();
        }
        CHECK_EQ(mismatches, 0);
    }
}
