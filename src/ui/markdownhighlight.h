// SPDX-License-Identifier: Apache-2.0
// Markdown syntax colours for the editor: a role for every byte of the visible lines, kept apart
// from the wrap layout and from the theme (the editor maps roles to colours as it draws, so a theme
// change, a resize or a zoom needs no new scan).
//
// Two layers. The block layer keeps, for every line, the state the line starts in (16 bits: a
// fence's character and length, display math, the front matter, whether the line before was
// paragraph text with a `|`, an open table). A line's state follows from the state before it and
// the line's first kBlockPrefixBytes alone, so a scan goes forward line by line, a budget of bytes
// a frame (Settle), and an edit rescans from its first line until the state after an unchanged
// line equals the one kept from before (the old states after the edit are shifted with the lines,
// never trusted until then). Lines whose state is not known yet are drawn plain. The inline layer
// tokenizes only the lines in view (PrepareView), each from its own bytes, and keeps their runs
// until an edit, a state change or the view moves them out.
//
// The subset, in order of precedence:
//   - Front matter: a first line `---` closed by `---` or `...` within kFrontMatterLines lines;
//     unclosed (or closed later) it is none, as in the engine. Its lines are Info, the fences Marker.
//   - Fenced code (``` or ~~~, at least 3, after quote and list markers; a backtick fence's info
//     has no backtick; closed by the same character, at least as long, nothing after): the fence
//     runs Marker, the language Info, every line inside Code (Math in a `math` fence), quote markers
//     of a fence opened in a quote Marker. Unclosed, it runs to the end of the text.
//   - Display math: a line starting `$$` (or `\[` alone) that does not close on itself opens it;
//     the line holding the closing `$$` (or ending in `\]`) closes it; a blank line or a fence line
//     ends it without a close (the engine then shows the `$$` as text: the editor has coloured the
//     lines between as Math, at most a paragraph). `$$ x $$` alone on a line is one formula.
//   - Block markers (Marker): quote `>`, list bullets and numbers, task boxes, ATX `#` runs (the
//     text Heading), thematic breaks, setext underlines (the line above Heading: only that line,
//     not a paragraph's earlier lines), a table's delimiter row and its `|` (the row above the
//     delimiter row is its header, Heading; a row is cut into cells at every unescaped `|` before
//     its inline syntax, as GFM does, so a `|` ends a code span's cell too), link reference
//     definitions (label Link, destination Url), GitHub alerts and callouts (`[!NOTE]` Heading).
//   - Inline, left to right, the construct that starts first winning: backslash escapes (the
//     backslash Marker, the escaped byte as text), code spans (backtick runs of equal length; an
//     unmatched run is text), formulas (`$x$` by Pandoc's rule as the engine reads it, `$$x$$`,
//     `\( x \)`: delimiters Marker, body Math; one over the engine's size cap Code), autolinks
//     `<scheme:...>` and `<a@b>` (Url), HTML comments `<!-- -->` on one line (Comment), links and
//     images `[text](dest "title")` and `[text][ref]` (brackets and parentheses Marker, text Link,
//     destination Url; a link holds no link), footnote references `[^x]` (Link), wiki links and
//     embeds `[[note|alias]]` when SetWikiLinks (brackets Marker, target Link), bare `http://` and
//     `https://` URLs as the engine links them (Url), emphasis, strong and strike delimiters
//     matched by CommonMark's delimiter rules (`~` and `~~` as GFM's strikethrough; Marker; the
//     text keeps its role; unmatched delimiters are text).
// Inline syntax never spans lines: each line is read alone, so an emphasis or code span whose
// delimiters sit on different lines is text. Indented code blocks, HTML blocks, lazy continuation
// and list item nesting are not tracked: an indented line is read as if it were not indented.
// Soft wrapping plays no part: roles are by source byte.
//
// Fallbacks (plain text, documented limits): a line whose state is unknown; inline syntax of a
// line longer than kMaxInlineBytes or yielding more than kMaxLineRuns runs (its block colours stay);
// block syntax past a line's first kBlockPrefixBytes (a longer line is no fence close, underline,
// delimiter row or rule; one whose first kBlockPrefixBytes are blank is blank); fence runs longer
// than kMaxFenceLength count as that long; lines past kMaxViewLines of a view, or past the
// view's run capacity, or past a frame's view budget (drawn in a later frame).
#pragma once

#include "ui/textbuffer.h"
#include "ui/textwrap.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::ui {

// What a byte of the source shows as.
enum class SyntaxRole : uint8_t {
    Plain,    // text
    Marker,   // syntax punctuation: block markers, emphasis delimiters, brackets, fences, pipes, escapes
    Heading,  // heading text, a table's header row, an alert's type
    Code,     // code spans, fenced code
    Link,     // link text, footnote references, wiki links, reference labels
    Url,      // link destinations, autolinks, bare URLs
    Math,     // formulas
    Info,     // a fence's language, the front matter
    Comment,  // HTML comments
};
constexpr int kSyntaxRoleCount = 9;

// A run of one role: from `start` (bytes from the line's start) to the next run's start, the last
// to the line's end.
struct SyntaxRun {
    uint32_t start = 0;
    SyntaxRole role = SyntaxRole::Plain;
};

class MarkdownHighlighter {
public:
    // What block classification reads of a line.
    static constexpr size_t kBlockPrefixBytes = 1024;
    // Longer lines get their block colours only.
    static constexpr size_t kMaxInlineBytes = 16 * 1024;
    // A line with more runs gets its block colours only.
    static constexpr size_t kMaxLineRuns = 512;
    // Lines a front matter may take, its closing line included.
    static constexpr size_t kFrontMatterLines = 1000;
    static constexpr size_t kMaxFenceLength = 255;
    // Lines of a view that get runs; the runs the view keeps in all.
    static constexpr size_t kMaxViewLines = 1024;
    static constexpr size_t kMaxViewRuns = 64 * 1024;
    // Work units: a line's scan costs kLineCost plus the bytes it reads (a line's fixed cost is
    // about 64 bytes' worth: bench.highlight's short lines and tester.md copies take about the same
    // time a frame, 0.2 ms on a Ryzen 5 PRO 4650GE).
    static constexpr size_t kLineCost = 64;
    // A frame's scan (Settle), and the line bytes a frame's PrepareView tokenizes (it may finish
    // the line it started: kMaxInlineBytes more at most).
    static constexpr size_t kFrameBudget = 384 * 1024;
    static constexpr size_t kViewBudget = 64 * 1024;

    // What the highlighter did, for tests and benchmarks.
    struct Counters {
        uint64_t scannedLines = 0;     // block states computed
        uint64_t scannedBytes = 0;     // bytes block classification read (front matter look-ahead too)
        uint64_t tokenizedLines = 0;   // lines whose runs were made
        uint64_t tokenizedBytes = 0;
    };

    // Starts over on `buffer`'s text (a new document): nothing known, the scan at line 0. Keeps its
    // storage.
    void Reset(const TextBuffer& buffer);
    // Releases everything (highlighting turned off); Reset starts again.
    void Clear();
    bool Active() const { return !states_.empty(); }

    // After `edit` of `buffer` (the edit WrapLayout::OnEdit gets).
    void OnEdit(const TextBuffer& buffer, const WrapLayout::Edit& edit);

    // Scans up to `budget` work units; returns whether lines are still left.
    bool Settle(const TextBuffer& buffer, size_t budget);
    bool Pending() const { return Active() && scan_ + 1 < states_.size(); }
    // Whether the state `line` starts in is known (its colours can be drawn).
    bool Known(size_t line) const { return Active() && line <= scan_; }
    // The state `line` starts in (meaningful when Known).
    uint16_t StateOf(size_t line) const { return states_[line]; }

    // Wiki links and embeds (Obsidian's dialect). Only the view's runs change.
    void SetWikiLinks(bool on);
    bool WikiLinks() const { return wikiLinks_; }

    // Makes the runs of lines [first, last] current: lines whose runs are missing or stale are
    // tokenized, up to `budget` bytes of their text. Returns whether a line of the range is left
    // without runs for the budget (a later frame does it); lines whose state is unknown wait for
    // Settle and do not count.
    bool PrepareView(const TextBuffer& buffer, size_t first, size_t last, size_t budget);
    // The runs of `line` as the last PrepareView made them; nullptr (plain) when it has none.
    const SyntaxRun* Runs(size_t line, size_t& count) const;

    // The runs of `line` made now (for tests and the fuzz harness), into `out`; false when its
    // state is unknown.
    bool LineRuns(const TextBuffer& buffer, size_t line, std::vector<SyntaxRun>& out);

    const Counters& Stats() const { return stats_; }
    // The bytes it holds, capacities included.
    size_t MemoryBytes() const;

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);

    struct ViewLine {
        uint32_t begin = 0;      // into runs_
        uint32_t count = 0;
        uint16_t state = 0;      // the state it was made in
        uint16_t next = 0;       // the next line's
        bool valid = false;      // made, and its text and its next line's unchanged since
        bool known = false;      // made with a known state (else it has no runs)
    };

    // The first bytes of `line` (at most `limit`), in place or through `scratch`; `length` the
    // line's length.
    std::string_view LineBytes(const TextBuffer& buffer, size_t line, size_t limit, std::string& scratch,
                               size_t& length) const;
    // The state after `line`, which starts in `state`. `scanning`: the scan's own step, which
    // notes how far a front matter's look-ahead read.
    uint16_t StateAfter(const TextBuffer& buffer, size_t line, uint16_t state, bool scanning);
    // Appends `line`'s runs to `out`.
    void Tokenize(const TextBuffer& buffer, size_t line, uint16_t state, uint16_t next, std::vector<SyntaxRun>& out);
    // Whether a front matter opened on line 0 closes, and the line it closes on (the last line a
    // front matter may take when it does not).
    bool FrontMatterCloses(const TextBuffer& buffer, size_t& closeLine);

    // states_[i]: the state line i starts in, i <= the line count (the last: after the text).
    // Lines [0, scan_] are known. keeps_: stretches [from, to] of states kept from before edits
    // (sorted, all past scan_), each valid once a scan reaches its first line in the state it
    // holds there; at most kMaxKeeps (the farthest go: scanned again rather than trusted).
    static constexpr size_t kMaxKeeps = 16;
    struct Keep {
        size_t from;
        size_t to;
    };
    std::vector<uint16_t> states_;
    size_t scan_ = 0;
    std::vector<Keep> keeps_;
    std::vector<Keep> nextKeeps_;
    // The last line a front matter's look-ahead depends on (0 without a front matter candidate).
    size_t frontMatterEnd_ = 0;
    bool wikiLinks_ = false;

    // The view: lines [viewFirst_, viewFirst_ + view_.size()), their runs in runs_.
    size_t viewFirst_ = 0;
    std::vector<ViewLine> view_;
    std::vector<SyntaxRun> runs_;
    // Rebuilding the view: the next lines and runs, swapped in.
    std::vector<ViewLine> nextView_;
    std::vector<SyntaxRun> nextRuns_;

    // Scratch, reused.
    std::string lineScratch_;
    std::string nextScratch_;
    std::vector<uint8_t> roles_;
    struct Tick {
        uint32_t pos;
        uint32_t len;
        int32_t next;   // the next run of the same length, or -1
    };
    std::vector<Tick> ticks_;
    std::vector<int32_t> lastTick_;
    struct Delim {
        uint32_t start;     // the first byte not yet matched
        uint16_t len;       // bytes not yet matched
        uint16_t orig;
        char ch;
        bool canOpen;
        bool canClose;
        int32_t prev;       // the delimiter before it not yet removed, or -1
    };
    std::vector<Delim> delims_;
    struct Bracket {
        uint32_t pos;       // the `[` (an image's `!`)
        uint32_t delims;    // delims_.size() when it opened
        bool image;
        bool active;
    };
    std::vector<Bracket> brackets_;
    std::vector<int32_t> parenClose_;
    std::vector<int32_t> nextSpace_;
    std::vector<int32_t> stack_;

    Counters stats_;
    // Work units spent, for the budgets.
    uint64_t work_ = 0;

    friend struct InlineScan;
};

} // namespace rgui::ui
