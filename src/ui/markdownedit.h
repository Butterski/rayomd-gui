// SPDX-License-Identifier: Apache-2.0
// What the editor knows of Markdown while it is typed: the container prefix of a line (indentation,
// `>` quote markers, a list marker with its task box), list and quote continuation on Enter, list
// item indentation on Tab, the completions that open after ```, `> [!` and `[[`, the blocks the
// insert list puts on lines of their own, and the ATX headings of a document. Every function reads
// at most the bytes it is given; the editor hands it a line's first kLinePrefixBytes, never the
// document, so a keystroke stays O(1) in the document's size.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::ui::markdown {

// How much of a line the editor reads for its prefix.
constexpr size_t kLinePrefixBytes = 256;

// A replacement relative to some origin (the line's start): [pos, pos + len) becomes `text`, the
// caret goes to pos + caret.
struct TextEdit {
    size_t pos = 0;
    size_t len = 0;
    std::string text;
    size_t caret = 0;
};

// The container prefix of a line.
struct LinePrefix {
    size_t quoteEnd = 0;    // after the indentation, the `>` markers and one space after each
    size_t markerEnd = 0;   // after the list marker, the spaces after it and a task box; quoteEnd without one
    size_t indent = 0;      // bytes of indentation between quoteEnd and the list marker
    bool quote = false;     // a `>` was read
    bool list = false;      // a list marker was read
    bool ordered = false;   // `1.` or `1)`
    bool task = false;      // `[ ]`, `[x]` or `[X]` after the marker
    char bullet = 0;        // '-', '*', '+', or the ordered delimiter '.' or ')'
    uint64_t number = 0;    // an ordered item's number
};
LinePrefix ParseLinePrefix(std::string_view line);

// Enter at `caret` (an offset into `line`, which holds the line's first bytes; `whole` when it is
// the complete line): on a list item or quote line, past its marker, the edit that breaks the line
// and starts the next item (`- `, `* `, `+ `, the next number, `- [ ] `, `> `, with the same
// indentation and quotes). On an item with nothing after its marker, the edit that removes the
// marker (a list marker inside a quote leaves the quote). False when Enter is a plain line break.
bool ContinueOnEnter(std::string_view line, bool whole, size_t caret, TextEdit& edit);

// Tab (or Shift+Tab with `outdent`) on a list item line: four spaces more before the marker, or
// one tab or up to four spaces fewer. False when the line is no list item or nothing is left to
// remove; the edit keeps the caret where it was in the item's text.
bool IndentListItem(std::string_view line, size_t caret, bool outdent, TextEdit& edit);

// ---- Completion ----
enum class Completion : uint8_t { None, Fence, Alert, WikiLink };
// What the line before the caret (its last bytes, at most kLinePrefixBytes) opens: a fence (```
// after nothing but indentation), an alert (`[!` after `>` markers) or, when `wikiLinks`, a wiki
// link (`[[`, not after `!` or another `[`).
Completion CompletionTrigger(std::string_view beforeCaret, bool wikiLinks);
// The trigger's bytes ("```", "[!", "[[").
std::string_view TriggerText(Completion kind);
// Whether `filter` (typed after the trigger) still belongs to the completion: one line, at most
// kMaxFilterBytes; a fence's language or an alert's type has no space, backtick or bracket.
constexpr size_t kMaxFilterBytes = 64;
bool FilterValid(Completion kind, std::string_view filter);
// How `item` matches `filter`, ASCII case-insensitive: 0 equal (or an empty filter), 1 starting
// with it, 2 containing it, -1 not at all. Lists show the better matches first.
int MatchRank(std::string_view item, std::string_view filter);

// The fence names offered after ```: the engine's highlighted languages with their common
// aliases (highlight.cpp's kSpecs), `mermaid` and `math`.
const std::vector<std::string>& FenceNames();
// GitHub's alert types: NOTE, TIP, IMPORTANT, WARNING, CAUTION.
const std::vector<std::string>& AlertTypes();

// ---- Blocks on lines of their own ----
// Where the caret is, for PlaceBlock: whether the line has text before and after the caret, and
// whether the lines above and below have text (a missing line has none).
struct BlockSite {
    bool textBefore = false;
    bool textAfter = false;
    bool textAbove = false;
    bool textBelow = false;
    bool atEnd = false;     // the caret's line is the document's last
};
// `block` (lines without the last line break) as inserted at the caret on lines of its own, a
// blank line between it and text before or after it. The caret goes to `caretInBlock` in it, or,
// with `caretAfter`, below it after a blank line.
TextEdit PlaceBlock(std::string_view block, size_t caretInBlock, bool caretAfter, const BlockSite& site);
// Whether `text` holds anything but spaces and tabs.
bool HasText(std::string_view text);

// ---- Headings ----
// The document's ATX headings (`#` to `######`), outside fenced code and the front matter, their
// text trimmed and without a closing `#` sequence: at most kMaxHeadings, each cut to
// kMaxHeadingBytes (at a code point), kMaxHeadingTotal bytes in all. One forward pass.
constexpr size_t kMaxHeadings = 1000;
constexpr size_t kMaxHeadingBytes = 200;
constexpr size_t kMaxHeadingTotal = 64 * 1024;
std::vector<std::string> DocumentHeadings(std::string_view text);

} // namespace rgui::ui::markdown
