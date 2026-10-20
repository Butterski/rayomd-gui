// SPDX-License-Identifier: Apache-2.0
// bench.highlight: what the editor's syntax colours cost, on the highlighter alone (a TextBuffer,
// no fonts or drawing): the first scan of a document a frame budget at a time, a view of 60 lines
// tokenized cold and checked warm, keystrokes and line breaks in the middle (the edit, its scan and
// the view again), and a fence typed at the top and removed (every line's state changes, then
// returns). Documents: tester.md and copies of it to 100 KiB, 1 MiB and 10 MiB, and hostile ones:
// many short lines, one long line, dense syntax, unmatched openers, a long fence, Unicode with
// combining marks.
#include "runner.h"

#include "common/file_io.h"
#include "ui/markdownhighlight.h"
#include "ui/textbuffer.h"
#include "ui/textwrap.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RGUI_HIGHLIGHT_ASAN 1
#endif
#endif

namespace {

using Clock = std::chrono::steady_clock;
using rgui::ui::MarkdownHighlighter;
using rgui::ui::TextBuffer;
using rgui::ui::WrapLayout;

double MsSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

double Percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    size_t rank = static_cast<size_t>(p * static_cast<double>(values.size()) + 0.999999);
    rank = std::max<size_t>(rank, 1);
    return values[std::min(rank, values.size()) - 1];
}

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

std::string Fill(std::string_view piece, size_t bytes) {
    std::string out;
    out.reserve(bytes + piece.size());
    while (out.size() < bytes) out.append(piece);
    return out;
}

struct Settled {
    int frames = 0;
    double ms = 0;
    std::vector<double> frameMs;
};

// Settle a frame budget at a time, each frame timed.
Settled SettleFrames(MarkdownHighlighter& h, const TextBuffer& buffer) {
    Settled s;
    for (;;) {
        const auto t0 = Clock::now();
        const bool more = h.Settle(buffer, MarkdownHighlighter::kFrameBudget);
        const double ms = MsSince(t0);
        s.frameMs.push_back(ms);
        s.ms += ms;
        s.frames++;
        if (!more) break;
    }
    return s;
}

} // namespace

RGUI_TEST("bench.highlight") {
    std::string tester;
    CHECK(RayoMd::File::ReadAll(RayoMd::File::PathFromUtf8(std::string(rgui::test::SourceDir()) + "/external/rayomd/tester.md"),
                                8u << 20, tester));
    if (tester.empty()) return;
#if defined(__SANITIZE_ADDRESS__) || defined(RGUI_HIGHLIGHT_ASAN)
    const size_t kBig = 1u << 20;   // sanitized builds check the path, not the time
    const int kKeys = 20;
#else
    const size_t kBig = 10u << 20;
    const int kKeys = 400;
#endif
    struct Doc {
        const char* name;
        std::string text;
    };
    std::vector<Doc> docs;
    docs.push_back({"tester.md", tester});
    docs.push_back({"100 KiB", Fill(tester, 100u << 10)});
    docs.push_back({"1 MiB", Fill(tester, 1u << 20)});
    docs.push_back({kBig == (10u << 20) ? "10 MiB" : "1 MiB'", Fill(tester, kBig)});
    docs.push_back({"short lines", Fill("a\n", 2u << 20)});
    docs.push_back({"one long line", Fill("word *em* `code` [l](u) ", 4u << 20)});
    docs.push_back({"dense syntax", Fill("*a* `b` [c](d) $e$ ~~f~~ <g:h> **i** \\* [^j] www.k.l\n", 1u << 20)});
    docs.push_back({"unmatched", Fill(Fill("[a]( $b `c ** <d: ![[e ", 4096) + "\n", 1u << 20)});
    docs.push_back({"long fence", "```c\n" + Fill("int x = *p; // `a` [b](c)\n", 1u << 20) + "```\n"});
    docs.push_back({"unicode", Fill("Zażółć *gęślą* jaźń e\xCC\x81\xCC\x82 \xE4\xB8\xAD\xE6\x96\x87 `\xF0\x9F\x98\x80`\n", 1u << 20)});

    std::printf("bench.highlight: frame budget %zu units, view budget %zu bytes, %d keystrokes\n",
                MarkdownHighlighter::kFrameBudget, MarkdownHighlighter::kViewBudget, kKeys);
    std::printf("  %-14s %9s %8s | %-28s | %-15s | %-23s | %-23s | %-25s | %9s\n", "document", "bytes", "lines",
                "first scan: frames ms, frame p50/max", "view cold/warm", "key p50/p99/max ms", "Enter p50/p99/max ms",
                "fence typed/removed ms", "cache KiB");
    for (Doc& doc : docs) {
        TextBuffer buffer;
        buffer.SetText(doc.text);
        MarkdownHighlighter h;
        h.Reset(buffer);
        const Settled first = SettleFrames(h, buffer);
        CHECK(!h.Pending());

        // A view of 60 lines in the middle, cold (the slowest of the frames its budget takes) then
        // warm (nothing changed: no work).
        const size_t mid = buffer.LineCount() / 2;
        double cold = 0;
        for (bool more = true; more;) {
            const auto t0 = Clock::now();
            more = h.PrepareView(buffer, mid, mid + 59, MarkdownHighlighter::kViewBudget);
            cold = std::max(cold, MsSince(t0));
        }
        const uint64_t tokenized = h.Stats().tokenizedLines;
        auto t0 = Clock::now();
        h.PrepareView(buffer, mid, mid + 59, MarkdownHighlighter::kViewBudget);
        const double warm = MsSince(t0);
        CHECK_EQ(h.Stats().tokenizedLines, tokenized);

        // Keystrokes at the middle line's end, each a frame's work: the edit, the scan, the view.
        const size_t memoryBefore = h.MemoryBytes();
        std::vector<double> keys, enters;
        const char typed[] = "The quick brown fox *jumps* over `the` lazy dog. ";
        size_t at = buffer.LineEnd(mid);
        for (int k = 0; k < kKeys; k++) {
            t0 = Clock::now();
            h.OnEdit(buffer, Apply(buffer, at, 0, std::string_view(typed + k % (sizeof(typed) - 1), 1)));
            h.Settle(buffer, MarkdownHighlighter::kFrameBudget);
            h.PrepareView(buffer, mid, mid + 59, MarkdownHighlighter::kViewBudget);
            keys.push_back(MsSince(t0));
            at++;
        }
        const size_t memoryTyped = h.MemoryBytes();
        for (int k = 0; k < kKeys / 8; k++) {
            t0 = Clock::now();
            h.OnEdit(buffer, Apply(buffer, at, 0, "\n"));
            h.Settle(buffer, MarkdownHighlighter::kFrameBudget);
            h.PrepareView(buffer, mid, mid + 59, MarkdownHighlighter::kViewBudget);
            enters.push_back(MsSince(t0));
            at++;
        }
        CHECK(!h.Pending());
        const size_t memoryAfter = h.MemoryBytes();

        // A fence typed at the top that nothing closes changes every line's state; removed, they
        // return: two scans of the whole document.
        h.OnEdit(buffer, Apply(buffer, 0, 0, "~~~~~\n"));
        const Settled typedFence = SettleFrames(h, buffer);
        h.OnEdit(buffer, Apply(buffer, 0, 6, ""));
        const Settled removedFence = SettleFrames(h, buffer);

        std::printf("  %-14s %9zu %8zu | %5d %8.2f %6.3f/%6.3f | %6.3f/%6.4f | %7.4f/%7.4f/%7.4f | %7.4f/%7.4f/%7.4f | "
                    "%4d %7.2f/%4d %7.2f | %9.1f\n",
                    doc.name, doc.text.size(), buffer.LineCount(), first.frames, first.ms, Percentile(first.frameMs, 0.5),
                    Percentile(first.frameMs, 1.0), cold, warm, Percentile(keys, 0.5), Percentile(keys, 0.99),
                    Percentile(keys, 1.0), Percentile(enters, 0.5), Percentile(enters, 0.99), Percentile(enters, 1.0),
                    typedFence.frames, typedFence.ms, removedFence.frames, removedFence.ms,
                    static_cast<double>(memoryAfter) / 1024.0);
        // Typing grows only the scratch that follows its line's length (amortized, at most
        // kMaxInlineBytes); Enter may grow the states' vector (amortized).
        if (memoryAfter != memoryBefore) {
            std::printf("  %-14s storage %zu -> %zu (typed) -> %zu bytes (line breaks)\n", doc.name, memoryBefore,
                        memoryTyped, memoryAfter);
        }
    }
}
