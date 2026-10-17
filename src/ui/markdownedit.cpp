// SPDX-License-Identifier: Apache-2.0
#include "ui/markdownedit.h"

#include <algorithm>

namespace rgui::ui::markdown {

namespace {

bool IsBlank(char c) { return c == ' ' || c == '\t'; }

size_t SkipBlanks(std::string_view s, size_t i) {
    while (i < s.size() && IsBlank(s[i])) i++;
    return i;
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// A thematic break such as `- - -` or `* * *`: the bullet and two more of it, blanks between.
bool IsRule(std::string_view line, size_t from, char bullet) {
    int marks = 0;
    for (size_t i = from; i < line.size(); i++) {
        if (line[i] == bullet) marks++;
        else if (!IsBlank(line[i])) return false;
    }
    return marks >= 3;
}

} // namespace

bool HasText(std::string_view text) {
    for (char c : text) {
        if (!IsBlank(c)) return true;
    }
    return false;
}

LinePrefix ParseLinePrefix(std::string_view line) {
    LinePrefix p;
    const size_t n = line.size();
    // `>` markers, each with the indentation before it and one blank after it.
    size_t at = 0;
    for (;;) {
        const size_t q = SkipBlanks(line, at);
        if (q >= n || line[q] != '>') break;
        at = q + 1;
        if (at < n && IsBlank(line[at])) at++;
        p.quote = true;
    }
    p.quoteEnd = at;
    p.markerEnd = at;
    const size_t m = SkipBlanks(line, at);
    if (m >= n) return p;
    size_t after = 0;   // the byte after the marker
    if (line[m] == '-' || line[m] == '*' || line[m] == '+') {
        after = m + 1;
        p.bullet = line[m];
    } else if (line[m] >= '0' && line[m] <= '9') {
        size_t d = m;
        uint64_t number = 0;
        while (d < n && d - m < 9 && line[d] >= '0' && line[d] <= '9') number = number * 10 + static_cast<uint64_t>(line[d++] - '0');
        if (d >= n || (line[d] != '.' && line[d] != ')')) return p;
        p.ordered = true;
        p.number = number;
        p.bullet = line[d];
        after = d + 1;
    } else {
        return p;
    }
    // The marker needs a blank after it: a lone `-` may underline a heading.
    if (after >= n || !IsBlank(line[after])) {
        p.ordered = false;
        p.bullet = 0;
        p.number = 0;
        return p;
    }
    if (!p.ordered && IsRule(line, m, p.bullet)) {
        p.bullet = 0;
        return p;
    }
    p.list = true;
    p.indent = m - at;
    size_t end = SkipBlanks(line, after);
    if (end + 3 <= n && line[end] == '[' && (line[end + 1] == ' ' || line[end + 1] == 'x' || line[end + 1] == 'X') &&
        line[end + 2] == ']' && (end + 3 == n || IsBlank(line[end + 3]))) {
        p.task = true;
        end = SkipBlanks(line, end + 3);
    }
    p.markerEnd = end;
    return p;
}

bool ContinueOnEnter(std::string_view line, bool whole, size_t caret, TextEdit& edit) {
    const LinePrefix p = ParseLinePrefix(line);
    if (!p.list && !p.quote) return false;
    if (caret < p.markerEnd) return false;
    edit = TextEdit();
    if (whole && !HasText(line.substr(p.markerEnd))) {
        // An empty item ends the list (a quote's own marker stays) or the quote.
        edit.pos = p.list ? p.quoteEnd : 0;
        edit.len = line.size() - edit.pos;
        edit.caret = 0;
        return true;
    }
    std::string next = "\n";
    next.append(line.data(), p.quoteEnd);
    if (p.list) {
        next.append(line.data() + p.quoteEnd, p.indent);
        if (p.ordered) next += std::to_string(p.number + 1);
        next += p.bullet;
        next += ' ';
        if (p.task) next += "[ ] ";
    }
    edit.pos = caret;
    // The blanks after the caret would follow the new marker: they go.
    size_t blanks = caret;
    while (blanks < line.size() && IsBlank(line[blanks])) blanks++;
    edit.len = blanks > caret ? blanks - caret : 0;
    edit.caret = next.size();
    edit.text = std::move(next);
    return true;
}

bool IndentListItem(std::string_view line, size_t caret, bool outdent, TextEdit& edit) {
    const LinePrefix p = ParseLinePrefix(line);
    if (!p.list) return false;
    edit = TextEdit();
    edit.pos = p.quoteEnd;
    const std::string_view indent = line.substr(p.quoteEnd, p.indent);
    const size_t into = caret > p.quoteEnd ? caret - p.quoteEnd : 0;
    if (!outdent) {
        edit.text = indent.find('\t') != std::string_view::npos ? "\t" : "    ";
        edit.caret = into + edit.text.size();
        return true;
    }
    if (indent.empty()) return false;
    size_t removed = 1;
    if (indent[0] == ' ') {
        removed = 0;
        while (removed < indent.size() && removed < 4 && indent[removed] == ' ') removed++;
    }
    edit.len = removed;
    edit.caret = into > removed ? into - removed : 0;
    return true;
}

// ---- Completion ----

Completion CompletionTrigger(std::string_view beforeCaret, bool wikiLinks) {
    const size_t n = beforeCaret.size();
    if (n >= 3 && beforeCaret.substr(n - 3) == "```") {
        const std::string_view head = beforeCaret.substr(0, n - 3);
        const LinePrefix p = ParseLinePrefix(head);
        if (!HasText(head.substr(std::min(p.markerEnd, head.size())))) return Completion::Fence;
    }
    if (n >= 2 && beforeCaret.substr(n - 2) == "[!") {
        const std::string_view head = beforeCaret.substr(0, n - 2);
        const LinePrefix p = ParseLinePrefix(head);
        if (p.quote && !p.list && !HasText(head.substr(p.quoteEnd))) return Completion::Alert;
    }
    if (wikiLinks && n >= 2 && beforeCaret.substr(n - 2) == "[[" &&
        (n == 2 || (beforeCaret[n - 3] != '[' && beforeCaret[n - 3] != '!'))) {
        return Completion::WikiLink;
    }
    return Completion::None;
}

std::string_view TriggerText(Completion kind) {
    switch (kind) {
    case Completion::Fence: return "```";
    case Completion::Alert: return "[!";
    case Completion::WikiLink: return "[[";
    case Completion::None: break;
    }
    return {};
}

bool FilterValid(Completion kind, std::string_view filter) {
    if (filter.size() > kMaxFilterBytes) return false;
    std::string_view refused;
    switch (kind) {
    case Completion::Fence: refused = std::string_view(" \t`~\n", 5); break;
    case Completion::Alert: refused = std::string_view(" \t[]\n", 5); break;
    case Completion::WikiLink: refused = std::string_view("[]|\n", 4); break;
    case Completion::None: return false;
    }
    return filter.find_first_of(refused) == std::string_view::npos;
}

bool Matches(std::string_view item, std::string_view filter) {
    if (filter.empty()) return true;
    if (filter.size() > item.size()) return false;
    for (size_t i = 0; i + filter.size() <= item.size(); i++) {
        size_t k = 0;
        while (k < filter.size() && Lower(item[i + k]) == Lower(filter[k])) k++;
        if (k == filter.size()) return true;
    }
    return false;
}

const std::vector<std::string>& FenceNames() {
    static const std::vector<std::string> names = {
        "bash", "sh", "shell", "zsh", "console", "powershell", "ps1", "pwsh", "bat", "cmd",
        "javascript", "js", "jsx", "typescript", "ts", "tsx", "python", "py", "pycon", "c",
        "cpp", "c++", "csharp", "cs", "java", "kotlin", "kt", "go", "rust", "rs",
        "swift", "php", "ruby", "rb", "sql", "html", "xml", "svg", "css", "scss",
        "json", "jsonc", "yaml", "yml", "toml", "ini", "dockerfile", "diff", "mermaid", "math",
    };
    return names;
}

const std::vector<std::string>& AlertTypes() {
    static const std::vector<std::string> types = {"NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION"};
    return types;
}

// ---- Blocks ----

TextEdit PlaceBlock(std::string_view block, size_t caretInBlock, bool caretAfter, const BlockSite& site) {
    TextEdit edit;
    if (site.textBefore) edit.text = "\n\n";
    else if (site.textAbove) edit.text = "\n";
    const size_t start = edit.text.size();
    edit.text.append(block.data(), block.size());
    if (caretAfter) {
        edit.text += "\n\n";
        edit.caret = edit.text.size();
        return edit;
    }
    if (site.textAfter) edit.text += "\n\n";
    else if (site.atEnd || site.textBelow) edit.text += "\n";
    edit.caret = start + std::min(caretInBlock, block.size());
    return edit;
}

// ---- Headings ----

std::vector<std::string> DocumentHeadings(std::string_view text) {
    std::vector<std::string> headings;
    const size_t n = text.size();
    auto lineEnd = [&](size_t from) {
        const size_t nl = text.find('\n', from);
        return nl == std::string_view::npos ? n : nl;
    };
    auto trimmed = [&](size_t s, size_t e) {
        while (s < e && (IsBlank(text[s]) || text[s] == '\r')) s++;
        while (e > s && (IsBlank(text[e - 1]) || text[e - 1] == '\r')) e--;
        return text.substr(s, e - s);
    };
    size_t pos = 0;
    // The front matter: `---` to `---` or `...`; unclosed, it is none.
    if (trimmed(0, lineEnd(0)) == "---") {
        for (size_t at = lineEnd(0) + 1; at < n;) {
            const size_t e = lineEnd(at);
            const std::string_view line = trimmed(at, e);
            if (line == "---" || line == "...") {
                pos = e;
                break;
            }
            at = e + 1;
        }
    }
    size_t total = 0;
    bool inFence = false;
    char fenceChar = 0;
    size_t fenceLength = 0;
    for (size_t s = pos; s < n && headings.size() < kMaxHeadings;) {
        const size_t e = lineEnd(s);
        size_t i = s;
        while (i < e && i - s < 3 && text[i] == ' ') i++;
        if (i < e && (text[i] == '`' || text[i] == '~')) {
            size_t k = i;
            while (k < e && text[k] == text[i]) k++;
            const size_t run = k - i;
            if (run >= 3) {
                if (!inFence) {
                    // A backtick fence's info string has no backtick.
                    if (text[i] != '`' || text.substr(k, e - k).find('`') == std::string_view::npos) {
                        inFence = true;
                        fenceChar = text[i];
                        fenceLength = run;
                    }
                } else if (text[i] == fenceChar && run >= fenceLength && trimmed(k, e).empty()) {
                    inFence = false;
                }
            }
        } else if (!inFence && i < e && text[i] == '#') {
            size_t k = i;
            while (k < e && text[k] == '#') k++;
            if (k - i <= 6 && (k == e || IsBlank(text[k]) || text[k] == '\r')) {
                std::string_view title = trimmed(k, e);
                // A closing sequence of `#` after a blank (or alone) is not part of the text.
                size_t end = title.size();
                while (end > 0 && title[end - 1] == '#') end--;
                if (end == 0 || IsBlank(title[end - 1])) {
                    while (end > 0 && IsBlank(title[end - 1])) end--;
                    title = title.substr(0, end);
                }
                if (!title.empty()) {
                    size_t keep = std::min(title.size(), kMaxHeadingBytes);
                    while (keep < title.size() && keep > 0 && (static_cast<unsigned char>(title[keep]) & 0xC0) == 0x80) keep--;
                    if (total + keep > kMaxHeadingTotal) break;
                    headings.emplace_back(title.substr(0, keep));
                    total += keep;
                }
            }
        }
        s = e + 1;
    }
    return headings;
}

} // namespace rgui::ui::markdown
