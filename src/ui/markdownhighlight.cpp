// SPDX-License-Identifier: Apache-2.0
#include "ui/markdownhighlight.h"

#include "common/text_utils.h"
#include "text/unicode.h"
#include "ui/markdownedit.h"

#include <algorithm>
#include <cstring>

namespace rgui::ui {

namespace {

constexpr size_t kNpos = std::string_view::npos;

// ---- Line states (16 bits) ----
// bits 0-1 the mode (0 normal text); Fence: bit 2 `~`, bits 3-10 the run's length, bit 11 opened
// after quote markers, bit 12 a `math` fence; Math: bit 2 `\[`, bit 11 quoted; normal text: bit 13
// the line before was paragraph text, bit 14 ... holding a `|`, bit 15 inside a table.
constexpr uint16_t kModeMask = 3;
constexpr uint16_t kFence = 1;
constexpr uint16_t kMath = 2;
constexpr uint16_t kFront = 3;
constexpr uint16_t kTilde = 1u << 2;
constexpr uint16_t kBracketMath = 1u << 2;
constexpr int kLenShift = 3;
constexpr uint16_t kLenMask = 0xFFu << kLenShift;
constexpr uint16_t kQuoted = 1u << 11;
constexpr uint16_t kMathFence = 1u << 12;
constexpr uint16_t kPara = 1u << 13;
constexpr uint16_t kPipe = 1u << 14;
constexpr uint16_t kTable = 1u << 15;

// The engine's caps on a formula's body (inline_markdown.cpp): longer ones show verbatim, as code.
constexpr size_t kMaxInlineMathBytes = 1024;
constexpr size_t kMaxDisplayMathBytes = 8192;
// A front matter's closing line is no longer than this.
constexpr size_t kFrontCloseBytes = 64;
// Link labels, footnote labels and URL schemes as CommonMark bounds them.
constexpr size_t kMaxLabelBytes = 999;
constexpr size_t kMaxSchemeBytes = 32;
// Alert types (`[!WARNING]`, Obsidian's callouts).
constexpr size_t kMaxAlertBytes = 32;

bool IsBlank(char c) { return c == ' ' || c == '\t'; }

size_t SkipBlanks(std::string_view s, size_t i) {
    while (i < s.size() && IsBlank(s[i])) i++;
    return i;
}

bool AllBlank(std::string_view s) {
    for (const char c : s) {
        if (!IsBlank(c)) return false;
    }
    return true;
}

std::string_view Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && IsBlank(s[a])) a++;
    while (b > a && IsBlank(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool IsAsciiPunct(unsigned char c) {
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlnum(char c) { return IsAlpha(c) || IsDigit(c); }

// Whether the backslashes before s[at] are odd in number.
bool EscapedAt(std::string_view s, size_t at) {
    size_t n = 0;
    while (at > n && s[at - 1 - n] == '\\') n++;
    return (n & 1) != 0;
}

// The first "$$" in `s` not after an odd number of backslashes (the engine's FindDisplayClose).
size_t FindDisplayClose(std::string_view s) {
    for (size_t at = s.find("$$"); at != kNpos; at = s.find("$$", at + 1)) {
        if (!EscapedAt(s, at)) return at;
    }
    return kNpos;
}

bool EndsWithBracketClose(std::string_view s) {
    return s.size() >= 2 && s[s.size() - 1] == ']' && s[s.size() - 2] == '\\' && !EscapedAt(s, s.size() - 2);
}

// A thematic break from `from`: three or more of one of `*`, `-`, `_`, blanks between.
bool IsRule(std::string_view s, size_t from) {
    if (from >= s.size()) return false;
    const char c = s[from];
    if (c != '*' && c != '-' && c != '_') return false;
    int marks = 0;
    for (size_t i = from; i < s.size(); i++) {
        if (s[i] == c) marks++;
        else if (!IsBlank(s[i])) return false;
    }
    return marks >= 3;
}

// A table's delimiter row: cells `:?-+:?` between `|`, at least one `|` and one `-`.
bool IsDelimiterRow(std::string_view s) {
    bool pipe = false, dash = false;
    size_t i = SkipBlanks(s, 0);
    if (i < s.size() && s[i] == '|') {
        pipe = true;
        i++;
    }
    for (;;) {
        i = SkipBlanks(s, i);
        if (i >= s.size()) break;
        if (s[i] == ':') i++;
        size_t dashes = 0;
        while (i < s.size() && s[i] == '-') {
            i++;
            dashes++;
        }
        if (dashes == 0) return false;
        dash = true;
        if (i < s.size() && s[i] == ':') i++;
        i = SkipBlanks(s, i);
        if (i >= s.size()) break;
        if (s[i] != '|') return false;
        pipe = true;
        i++;
    }
    return pipe && dash;
}

enum class Kind : uint8_t {
    Blank,
    Text,
    Heading,
    Rule,
    Underline,
    Delimiter,
    Row,
    FenceOpen,
    FenceBody,
    FenceClose,
    MathOpen,
    MathBody,
    MathClose,
    MathLine,
    FrontOpen,
    FrontBody,
    FrontClose,
    Definition,
};

// A line's block syntax, from its first bytes and the state it starts in.
struct LineClass {
    Kind kind = Kind::Text;
    uint16_t next = 0;        // the state after the line
    size_t quoteEnd = 0;      // [0, quoteEnd): quote markers
    size_t listStart = 0;     // [listStart, markerEnd): a list marker and its task box
    size_t markerEnd = 0;
    bool quote = false;
    size_t content = 0;       // where the line's text starts (after its block markers)
    size_t a = 0, b = 0;      // kind-specific: a fence's run, a formula's delimiters, a label's brackets
    bool footnote = false;    // Definition: `[^label]:`
};

// `p` is the line's first bytes (at most kBlockPrefixBytes), `n` its length. `frontOpen`: line 0
// opens a front matter that closes (the caller looked ahead).
LineClass Classify(uint16_t s, std::string_view p, size_t n, bool frontOpen) {
    LineClass c;
    const bool cut = n > p.size();
    const uint16_t mode = s & kModeMask;
    if (mode == kFront) {
        const std::string_view t = Trim(p);
        if (!cut && (t == "---" || t == "...")) {
            c.kind = Kind::FrontClose;
            c.next = 0;
        } else {
            c.kind = Kind::FrontBody;
            c.next = s;
        }
        return c;
    }
    const markdown::LinePrefix pre = markdown::ParseLinePrefix(p);
    c.quoteEnd = pre.quoteEnd;
    c.markerEnd = pre.markerEnd;
    c.listStart = pre.list ? pre.quoteEnd + pre.indent : pre.markerEnd;
    c.quote = pre.quote;
    const size_t i = SkipBlanks(p, pre.markerEnd);
    c.content = i;

    if (mode == kFence) {
        const char fence = (s & kTilde) ? '~' : '`';
        const size_t open = (s & kLenMask) >> kLenShift;
        size_t k = i;
        while (k < p.size() && p[k] == fence) k++;
        const size_t run = std::min(k - i, MarkdownHighlighter::kMaxFenceLength);
        if (!cut && k - i >= 3 && run >= open && AllBlank(p.substr(k))) {
            c.kind = Kind::FenceClose;
            c.a = i;
            c.b = k;
            c.next = 0;
        } else {
            c.kind = Kind::FenceBody;
            c.next = s;
        }
        return c;
    }

    const auto isFence = [&] {
        if (i >= p.size() || (p[i] != '`' && p[i] != '~')) return false;
        size_t k = i;
        while (k < p.size() && p[k] == p[i]) k++;
        return k - i >= 3 && (p[i] != '`' || p.substr(k).find('`') == kNpos);
    };

    if (mode == kMath) {
        // As the engine's search for the closing line: a blank line or a fence ends the attempt,
        // and so does a second `\[` or a `$$` with text after it; then the line is read anew.
        const std::string_view t = Trim(p.substr(pre.markerEnd));
        bool ends = AllBlank(p) || isFence();
        if (!ends) {
            if (s & kBracketMath) {
                if (t == "\\[") {
                    ends = true;
                } else if (!cut && EndsWithBracketClose(t)) {
                    c.kind = Kind::MathClose;
                    c.a = static_cast<size_t>(t.data() - p.data()) + t.size() - 2;
                    c.next = 0;
                    return c;
                }
            } else {
                const size_t close = FindDisplayClose(t);
                if (close != kNpos) {
                    if (!cut && AllBlank(t.substr(close + 2))) {
                        c.kind = Kind::MathClose;
                        c.a = static_cast<size_t>(t.data() - p.data()) + close;
                        c.next = 0;
                        return c;
                    }
                    ends = true;
                }
            }
        }
        if (!ends) {
            c.kind = Kind::MathBody;
            c.next = s;
            return c;
        }
        s = 0;
    }

    if (AllBlank(p)) {
        c.kind = Kind::Blank;
        c.next = 0;
        return c;
    }
    if (frontOpen) {
        c.kind = Kind::FrontOpen;
        c.next = kFront;
        return c;
    }
    // A fence.
    if (isFence()) {
        size_t k = i;
        while (k < p.size() && p[k] == p[i]) k++;
        const size_t info = SkipBlanks(p, k);
        size_t word = info;
        while (word < p.size() && !IsBlank(p[word])) word++;
        const std::string_view language = p.substr(info, word - info);
        c.kind = Kind::FenceOpen;
        c.a = i;
        c.b = k;
        c.next = static_cast<uint16_t>(kFence | (p[i] == '~' ? kTilde : 0) |
                                       (std::min(k - i, MarkdownHighlighter::kMaxFenceLength) << kLenShift) |
                                       (pre.quote ? kQuoted : 0) | (language == "math" ? kMathFence : 0));
        return c;
    }
    // Display math.
    const std::string_view t = Trim(p.substr(pre.markerEnd));
    const size_t tAt = static_cast<size_t>(t.data() - p.data());
    if (StartsWith(t, "$$")) {
        const std::string_view rest = t.substr(2);
        const size_t close = FindDisplayClose(rest);
        if (close == kNpos) {
            c.kind = Kind::MathOpen;
            c.a = tAt;
            c.next = static_cast<uint16_t>(kMath | (pre.quote ? kQuoted : 0));
            return c;
        }
        if (!cut && !Trim(rest.substr(0, close)).empty() && AllBlank(rest.substr(close + 2))) {
            c.kind = Kind::MathLine;
            c.a = tAt;
            c.b = tAt + 2 + close;
            c.next = 0;
            return c;
        }
    } else if (StartsWith(t, "\\[")) {
        if (t.size() == 2) {
            c.kind = Kind::MathOpen;
            c.a = tAt;
            c.next = static_cast<uint16_t>(kMath | kBracketMath | (pre.quote ? kQuoted : 0));
            return c;
        }
        // The padded one-line form only, as the engine reads it.
        if (!cut && t.size() >= 7 && EndsWithBracketClose(t) && IsBlank(t[2]) && IsBlank(t[t.size() - 3]) &&
            !Trim(t.substr(2, t.size() - 4)).empty()) {
            c.kind = Kind::MathLine;
            c.a = tAt;
            c.b = tAt + t.size() - 2;
            c.next = 0;
            return c;
        }
    }
    // An ATX heading.
    if (i < p.size() && p[i] == '#') {
        size_t k = i;
        while (k < p.size() && p[k] == '#') k++;
        if (k - i <= 6 && (k == p.size() || IsBlank(p[k]))) {
            c.kind = Kind::Heading;
            c.a = i;
            c.content = SkipBlanks(p, k);
            c.next = 0;
            return c;
        }
    }
    // A setext underline below paragraph text.
    if ((s & kPara) && !pre.list) {
        const size_t q = SkipBlanks(p, pre.quoteEnd);
        if (q < p.size() && (p[q] == '=' || p[q] == '-')) {
            size_t k = q;
            while (k < p.size() && p[k] == p[q]) k++;
            if (!cut && AllBlank(p.substr(k))) {
                c.kind = Kind::Underline;
                c.next = 0;
                return c;
            }
        }
    }
    // A table's delimiter row below text holding a `|`.
    if ((s & kPara) && (s & kPipe) && !cut && IsDelimiterRow(p.substr(pre.quoteEnd))) {
        c.kind = Kind::Delimiter;
        c.next = kTable;
        return c;
    }
    if (!cut && IsRule(p, i)) {
        c.kind = Kind::Rule;
        c.next = 0;
        return c;
    }
    if (s & kTable) {
        c.kind = Kind::Row;
        c.next = kTable;
        return c;
    }
    // A link reference or footnote definition, which cannot interrupt a paragraph.
    if (!(s & kPara) && i < p.size() && p[i] == '[') {
        size_t k = i + 1;
        const size_t limit = std::min(p.size(), i + 1 + kMaxLabelBytes);
        while (k < limit && p[k] != ']' && p[k] != '[') {
            if (p[k] == '\\') k++;
            k++;
        }
        if (k + 1 < p.size() && k > i + 1 && p[k] == ']' && p[k + 1] == ':') {
            c.kind = Kind::Definition;
            c.a = i;
            c.b = k;
            c.footnote = p[i + 1] == '^';
            c.next = 0;
            return c;
        }
    }
    c.kind = Kind::Text;
    // A list item's first line is no setext heading's text.
    c.next = pre.list ? 0 : static_cast<uint16_t>(kPara | (p.find('|') != kNpos ? kPipe : 0));
    return c;
}

// ---- Inline syntax ----

bool IsUnicodePunct(uint32_t cp) {
    return (cp >= 0xA1 && cp <= 0xBF) || cp == 0xD7 || cp == 0xF7 || (cp >= 0x2010 && cp <= 0x2027) ||
           (cp >= 0x2030 && cp <= 0x205E) || (cp >= 0x3001 && cp <= 0x3003) || (cp >= 0x3008 && cp <= 0x3011) ||
           (cp >= 0x3014 && cp <= 0x301F) || (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20);
}

// The code point that ends at `end` (exclusive) or starts at `at`, for flanking; line edges are
// white space.
uint32_t CodePointBefore(std::string_view s, size_t end) {
    if (end == 0) return ' ';
    size_t start = end - 1;
    while (start > 0 && end - start < 4 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
    const unsigned char lead = static_cast<unsigned char>(s[start]);
    if (lead < 0x80) return lead;
    uint32_t cp = 0;
    size_t len = 0;
    if (!RayoMd::Text::DecodeUtf8(s, start, cp, len) || start + len != end) return 0xFFFD;
    return cp;
}

uint32_t CodePointAt(std::string_view s, size_t at) {
    if (at >= s.size()) return ' ';
    const unsigned char lead = static_cast<unsigned char>(s[at]);
    if (lead < 0x80) return lead;
    uint32_t cp = 0;
    size_t len = 0;
    if (!RayoMd::Text::DecodeUtf8(s, at, cp, len)) return 0xFFFD;
    return cp;
}

bool IsSpaceCp(uint32_t cp) { return cp < 0x80 ? (cp == ' ' || cp == '\t' || cp == '\n') : text::IsWhiteSpace(cp); }
bool IsPunctCp(uint32_t cp) { return cp < 0x80 ? IsAsciiPunct(static_cast<unsigned char>(cp)) : IsUnicodePunct(cp); }

// Bytes the inline scan stops at; the others are text.
struct StopTable {
    bool stop[256] = {};
    constexpr StopTable() {
        for (const char c : {'\\', '`', '$', '<', '!', '[', ']', '*', '_', '~', 'h'}) stop[static_cast<unsigned char>(c)] = true;
    }
};
constexpr StopTable kStops;

} // namespace

// The inline syntax of one line's bytes [from, to), into roles (one per byte of the line). Linear
// in the bytes: every look-ahead stops at the next byte that could close it, uses a table made
// once (backtick runs, parentheses, white space) or a cached search that moves forward only.
struct InlineScan {
    InlineScan(MarkdownHighlighter& highlighter, std::string_view line, uint8_t* lineRoles, size_t begin, size_t end,
               uint8_t baseRole, bool wikiLinks)
        : h(highlighter), s(line), roles(lineRoles), from(begin), to(end), base(baseRole), wiki(wikiLinks) {}

    MarkdownHighlighter& h;
    std::string_view s;
    uint8_t* roles;
    size_t from, to;
    uint8_t base;
    bool wiki;

    size_t tick = 0;              // the first backtick run not before the scan
    bool tables = false;          // parenClose_ and nextSpace_ made
    size_t inactiveBelow = 0;     // link openers below this index of brackets_ are inactive
    // Cached forward searches: where the last search started and what it found.
    struct Cache {
        size_t from = kNpos;
        size_t found = kNpos;
    };
    Cache comment, wikiClose, titleClose[3];

    void Fill(size_t a, size_t b, SyntaxRole role) {
        for (size_t k = a; k < b && k < to; k++) roles[k] = static_cast<uint8_t>(role);
    }
    // Marks [a, b) as `role` where it still has the base role.
    void FillBase(size_t a, size_t b, SyntaxRole role) {
        for (size_t k = a; k < b && k < to; k++) {
            if (roles[k] == base) roles[k] = static_cast<uint8_t>(role);
        }
    }

    // `needle` from `at` on, through `cache`: searches start at increasing positions, so a found
    // position at or after `at` answers again, and so does a search that found nothing.
    size_t CachedFind(Cache& cache, std::string_view needle, size_t at) {
        if (cache.from != kNpos && cache.from <= at && (cache.found == kNpos || cache.found >= at)) return cache.found;
        cache.from = at;
        const size_t found = s.substr(0, to).find(needle, at);
        cache.found = found;
        return found;
    }
    size_t CachedClose(Cache& cache, char ch, size_t at) {
        if (cache.from != kNpos && cache.from <= at && (cache.found == kNpos || cache.found >= at)) return cache.found;
        cache.from = at;
        size_t k = at;
        while (k < to && s[k] != ch) k += s[k] == '\\' ? 2 : 1;
        cache.found = k < to ? k : kNpos;
        return cache.found;
    }

    void Prepare() {
        // Backtick runs; each one's next run of the same length (runs over 255 never match).
        h.ticks_.clear();
        for (size_t k = from; k < to;) {
            if (s[k] != '`') {
                const void* next = std::memchr(s.data() + k, '`', to - k);
                if (!next) break;
                k = static_cast<size_t>(static_cast<const char*>(next) - s.data());
            }
            size_t e = k;
            while (e < to && s[e] == '`') e++;
            h.ticks_.push_back({static_cast<uint32_t>(k), static_cast<uint32_t>(e - k), -1});
            k = e;
        }
        if (h.ticks_.size() > 1) {
            h.lastTick_.assign(256, -1);
            for (size_t r = h.ticks_.size(); r-- > 0;) {
                const uint32_t len = h.ticks_[r].len;
                if (len > 255) continue;
                h.ticks_[r].next = h.lastTick_[len];
                h.lastTick_[len] = static_cast<int32_t>(r);
            }
        }
    }

    // The backtick run starting at `at` (a run start the scan reached), its index.
    size_t TickAt(size_t& cursor, size_t at) const {
        while (cursor < h.ticks_.size() && h.ticks_[cursor].pos < at) cursor++;
        return cursor;
    }

    // Parentheses' partners and the next white space, for link destinations.
    void MakeTables() {
        tables = true;
        h.parenClose_.assign(to, -1);
        h.nextSpace_.assign(to + 1, static_cast<int32_t>(to));
        h.stack_.clear();
        for (size_t k = from; k < to; k++) {
            const char c = s[k];
            if (c == '\\') {
                k++;
                continue;
            }
            if (c == '(') {
                h.stack_.push_back(static_cast<int32_t>(k));
            } else if (c == ')' && !h.stack_.empty()) {
                h.parenClose_[static_cast<size_t>(h.stack_.back())] = static_cast<int32_t>(k);
                h.stack_.pop_back();
            }
        }
        for (size_t k = to; k-- > from;) {
            const unsigned char c = static_cast<unsigned char>(s[k]);
            h.nextSpace_[k] = (c <= ' ') ? static_cast<int32_t>(k) : h.nextSpace_[k + 1];
        }
    }

    // The first closing delimiter of a formula's body from `at` (the engine's FindMathClose): a
    // backslash and the byte after it are one unit, code spans are opaque. kind: 0 `$`, 1 `$$`,
    // 2 `\)`.
    size_t MathClose(size_t at, int kind) {
        size_t cursor = tick;
        for (size_t k = at; k < to;) {
            const char c = s[k];
            if (c == '\\') {
                if (kind == 2 && k + 1 < to) {
                    if (s[k + 1] == ')') return k;
                    if (s[k + 1] == '(') return kNpos;
                }
                k += 2;
                continue;
            }
            if (c == '`') {
                const size_t r = TickAt(cursor, k);
                const int32_t close = r < h.ticks_.size() && h.ticks_[r].pos == k ? h.ticks_[r].next : -1;
                const size_t len = r < h.ticks_.size() && h.ticks_[r].pos == k ? h.ticks_[r].len : 1;
                k = close >= 0 ? h.ticks_[static_cast<size_t>(close)].pos + len : k + len;
                continue;
            }
            if (c == '$') {
                if (kind == 0) return k;
                if (kind == 1 && k + 1 < to && s[k + 1] == '$') return k;
            }
            k++;
        }
        return kNpos;
    }

    // A formula at `i` ('$' or the backslash of "\("): its end, or kNpos with `literal` the end of
    // the bytes that stay text.
    size_t Formula(size_t i, size_t& literal) {
        const auto paint = [&](size_t open, size_t bodyStart, size_t bodyEnd, size_t close, size_t cap) {
            Fill(open, bodyStart, SyntaxRole::Marker);
            Fill(bodyStart, bodyEnd, bodyEnd - bodyStart > cap ? SyntaxRole::Code : SyntaxRole::Math);
            Fill(bodyEnd, close, SyntaxRole::Marker);
        };
        if (s[i] == '$') {
            if (i + 1 < to && s[i + 1] == '$') {
                literal = i + 2;
                const size_t close = MathClose(i + 2, 1);
                if (close == kNpos) return kNpos;
                if (Trim(s.substr(i + 2, close - (i + 2))).empty()) {
                    literal = close + 2;
                    return kNpos;
                }
                paint(i, i + 2, close, close + 2, kMaxDisplayMathBytes);
                return close + 2;
            }
            literal = i + 1;
            if (i + 1 >= to || IsBlank(s[i + 1])) return kNpos;
            const size_t close = MathClose(i + 1, 0);
            if (close == kNpos || IsBlank(s[close - 1])) return kNpos;
            if (close + 1 < to && IsDigit(s[close + 1])) return kNpos;
            paint(i, i + 1, close, close + 1, kMaxInlineMathBytes);
            return close + 1;
        }
        literal = i;
        if (i + 2 >= to || s[i + 1] != '(' || !IsBlank(s[i + 2])) return kNpos;
        const size_t close = MathClose(i + 2, 2);
        if (close == kNpos || !IsBlank(s[close - 1]) || Trim(s.substr(i + 2, close - (i + 2))).empty()) return kNpos;
        paint(i, i + 2, close, close + 2, kMaxInlineMathBytes);
        return close + 2;
    }

    // An autolink or HTML comment at `i` ('<'): its end, or kNpos.
    size_t Angle(size_t i) {
        if (s.substr(i, 4) == "<!--") {
            const size_t close = CachedFind(comment, "-->", i + 4);
            if (close == kNpos) return kNpos;
            Fill(i, close + 3, SyntaxRole::Comment);
            return close + 3;
        }
        size_t k = i + 1;
        if (k < to && IsAlpha(s[k])) {
            size_t e = k + 1;
            while (e < to && e - k < kMaxSchemeBytes && (IsAlnum(s[e]) || s[e] == '+' || s[e] == '.' || s[e] == '-')) e++;
            if (e < to && s[e] == ':' && e - k >= 2) {
                e++;
                while (e < to && static_cast<unsigned char>(s[e]) > ' ' && s[e] != '<' && s[e] != '>') e++;
                if (e < to && s[e] == '>') {
                    Fill(i, e + 1, SyntaxRole::Url);
                    return e + 1;
                }
                return kNpos;
            }
        }
        // An email address.
        size_t e = k;
        while (e < to && (IsAlnum(s[e]) || std::strchr(".!#$%&'*+/=?^_`{|}~-", s[e]) != nullptr) && s[e] != '\0') e++;
        if (e == k || e >= to || s[e] != '@') return kNpos;
        const size_t domain = ++e;
        while (e < to && (IsAlnum(s[e]) || s[e] == '-' || s[e] == '.')) e++;
        if (e == domain || e >= to || s[e] != '>') return kNpos;
        Fill(i, e + 1, SyntaxRole::Url);
        return e + 1;
    }

    // `[[target|alias]]` at `i` (after the `!` of an embed at `bang`): its end, or kNpos.
    size_t WikiLink(size_t bang, size_t i) {
        const size_t close = CachedFind(wikiClose, "]]", i + 2);
        if (close == kNpos || close == i + 2) return kNpos;
        Fill(bang, i + 2, SyntaxRole::Marker);
        for (size_t k = i + 2; k < close; k++) roles[k] = static_cast<uint8_t>(s[k] == '|' ? SyntaxRole::Marker : SyntaxRole::Link);
        Fill(close, close + 2, SyntaxRole::Marker);
        return close + 2;
    }

    // An inline link's `(destination "title")` from the `(` at `open`: the end after its `)`, or
    // kNpos. [destA, destB) is the destination.
    size_t Destination(size_t open, size_t& destA, size_t& destB) {
        if (!tables) MakeTables();
        size_t j = SkipBlanks(s.substr(0, to), open + 1);
        destA = destB = j;
        if (j < to && s[j] == '<') {
            size_t k = j + 1;
            while (k < to && s[k] != '>' && s[k] != '<') k += s[k] == '\\' ? 2 : 1;
            if (k >= to || s[k] != '>') return kNpos;
            destB = k + 1;
            j = k + 1;
        } else {
            const int32_t match = h.parenClose_[open];
            const size_t space = static_cast<size_t>(h.nextSpace_[j]);
            if (match >= 0 && static_cast<size_t>(match) < space) {
                destB = static_cast<size_t>(match);
                return destB + 1;
            }
            destB = space;
            j = space;
        }
        j = SkipBlanks(s.substr(0, to), j);
        if (j < to && s[j] == ')') return j + 1;
        if (j >= to || (s[j] != '"' && s[j] != '\'' && s[j] != '(')) return kNpos;
        const char closeChar = s[j] == '(' ? ')' : s[j];
        const size_t q = CachedClose(titleClose[s[j] == '"' ? 0 : (s[j] == '\'' ? 1 : 2)], closeChar, j + 1);
        if (q == kNpos) return kNpos;
        const size_t end = SkipBlanks(s.substr(0, to), q + 1);
        if (end >= to || s[end] != ')') return kNpos;
        destB = q + 1;
        return end + 1;
    }

    // `]` at `i`: a link or image with the innermost open bracket, or text.
    size_t CloseBracket(size_t i) {
        auto& stack = h.brackets_;
        if (stack.empty()) return i + 1;
        const size_t index = stack.size() - 1;
        const MarkdownHighlighter::Bracket b = stack.back();
        stack.pop_back();
        if (inactiveBelow > stack.size()) inactiveBelow = stack.size();
        if (!b.active || (!b.image && index < inactiveBelow)) return i + 1;
        size_t end = kNpos, destA = 0, destB = 0;
        size_t refA = 0, refB = 0;
        if (i + 1 < to && s[i + 1] == '(') {
            end = Destination(i + 1, destA, destB);
        } else if (i + 1 < to && s[i + 1] == '[') {
            size_t k = i + 2;
            const size_t limit = std::min(to, i + 2 + kMaxLabelBytes);
            while (k < limit && s[k] != ']' && s[k] != '[') k += s[k] == '\\' ? 2 : 1;
            if (k < to && s[k] == ']') {
                refA = i + 2;
                refB = k;
                end = k + 1;
            }
        }
        if (end == kNpos) return i + 1;
        const size_t text = b.pos + (b.image ? 2 : 1);
        // Emphasis inside the link text pairs up only there.
        ProcessEmphasis(b.delims);
        h.delims_.resize(b.delims);
        Fill(b.pos, text, SyntaxRole::Marker);
        FillBase(text, i, SyntaxRole::Link);
        Fill(i, i + 2, SyntaxRole::Marker);
        if (s[i + 1] == '[') {
            Fill(refA, refB, SyntaxRole::Link);
            Fill(refB, end, SyntaxRole::Marker);
        } else {
            // The destination and its title (Destination's destB ends after it).
            Fill(i + 2, destA, SyntaxRole::Marker);
            Fill(destA, destB, SyntaxRole::Url);
            Fill(destB, end, SyntaxRole::Marker);
        }
        // A link holds no link: the openers before it can no longer make one.
        if (!b.image) inactiveBelow = stack.size();
        return end;
    }

    // A run of `*`, `_` or `~` at `i`: pushed as a delimiter; its end.
    size_t DelimiterRun(size_t i) {
        const char c = s[i];
        size_t k = i;
        while (k < to && s[k] == c) k++;
        const size_t len = k - i;
        if (c == '~' && len > 2) return k;
        const uint32_t before = i > 0 ? CodePointBefore(s, i) : ' ';
        const uint32_t after = CodePointAt(s.substr(0, to), k);
        const bool beforeSpace = IsSpaceCp(before), afterSpace = IsSpaceCp(after);
        const bool beforePunct = IsPunctCp(before), afterPunct = IsPunctCp(after);
        const bool left = !afterSpace && (!afterPunct || beforeSpace || beforePunct);
        const bool right = !beforeSpace && (!beforePunct || afterSpace || afterPunct);
        bool canOpen = left, canClose = right;
        if (c == '_') {
            canOpen = left && (!right || beforePunct);
            canClose = right && (!left || afterPunct);
        }
        if (!canOpen && !canClose) return k;
        const uint16_t n = static_cast<uint16_t>(std::min<size_t>(len, 0xFFFF));
        const int32_t prev = static_cast<int32_t>(h.delims_.size()) - 1;
        h.delims_.push_back({static_cast<uint32_t>(i), n, n, c, canOpen, canClose, prev});
        return k;
    }

    // CommonMark's process emphasis over delims_[bottom..]: matched delimiter bytes become Marker.
    // Linear: closers look back over a list without the delimiters removed, and no further than
    // where the last fruitless search for their kind stopped (openers_bottom).
    void ProcessEmphasis(size_t bottom) {
        auto& d = h.delims_;
        const int32_t floor = static_cast<int32_t>(bottom) - 1;
        int32_t openersBottom[3][3][2];
        for (auto& a : openersBottom) {
            for (auto& b : a) b[0] = b[1] = floor;
        }
        for (size_t c = bottom; c < d.size();) {
            MarkdownHighlighter::Delim& closer = d[c];
            if (!closer.canClose || closer.len == 0) {
                c++;
                continue;
            }
            const int kind = closer.ch == '*' ? 0 : (closer.ch == '_' ? 1 : 2);
            int32_t& stop = kind == 2 ? openersBottom[2][closer.orig][0] : openersBottom[kind][closer.orig % 3][closer.canOpen ? 1 : 0];
            int32_t o = closer.prev;
            int32_t found = -1;
            while (o > stop && o > floor) {
                const MarkdownHighlighter::Delim& opener = d[static_cast<size_t>(o)];
                if (opener.ch == closer.ch && opener.canOpen && opener.len > 0) {
                    if (kind == 2) {
                        if (opener.len == closer.len) {
                            found = o;
                            break;
                        }
                    } else {
                        const bool odd = (opener.canClose || closer.canOpen) && (opener.orig + closer.orig) % 3 == 0 &&
                                         !(opener.orig % 3 == 0 && closer.orig % 3 == 0);
                        if (!odd) {
                            found = o;
                            break;
                        }
                    }
                }
                o = opener.prev;
            }
            if (found < 0) {
                stop = closer.prev;
                if (!closer.canOpen) {
                    closer.len = 0;
                    if (c + 1 < d.size()) d[c + 1].prev = closer.prev;
                }
                c++;
                continue;
            }
            MarkdownHighlighter::Delim& opener = d[static_cast<size_t>(found)];
            const uint16_t use = kind == 2 ? closer.len : (opener.len >= 2 && closer.len >= 2 ? 2 : 1);
            Fill(opener.start + opener.len - use, opener.start + opener.len, SyntaxRole::Marker);
            Fill(closer.start, closer.start + use, SyntaxRole::Marker);
            opener.len = static_cast<uint16_t>(opener.len - use);
            closer.len = static_cast<uint16_t>(closer.len - use);
            closer.start += use;
            closer.prev = opener.len == 0 ? opener.prev : found;
            if (closer.len == 0) {
                if (c + 1 < d.size()) d[c + 1].prev = closer.prev;
                c++;
            }
        }
    }

    // A bare URL at `i` (`http://` or `https://`, as the engine links them): its end, or kNpos.
    size_t BareUrl(size_t i) {
        if (i > from) {
            const char p = s[i - 1];
            if (!IsBlank(p) && p != '*' && p != '_' && p != '~' && p != '(') return kNpos;
        }
        const std::string_view rest = s.substr(i, to - i);
        size_t k;
        if (StartsWith(rest, "https://")) k = i + 8;
        else if (StartsWith(rest, "http://")) k = i + 7;
        else return kNpos;
        if (k >= to || !IsAlnum(s[k])) return kNpos;
        size_t e = k;
        while (e < to && static_cast<unsigned char>(s[e]) > ' ' && s[e] != '<') e++;
        // Trailing punctuation is not part of it, nor a `)` without its `(`.
        for (;;) {
            if (e <= k) break;
            const char last = s[e - 1];
            if (std::strchr("?!.,:*_~'\";", last) != nullptr) {
                e--;
                continue;
            }
            if (last == ')') {
                long balance = 0;
                for (size_t q = i; q < e; q++) balance += s[q] == '(' ? 1 : (s[q] == ')' ? -1 : 0);
                if (balance < 0) {
                    e--;
                    continue;
                }
            }
            break;
        }
        Fill(i, e, SyntaxRole::Url);
        return e;
    }

    void Run() {
        Prepare();
        h.delims_.clear();
        h.brackets_.clear();
        size_t i = from;
        while (i < to) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (!kStops.stop[c]) {
                i++;
                continue;
            }
            switch (c) {
            case '\\': {
                if (i + 1 < to && s[i + 1] == '(') {
                    size_t literal;
                    const size_t end = Formula(i, literal);
                    if (end != kNpos) {
                        i = end;
                        break;
                    }
                }
                if (i + 1 >= to) {
                    roles[i] = static_cast<uint8_t>(SyntaxRole::Marker);   // a hard line break
                    i++;
                } else if (IsAsciiPunct(static_cast<unsigned char>(s[i + 1]))) {
                    roles[i] = static_cast<uint8_t>(SyntaxRole::Marker);
                    i += 2;
                } else {
                    i++;
                }
                break;
            }
            case '`': {
                const size_t r = TickAt(tick, i);
                const auto& ticks = h.ticks_;
                const size_t len = r < ticks.size() && ticks[r].pos == i ? ticks[r].len : 1;
                const int32_t close = r < ticks.size() && ticks[r].pos == i ? ticks[r].next : -1;
                if (close < 0) {
                    i += len;
                    break;
                }
                const size_t closeAt = ticks[static_cast<size_t>(close)].pos;
                Fill(i, i + len, SyntaxRole::Marker);
                Fill(i + len, closeAt, SyntaxRole::Code);
                Fill(closeAt, closeAt + len, SyntaxRole::Marker);
                i = closeAt + len;
                break;
            }
            case '$': {
                size_t literal;
                const size_t end = Formula(i, literal);
                i = end != kNpos ? end : literal;
                break;
            }
            case '<': {
                const size_t end = Angle(i);
                i = end != kNpos ? end : i + 1;
                break;
            }
            case '!':
                if (i + 1 < to && s[i + 1] == '[') {
                    if (wiki && i + 2 < to && s[i + 2] == '[') {
                        const size_t end = WikiLink(i, i + 1);
                        if (end != kNpos) {
                            i = end;
                            break;
                        }
                    }
                    h.brackets_.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(h.delims_.size()), true, true});
                    i += 2;
                } else {
                    i++;
                }
                break;
            case '[': {
                if (wiki && i + 1 < to && s[i + 1] == '[') {
                    const size_t end = WikiLink(i, i);
                    if (end != kNpos) {
                        i = end;
                        break;
                    }
                }
                if (i + 1 < to && s[i + 1] == '^') {
                    size_t k = i + 2;
                    const size_t limit = std::min(to, i + 2 + kMaxLabelBytes);
                    while (k < limit && s[k] != ']' && s[k] != '[' && !IsBlank(s[k])) k++;
                    if (k < to && s[k] == ']' && k > i + 2) {
                        Fill(i, k + 1, SyntaxRole::Link);
                        i = k + 1;
                        break;
                    }
                }
                h.brackets_.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(h.delims_.size()), false, true});
                i++;
                break;
            }
            case ']':
                i = CloseBracket(i);
                break;
            case '*':
            case '_':
            case '~':
                i = DelimiterRun(i);
                break;
            default: {   // 'h'
                const size_t end = BareUrl(i);
                i = end != kNpos ? end : i + 1;
                break;
            }
            }
        }
        ProcessEmphasis(0);
    }
};

// ---- The highlighter ----

std::string_view MarkdownHighlighter::LineBytes(const TextBuffer& buffer, size_t line, size_t limit,
                                                std::string& scratch, size_t& length) const {
    const size_t start = buffer.LineStart(line);
    length = buffer.LineEnd(line) - start;
    std::string_view head, tail;
    buffer.Pieces(start, std::min(length, limit), head, tail);
    if (tail.empty()) return head;
    scratch.assign(head.data(), head.size());
    scratch.append(tail.data(), tail.size());
    return scratch;
}

bool MarkdownHighlighter::FrontMatterCloses(const TextBuffer& buffer, size_t& closeLine) {
    const size_t lines = buffer.LineCount();
    const size_t last = std::min(lines, kFrontMatterLines);
    for (size_t line = 1; line < last; line++) {
        const size_t length = buffer.LineEnd(line) - buffer.LineStart(line);
        work_ += kLineCost;
        if (length > kFrontCloseBytes) continue;
        size_t ignored;
        const std::string_view t = Trim(LineBytes(buffer, line, kFrontCloseBytes, nextScratch_, ignored));
        work_ += length;
        stats_.scannedBytes += length;
        if (t == "---" || t == "...") {
            closeLine = line;
            return true;
        }
    }
    closeLine = kFrontMatterLines - 1;
    return false;
}

uint16_t MarkdownHighlighter::StateAfter(const TextBuffer& buffer, size_t line, uint16_t state, bool scanning) {
    size_t length;
    const std::string_view p = LineBytes(buffer, line, kBlockPrefixBytes, lineScratch_, length);
    work_ += kLineCost + p.size();
    stats_.scannedLines++;
    stats_.scannedBytes += p.size();
    bool frontOpen = false;
    if (line == 0) {
        size_t end = 0;
        if (state == 0 && length == p.size() && Trim(p) == "---") frontOpen = FrontMatterCloses(buffer, end);
        if (scanning) frontMatterEnd_ = Trim(p) == "---" ? end : 0;
    }
    return Classify(state, p, length, frontOpen).next;
}

void MarkdownHighlighter::Reset(const TextBuffer& buffer) {
    // Room for an eighth more lines, so line breaks rarely grow the vector (a copy of it).
    const size_t count = buffer.LineCount() + 1;
    states_.reserve(count + count / 8 + 64);
    states_.assign(count, 0);
    scan_ = 0;
    keeps_.clear();
    frontMatterEnd_ = 0;
    view_.clear();
    viewFirst_ = 0;
}

void MarkdownHighlighter::Clear() {
    std::vector<uint16_t>().swap(states_);
    std::vector<ViewLine>().swap(view_);
    std::vector<ViewLine>().swap(nextView_);
    std::vector<SyntaxRun>().swap(runs_);
    std::vector<SyntaxRun>().swap(nextRuns_);
    std::string().swap(lineScratch_);
    std::string().swap(nextScratch_);
    std::vector<uint8_t>().swap(roles_);
    std::vector<Tick>().swap(ticks_);
    std::vector<int32_t>().swap(lastTick_);
    std::vector<Delim>().swap(delims_);
    std::vector<Bracket>().swap(brackets_);
    std::vector<int32_t>().swap(parenClose_);
    std::vector<int32_t>().swap(nextSpace_);
    std::vector<int32_t>().swap(stack_);
    std::vector<Keep>().swap(keeps_);
    std::vector<Keep>().swap(nextKeeps_);
    scan_ = 0;
    frontMatterEnd_ = 0;
}

void MarkdownHighlighter::OnEdit(const TextBuffer& buffer, const WrapLayout::Edit& edit) {
    (void)buffer;
    if (!Active()) return;
    const size_t f = edit.firstLine, oldLast = edit.oldLastLine, newLast = edit.newLastLine;
    // Lines before the edit keep their states (a front matter's look-ahead reads its lines, so an
    // edit there scans again from line 0). Known states past it and kept stretches become kept
    // stretches, those after the edit shifted; what lay inside the edit is gone.
    const bool front = frontMatterEnd_ > 0 && f <= frontMatterEnd_;
    const size_t known = std::min(scan_, f);
    const size_t restart = front ? 0 : known;
    nextKeeps_.clear();
    const auto keep = [&](size_t from, size_t to) {
        if (from <= restart) from = restart + 1;
        if (from <= to) nextKeeps_.push_back({from, to});
    };
    if (front && known >= 1) keep(1, known);
    const auto split = [&](size_t from, size_t to) {
        if (from <= f) keep(from, std::min(to, f));
        if (to > oldLast) keep(std::max(from, oldLast + 1) + newLast - oldLast, to + newLast - oldLast);
    };
    if (scan_ > oldLast) split(oldLast + 1, scan_);
    for (const Keep& k : keeps_) split(k.from, k.to);
    std::sort(nextKeeps_.begin(), nextKeeps_.end(), [](const Keep& a, const Keep& b) { return a.from < b.from; });
    // Past kMaxKeeps the farthest go: their lines are scanned again rather than trusted.
    if (nextKeeps_.size() > kMaxKeeps) nextKeeps_.resize(kMaxKeeps);
    keeps_.swap(nextKeeps_);

    if (newLast > oldLast) {
        states_.insert(states_.begin() + static_cast<std::ptrdiff_t>(oldLast + 1), newLast - oldLast, uint16_t{0});
    } else if (oldLast > newLast) {
        states_.erase(states_.begin() + static_cast<std::ptrdiff_t>(newLast + 1),
                      states_.begin() + static_cast<std::ptrdiff_t>(oldLast + 1));
    }
    scan_ = restart;

    // The view: the line before the edit (a setext underline or delimiter row may have come or
    // gone below it) and the edited lines go stale; the lines after them move with the text, so
    // their runs stay (PrepareView still checks each one's states).
    if (view_.empty()) return;
    const size_t lo = f > 0 ? f - 1 : 0;
    const size_t viewLast = viewFirst_ + view_.size() - 1;
    if (viewFirst_ > oldLast) {
        viewFirst_ = viewFirst_ + newLast - oldLast;
        return;
    }
    if (viewLast < lo) return;
    if (f > viewLast) {
        view_.back().valid = false;   // the view's last line is the one above the edit
        return;
    }
    if (viewFirst_ > f || newLast - f > kMaxViewLines) {
        view_.clear();
        return;
    }
    const size_t at = f - viewFirst_;
    for (size_t k = lo > viewFirst_ ? lo - viewFirst_ : 0; k <= at; k++) view_[k].valid = false;
    const auto gone = view_.begin() + static_cast<std::ptrdiff_t>(at + 1);
    view_.erase(gone, gone + static_cast<std::ptrdiff_t>(std::min(oldLast, viewLast) - f));
    view_.insert(view_.begin() + static_cast<std::ptrdiff_t>(at + 1), newLast - f, ViewLine{});
}

bool MarkdownHighlighter::Settle(const TextBuffer& buffer, size_t budget) {
    if (!Pending()) return false;
    const size_t lines = states_.size() - 1;
    const uint64_t start = work_;
    size_t next = 0;   // the first kept stretch not passed (they all lie past scan_)
    while (scan_ < lines && work_ - start < budget) {
        const uint16_t state = StateAfter(buffer, scan_, states_[scan_], true);
        const size_t at = scan_ + 1;
        if (next < keeps_.size() && at >= keeps_[next].from) {
            if (states_[at] == state) {
                // The kept states of the stretch are what a scan would give again.
                scan_ = keeps_[next].to;
                next++;
                continue;
            }
            if (++keeps_[next].from > keeps_[next].to) next++;
        }
        states_[at] = state;
        scan_ = at;
    }
    keeps_.erase(keeps_.begin(), keeps_.begin() + static_cast<std::ptrdiff_t>(next));
    return Pending();
}

void MarkdownHighlighter::SetWikiLinks(bool on) {
    if (on == wikiLinks_) return;
    wikiLinks_ = on;
    for (ViewLine& v : view_) v.valid = false;
}

void MarkdownHighlighter::Tokenize(const TextBuffer& buffer, size_t line, uint16_t state, uint16_t next,
                                   std::vector<SyntaxRun>& out) {
    size_t length;
    std::string_view p = LineBytes(buffer, line, kMaxInlineBytes + 1, lineScratch_, length);
    const bool inline_ = length <= kMaxInlineBytes;
    if (!inline_) p = p.substr(0, kBlockPrefixBytes);
    const std::string_view head = p.substr(0, std::min(p.size(), kBlockPrefixBytes));
    const bool frontOpen = line == 0 && state == 0 && (next & kModeMask) == kFront;
    const LineClass c = Classify(state, head, length, frontOpen);
    stats_.tokenizedLines++;
    stats_.tokenizedBytes += p.size();

    // Text above a setext underline or a table's delimiter row.
    bool headingText = false, headerRow = false;
    if (c.kind == Kind::Text && (c.next & kPara) && line + 2 < states_.size()) {
        size_t nextLength;
        const std::string_view np = LineBytes(buffer, line + 1, kBlockPrefixBytes, nextScratch_, nextLength);
        const Kind below = Classify(c.next, np, nextLength, false).kind;
        headingText = below == Kind::Underline;
        headerRow = below == Kind::Delimiter;
    }

    roles_.assign(p.size(), static_cast<uint8_t>(SyntaxRole::Plain));
    uint8_t* roles = roles_.data();
    const size_t n = p.size();
    const auto fill = [&](size_t a, size_t b, SyntaxRole role) {
        for (size_t k = a; k < b && k < n; k++) roles[k] = static_cast<uint8_t>(role);
    };
    const auto markers = [&] {
        fill(0, c.quoteEnd, SyntaxRole::Marker);
        fill(c.listStart, c.markerEnd, SyntaxRole::Marker);
    };
    // Inline syntax of [from, to) over `base`.
    size_t inlineFrom = n, inlineTo = n;
    SyntaxRole base = SyntaxRole::Plain;
    bool table = false;

    switch (c.kind) {
    case Kind::Blank:
        break;
    case Kind::FrontOpen:
    case Kind::FrontClose:
        fill(0, n, SyntaxRole::Marker);
        break;
    case Kind::FrontBody:
        fill(0, n, SyntaxRole::Info);
        break;
    case Kind::FenceOpen:
        markers();
        fill(c.a, c.b, SyntaxRole::Marker);
        fill(c.b, n, SyntaxRole::Info);
        break;
    case Kind::FenceBody: {
        const size_t from = (state & kQuoted) ? c.quoteEnd : 0;
        fill(0, from, SyntaxRole::Marker);
        fill(from, n, (state & kMathFence) ? SyntaxRole::Math : SyntaxRole::Code);
        break;
    }
    case Kind::FenceClose:
        markers();
        fill(c.a, c.b, SyntaxRole::Marker);
        break;
    case Kind::MathOpen:
        markers();
        fill(c.a, c.a + 2, SyntaxRole::Marker);
        fill(c.a + 2, n, SyntaxRole::Math);
        break;
    case Kind::MathBody: {
        const size_t from = (state & kQuoted) ? c.quoteEnd : 0;
        fill(0, from, SyntaxRole::Marker);
        fill(from, n, SyntaxRole::Math);
        break;
    }
    case Kind::MathClose: {
        const size_t from = (state & kQuoted) ? c.quoteEnd : 0;
        fill(0, from, SyntaxRole::Marker);
        fill(from, c.a, SyntaxRole::Math);
        fill(c.a, c.a + 2, SyntaxRole::Marker);
        break;
    }
    case Kind::MathLine:
        markers();
        fill(c.a, c.a + 2, SyntaxRole::Marker);
        fill(c.a + 2, c.b, SyntaxRole::Math);
        fill(c.b, c.b + 2, SyntaxRole::Marker);
        break;
    case Kind::Heading: {
        markers();
        fill(c.a, c.content, SyntaxRole::Marker);
        size_t end = n;
        if (inline_) {
            // A closing sequence of `#` after a blank, or alone.
            size_t e = n;
            while (e > c.content && IsBlank(p[e - 1])) e--;
            size_t h = e;
            while (h > c.content && p[h - 1] == '#') h--;
            if (h < e && (h == c.content || IsBlank(p[h - 1]))) {
                fill(h, e, SyntaxRole::Marker);
                end = h;
            }
        }
        fill(c.content, end, SyntaxRole::Heading);
        inlineFrom = c.content;
        inlineTo = end;
        base = SyntaxRole::Heading;
        break;
    }
    case Kind::Rule:
    case Kind::Underline:
    case Kind::Delimiter:
        fill(0, n, SyntaxRole::Marker);
        break;
    case Kind::Row:
        markers();
        inlineFrom = c.markerEnd;
        inlineTo = n;
        table = true;
        break;
    case Kind::Definition:
        markers();
        fill(c.a, c.a + 1, SyntaxRole::Marker);
        fill(c.a + 1, c.b, SyntaxRole::Link);
        fill(c.b, c.b + 2, SyntaxRole::Marker);
        if (c.footnote) {
            inlineFrom = c.b + 2;
            inlineTo = n;
        } else {
            size_t k = SkipBlanks(p, c.b + 2);
            while (k < n && !IsBlank(p[k])) roles[k++] = static_cast<uint8_t>(SyntaxRole::Url);
            for (; k < n; k++) {
                if (!IsBlank(p[k])) roles[k] = static_cast<uint8_t>(SyntaxRole::Url);
            }
        }
        break;
    case Kind::Text: {
        markers();
        size_t from = c.markerEnd;
        // A GitHub alert or Obsidian callout: `[!TYPE]` first in a quote.
        if (c.quote && c.content + 2 < n && p[c.content] == '[' && p[c.content + 1] == '!') {
            size_t k = c.content + 2;
            while (k < n && k - c.content < kMaxAlertBytes && (IsAlnum(p[k]) || p[k] == '-')) k++;
            if (k < n && p[k] == ']' && k > c.content + 2) {
                fill(c.content, c.content + 2, SyntaxRole::Marker);
                fill(c.content + 2, k, SyntaxRole::Heading);
                fill(k, k + 1, SyntaxRole::Marker);
                from = k + 1;
            }
        }
        base = headingText || headerRow ? SyntaxRole::Heading : SyntaxRole::Plain;
        fill(from, n, base);
        inlineFrom = from;
        inlineTo = n;
        table = headerRow;
        break;
    }
    }

    // Inline syntax; past kMaxLineRuns runs the line keeps its block roles. A table row is cut
    // into cells first: an unescaped `|` ends a cell even inside a code span (GFM).
    const auto markPipes = [&](bool scan) {
        size_t cell = inlineFrom;
        for (size_t k = inlineFrom; k <= inlineTo; k++) {
            if (k < inlineTo && p[k] == '\\') {
                k++;
                continue;
            }
            if (k < inlineTo && p[k] != '|') continue;
            if (scan && k > cell) InlineScan(*this, p, roles, cell, k, static_cast<uint8_t>(base), wikiLinks_).Run();
            if (k < inlineTo) roles[k] = static_cast<uint8_t>(SyntaxRole::Marker);
            cell = k + 1;
        }
    };
    if (inline_ && inlineFrom < inlineTo) {
        if (table) markPipes(true);
        else InlineScan(*this, p, roles, inlineFrom, inlineTo, static_cast<uint8_t>(base), wikiLinks_).Run();
    }

    const auto compress = [&](std::vector<SyntaxRun>& into, size_t limit) {
        const size_t begin = into.size();
        uint8_t last = 0xFF;
        for (size_t k = 0; k < n; k++) {
            if (roles[k] == last) continue;
            // Blanks draw nothing: they join the run before them.
            if ((p[k] == ' ' || p[k] == '\t') && last != 0xFF) continue;
            if (into.size() - begin == limit) return false;
            into.push_back({static_cast<uint32_t>(k), static_cast<SyntaxRole>(roles[k])});
            last = roles[k];
        }
        return true;
    };
    const size_t begin = out.size();
    if (!compress(out, kMaxLineRuns)) {
        out.resize(begin);
        if (inline_ && inlineFrom < inlineTo) {
            // Again without the inline roles.
            fill(inlineFrom, inlineTo, base);
            if (table) markPipes(false);
            if (!compress(out, kMaxLineRuns)) out.resize(begin);
        }
    }
    // A line of plain text needs no runs.
    if (out.size() == begin + 1 && out[begin].role == SyntaxRole::Plain) out.resize(begin);
}

bool MarkdownHighlighter::LineRuns(const TextBuffer& buffer, size_t line, std::vector<SyntaxRun>& out) {
    out.clear();
    if (!Known(line) || line + 1 >= states_.size()) return false;
    const uint16_t state = states_[line];
    const uint16_t next = Known(line + 1) ? states_[line + 1] : StateAfter(buffer, line, state, false);
    Tokenize(buffer, line, state, next, out);
    return true;
}

bool MarkdownHighlighter::PrepareView(const TextBuffer& buffer, size_t first, size_t last, size_t budget) {
    if (!Active()) return false;
    const size_t lines = states_.size() - 1;
    if (lines == 0 || first >= lines) {
        view_.clear();
        return false;
    }
    last = std::min(last, lines - 1);
    if (last < first) last = first;
    last = std::min(last, first + kMaxViewLines - 1);
    const auto current = [&](const ViewLine& v, size_t line) {
        if (!v.valid || v.known != Known(line)) return false;
        if (!v.known) return true;
        if (v.state != states_[line]) return false;
        return !Known(line + 1) || v.next == states_[line + 1];
    };
    const size_t count = last - first + 1;
    if (first == viewFirst_ && view_.size() == count) {
        bool same = true;
        for (size_t k = 0; k < count && same; k++) same = current(view_[k], first + k);
        if (same) return false;
    }
    nextView_.clear();
    nextRuns_.clear();
    const uint64_t start = stats_.tokenizedBytes;
    bool left = false;
    for (size_t line = first; line <= last; line++) {
        ViewLine v;
        const ViewLine* old = line >= viewFirst_ && line - viewFirst_ < view_.size() ? &view_[line - viewFirst_] : nullptr;
        v.begin = static_cast<uint32_t>(nextRuns_.size());
        if (old && current(*old, line)) {
            nextRuns_.insert(nextRuns_.end(), runs_.begin() + old->begin, runs_.begin() + old->begin + old->count);
            v.count = old->count;
            v.state = old->state;
            v.next = old->next;
            v.known = old->known;
            v.valid = true;
        } else if (!Known(line)) {
            v.valid = true;   // waits for Settle
        } else if (stats_.tokenizedBytes - start >= budget || nextRuns_.size() >= kMaxViewRuns) {
            left = true;
        } else {
            v.state = states_[line];
            v.next = Known(line + 1) ? states_[line + 1] : StateAfter(buffer, line, v.state, false);
            Tokenize(buffer, line, v.state, v.next, nextRuns_);
            if (nextRuns_.size() > kMaxViewRuns) {
                nextRuns_.resize(v.begin);
                left = true;
            } else {
                v.count = static_cast<uint32_t>(nextRuns_.size() - v.begin);
                v.known = true;
                v.valid = true;
            }
        }
        nextView_.push_back(v);
    }
    view_.swap(nextView_);
    runs_.swap(nextRuns_);
    viewFirst_ = first;
    return left;
}

const SyntaxRun* MarkdownHighlighter::Runs(size_t line, size_t& count) const {
    count = 0;
    if (line < viewFirst_ || line - viewFirst_ >= view_.size()) return nullptr;
    const ViewLine& v = view_[line - viewFirst_];
    if (!v.valid || !v.known || v.count == 0) return nullptr;
    count = v.count;
    return runs_.data() + v.begin;
}

size_t MarkdownHighlighter::MemoryBytes() const {
    return states_.capacity() * sizeof(uint16_t) + (view_.capacity() + nextView_.capacity()) * sizeof(ViewLine) +
           (runs_.capacity() + nextRuns_.capacity()) * sizeof(SyntaxRun) + lineScratch_.capacity() +
           nextScratch_.capacity() + roles_.capacity() + ticks_.capacity() * sizeof(Tick) +
           (lastTick_.capacity() + parenClose_.capacity() + nextSpace_.capacity() + stack_.capacity()) * sizeof(int32_t) +
           delims_.capacity() * sizeof(Delim) + brackets_.capacity() * sizeof(Bracket) +
           (keeps_.capacity() + nextKeeps_.capacity()) * sizeof(Keep);
}

} // namespace rgui::ui
