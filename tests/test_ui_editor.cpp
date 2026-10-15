// SPDX-License-Identifier: Apache-2.0
#include "runner.h"

#include "ui/textbuffer.h"
#include "ui/texteditor.h"
#include "ui/textwrap.h"
#include "text/fontchoice.h"
#include "text/fontindex.h"
#include "ui/theme.h"
#include "ui/tokens.h"
#include "ui/ui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using rgui::Key;
using rgui::ui::TextBuffer;
using rgui::ui::TextEditor;
using rgui::ui::WrapLayout;
using rgui::ui::WrapLine;

namespace {

// An in-memory clipboard behind a Context, answering read requests when told to.
struct FakeClipboard {
    std::string text;
    std::vector<std::function<void(std::string)>> pending;

    void Attach(rgui::ui::Context& ctx) {
        ctx.SetClipboard([this](std::string t) { text = std::move(t); },
            [this](std::function<void(std::string)> callback) { pending.push_back(std::move(callback)); });
    }
    void Answer() {
        auto callbacks = std::move(pending);
        pending.clear();
        for (auto& callback : callbacks) callback(text);
    }
};

void RunFrame(rgui::ui::Context& ctx, TextEditor& editor, double now) {
    ctx.BeginFrame(800, 600, 1.0, now);
    editor.Frame(ctx, {0, 0, 800, 600});
    ctx.EndFrame();
}

// Columns 10 px wide, rows 20 px high; `cells` columns per row, 10 rows visible.
void Grid(TextEditor& editor, int cells, int rows = 10) {
    editor.SetLayout(48.0f + 10.0f * static_cast<float>(cells), 16.0f + 20.0f * static_cast<float>(rows), 10, 20);
}

// The pane point in the middle of cell `col` of visual row `row` (no scroll).
float CellX(int col) { return 24.0f + 10.0f * static_cast<float>(col) + 2.0f; }
float RowY(int row) { return 16.0f + 20.0f * static_cast<float>(row) + 10.0f; }

void Type(TextEditor& editor, std::string_view text, double& now, double step = 50) {
    for (char c : text) {
        editor.SetNow(now);
        editor.HandleText(std::string(1, c));
        now += step;
    }
}

std::vector<size_t> NaiveLineStarts(const std::string& text) {
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') starts.push_back(i + 1);
    }
    return starts;
}

} // namespace

RGUI_TEST("ui.textbuffer_edits") {
    TextBuffer buffer;
    std::string model;
    std::mt19937 rng(7);
    const char alphabet[] = "ab\n\xC4\x85 ";   // includes a two-byte code point
    for (int i = 0; i < 3000; i++) {
        if (model.empty() || rng() % 3 != 0) {
            const size_t pos = rng() % (model.size() + 1);
            std::string bytes;
            const size_t n = rng() % 9;
            for (size_t k = 0; k < n; k++) bytes.push_back(alphabet[rng() % (sizeof(alphabet) - 1)]);
            buffer.Insert(pos, bytes);
            model.insert(pos, bytes);
        } else {
            const size_t pos = rng() % model.size();
            const size_t len = rng() % 12;
            buffer.Erase(pos, len);
            model.erase(pos, len);
        }
        if (i % 97 == 0) {
            CHECK(buffer.CopyText() == model);
            const auto starts = NaiveLineStarts(model);
            CHECK_EQ(buffer.LineCount(), starts.size());
            bool same = buffer.LineCount() == starts.size();
            for (size_t l = 0; same && l < starts.size(); l++) same = buffer.LineStart(l) == starts[l];
            CHECK(same);
        }
    }
    CHECK_EQ(buffer.Size(), model.size());
    CHECK(buffer.CopyText() == model);
    // Ranges on both sides of the gap and across it.
    buffer.Insert(model.size() / 2, "#");
    model.insert(model.size() / 2, "#");
    std::string scratch;
    for (size_t pos = 0; pos + 5 <= model.size(); pos += 7) {
        CHECK(buffer.Range(pos, 5, scratch) == std::string_view(model).substr(pos, 5));
    }
    CHECK(buffer.View() == model);
    for (size_t pos = 0; pos < model.size(); pos += 11) CHECK_EQ(buffer.LineOf(pos), static_cast<size_t>(
        std::count(model.begin(), model.begin() + static_cast<long>(pos), '\n')));
}

RGUI_TEST("ui.textbuffer_lines") {
    TextBuffer buffer;
    buffer.SetText("one\ntwo\n\nfour");
    CHECK_EQ(buffer.LineCount(), size_t(4));
    CHECK_EQ(buffer.LineStart(3), size_t(9));
    CHECK_EQ(buffer.LineEnd(0), size_t(3));
    CHECK_EQ(buffer.LineEnd(2), size_t(8));
    CHECK_EQ(buffer.LineEnd(3), size_t(13));
    CHECK_EQ(buffer.LineOf(3), size_t(0));   // the '\n' belongs to the line it ends
    CHECK_EQ(buffer.LineOf(4), size_t(1));
    buffer.Erase(2, 7);                      // "on" + "four": three line breaks gone
    CHECK(buffer.CopyText() == "onfour");
    CHECK_EQ(buffer.LineCount(), size_t(1));
    buffer.Insert(6, "\n");
    CHECK_EQ(buffer.LineCount(), size_t(2));
    CHECK_EQ(buffer.LineStart(1), size_t(7));
    CHECK_EQ(buffer.LineEnd(1), size_t(7));
}

RGUI_TEST("ui.textbuffer_utf8") {
    TextBuffer buffer;
    buffer.SetText("a\xC4\x85\xE4\xB8\xAD\xF0\x9F\x98\x80\xFFz");   // a ą 中 😀 <bad> z
    size_t len;
    CHECK_EQ(buffer.Decode(1, len), 0x105u);
    CHECK_EQ(len, size_t(2));
    CHECK_EQ(buffer.Decode(3, len), 0x4E2Du);
    CHECK_EQ(buffer.Decode(6, len), 0x1F600u);
    CHECK_EQ(len, size_t(4));
    CHECK_EQ(buffer.Decode(10, len), 0xFFFDu);
    CHECK_EQ(len, size_t(1));
    CHECK_EQ(buffer.Next(0), size_t(1));
    CHECK_EQ(buffer.Next(1), size_t(3));
    CHECK_EQ(buffer.Next(6), size_t(10));
    CHECK_EQ(buffer.Prev(10), size_t(6));
    CHECK_EQ(buffer.Prev(6), size_t(3));
    CHECK_EQ(buffer.Prev(3), size_t(1));
    CHECK_EQ(buffer.Prev(11), size_t(10));
    CHECK_EQ(buffer.Prev(12), size_t(11));
    CHECK_EQ(buffer.Prev(0), size_t(0));

    CHECK(rgui::ui::NormalizeEditorText("a\r\nb\rc\n") == "a\nb\nc\n");
    CHECK(rgui::ui::NormalizeEditorText("x\xC0\xAFy") == "x\xEF\xBF\xBD\xEF\xBF\xBDy");
    CHECK(rgui::ui::NormalizeEditorText("\xE4\xB8") == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

RGUI_TEST("ui.editor_wrap_line") {
    std::vector<uint32_t> breaks;
    CHECK_EQ(WrapLine("hello world foo", 8, breaks), 15u);
    CHECK(breaks == (std::vector<uint32_t>{6, 12}));          // after the spaces that fit
    WrapLine("abcdefghijkl", 5, breaks);
    CHECK(breaks == (std::vector<uint32_t>{5, 10}));          // a long word cut at the width
    WrapLine("ab cdefghijkl", 5, breaks);
    CHECK(breaks == (std::vector<uint32_t>{3, 8}));
    // 中 takes 2 cells: "ab" fills 2 of 3, 中 does not fit, then 中 + c, then d.
    CHECK_EQ(WrapLine("ab\xE4\xB8\xAD" "cd", 3, breaks), 6u);
    CHECK(breaks == (std::vector<uint32_t>{2, 6}));
    // A 2-cell code point on a 1-cell row still takes its own row.
    WrapLine("\xE4\xB8\xAD\xE4\xB8\xAD", 1, breaks);
    CHECK(breaks == (std::vector<uint32_t>{3}));
    // Tabs reach the next multiple of 4; spaces past the edge hang instead of starting a row.
    CHECK_EQ(WrapLine("a\tb", 80, breaks), 5u);
    CHECK(breaks.empty());
    WrapLine("abc     de", 4, breaks);
    CHECK(breaks == (std::vector<uint32_t>{8}));
    // A combining mark stays with its base.
    WrapLine("abce\xCC\x81" "f", 4, breaks);
    CHECK(breaks == (std::vector<uint32_t>{6}));
    CHECK_EQ(WrapLine("", 4, breaks), 0u);
    CHECK(breaks.empty());
}

RGUI_TEST("ui.editor_fenwick_random_edits") {
    TextEditor editor;
    Grid(editor, 12);
    std::mt19937 rng(11);
    const char* pieces[] = {"word ", "longerwordthanrow", "\n", "x", "\xE4\xB8\xAD", "\t", "  ", "a b c d e f g "};
    double now = 0;
    for (int i = 0; i < 1500; i++) {
        editor.SetNow(now += 2000);
        const size_t size = editor.Buffer().Size();
        const size_t a = size ? rng() % (size + 1) : 0;
        const size_t b = size ? rng() % (size + 1) : 0;
        editor.Select(a, b);
        if (rng() % 4 == 0 && size > 0) editor.HandleKey(Key::Backspace, 0, nullptr);
        else editor.HandleText(pieces[rng() % 8]);
        if (i % 50 == 0) Grid(editor, 6 + static_cast<int>(rng() % 20));
        if (i % 25 != 0) continue;

        const TextBuffer& buffer = editor.Buffer();
        const WrapLayout& wrap = editor.Wrap();
        CHECK_EQ(wrap.LineCount(), buffer.LineCount());
        size_t rows = 0;
        bool same = true;
        std::vector<uint32_t> breaks;
        std::string scratch;
        for (size_t line = 0; line < buffer.LineCount(); line++) {
            const size_t start = buffer.LineStart(line);
            WrapLine(buffer.Range(start, buffer.LineEnd(line) - start, scratch), wrap.WrapCells(), breaks);
            same = same && wrap.RowsOf(line) == breaks.size() + 1 && wrap.FirstRowOf(line) == rows;
            for (size_t r = 0; r <= breaks.size(); r++) {
                size_t gotLine, gotRow;
                wrap.LineOfRow(rows + r, gotLine, gotRow);
                same = same && gotLine == line && gotRow == r;
            }
            rows += breaks.size() + 1;
        }
        CHECK(same);
        CHECK_EQ(wrap.RowCount(), rows);
    }
}

RGUI_TEST("ui.editor_insert_erase") {
    TextEditor editor;
    Grid(editor, 40);
    int changes = 0;
    editor.SetOnChanged([&] { changes++; });
    double now = 0;
    Type(editor, "hello", now);
    CHECK(editor.CopyText() == "hello");
    CHECK_EQ(editor.Caret(), size_t(5));
    CHECK_EQ(changes, 5);
    editor.HandleKey(Key::Left, 0, nullptr);
    editor.HandleKey(Key::Left, 0, nullptr);
    editor.HandleText("\xC4\x85");
    CHECK(editor.CopyText() == "hel\xC4\x85lo");
    editor.HandleKey(Key::Backspace, 0, nullptr);   // a whole code point
    CHECK(editor.CopyText() == "hello");
    editor.HandleKey(Key::Delete, 0, nullptr);
    CHECK(editor.CopyText() == "helo");
    editor.HandleKey(Key::Enter, 0, nullptr);
    editor.HandleKey(Key::Tab, 0, nullptr);
    CHECK(editor.CopyText() == "hel\n\to");
    editor.Select(1, 4);
    editor.HandleText("EY");                          // replaces the selection
    CHECK(editor.CopyText() == "hEY\to");
    CHECK(!editor.HasSelection());
    editor.HandleText(std::string("\x01\x7F", 2));   // control characters are not text
    CHECK(editor.CopyText() == "hEY\to");
    CHECK(editor.Text() == "hEY\to");
    CHECK_EQ(editor.Revision(), uint64_t(changes));
}

RGUI_TEST("ui.editor_undo_grouping") {
    TextEditor editor;
    Grid(editor, 40);
    double now = 1000;
    // Typing within a second merges.
    Type(editor, "abc", now, 400);
    CHECK_EQ(editor.UndoGroups(), size_t(1));
    // A pause of a second starts a new group.
    now += 1000;
    Type(editor, "de", now, 100);
    CHECK_EQ(editor.UndoGroups(), size_t(2));
    // A caret jump ends the group even within the second.
    editor.HandleKey(Key::Left, 0, nullptr);
    Type(editor, "X", now, 10);
    CHECK_EQ(editor.UndoGroups(), size_t(3));
    CHECK(editor.CopyText() == "abcdXe");
    CHECK(editor.Undo());
    CHECK(editor.CopyText() == "abcde");
    CHECK_EQ(editor.Caret(), size_t(4));              // the caret before the group
    CHECK(editor.Undo());
    CHECK(editor.CopyText() == "abc");
    CHECK(editor.Redo());
    CHECK(editor.CopyText() == "abcde");
    CHECK_EQ(editor.Caret(), size_t(5));
    CHECK(editor.Redo());
    CHECK(editor.CopyText() == "abcdXe");
    CHECK(!editor.Redo());

    // Single deletions merge per direction; a backspace after typing is a new group.
    editor.SetText("0123456789");
    editor.Select(5, 5);
    editor.SetNow(now += 2000);
    for (int i = 0; i < 3; i++) {
        editor.SetNow(now += 100);
        editor.HandleKey(Key::Backspace, 0, nullptr);
    }
    CHECK(editor.CopyText() == "0156789");
    CHECK_EQ(editor.UndoGroups(), size_t(1));
    for (int i = 0; i < 2; i++) {
        editor.SetNow(now += 100);
        editor.HandleKey(Key::Delete, 0, nullptr);
    }
    CHECK(editor.CopyText() == "01789");
    CHECK_EQ(editor.UndoGroups(), size_t(2));
    editor.Undo();
    CHECK(editor.CopyText() == "0156789");
    CHECK_EQ(editor.Caret(), size_t(2));
    editor.Undo();
    CHECK(editor.CopyText() == "0123456789");
    CHECK_EQ(editor.Caret(), size_t(5));

    // A paste is its own group, between typing on both sides.
    editor.SetText("");
    editor.SetNow(now += 2000);
    Type(editor, "ab", now, 10);
    editor.SetNow(now += 10);
    editor.PasteText("PASTE");
    Type(editor, "cd", now, 10);
    CHECK(editor.CopyText() == "abPASTEcd");
    CHECK_EQ(editor.UndoGroups(), size_t(3));
    editor.Undo();
    CHECK(editor.CopyText() == "abPASTE");
    editor.Undo();
    CHECK(editor.CopyText() == "ab");
    // A new edit drops the redo history.
    Type(editor, "z", now, 10);
    CHECK(!editor.Redo());
    CHECK(editor.CopyText() == "abz");
}

RGUI_TEST("ui.editor_undo_limit") {
    TextEditor editor;
    Grid(editor, 40);
    std::string line(31, 'x');
    line += '\n';
    std::string block;
    block.reserve(24u << 20);
    while (block.size() < (24u << 20)) block += line;
    double now = 0;
    for (int i = 0; i < 3; i++) {
        editor.SetNow(now += 10);
        editor.PasteText(block);
    }
    // 72 MiB of insertions: the oldest group goes.
    CHECK_EQ(editor.UndoGroups(), size_t(2));
    CHECK(editor.UndoBytes() <= TextEditor::kUndoLimitBytes);
    CHECK(editor.Undo());
    CHECK(editor.Undo());
    CHECK(!editor.Undo());
    CHECK_EQ(editor.Buffer().Size(), block.size());
}

RGUI_TEST("ui.editor_word_motions") {
    TextEditor editor;
    Grid(editor, 80);
    // 0 foo_bar 7 , 8 ' ' 9 baz 12 ' ' 14 ąę 18 ' ' 19 qux 22
    editor.SetText("foo_bar, baz  \xC4\x85\xC4\x99 qux");
    CHECK_EQ(editor.WordRight(0), size_t(7));
    CHECK_EQ(editor.WordRight(7), size_t(12));
    CHECK_EQ(editor.WordRight(12), size_t(18));
    CHECK_EQ(editor.WordRight(18), size_t(22));
    CHECK_EQ(editor.WordRight(22), size_t(22));
    CHECK_EQ(editor.WordLeft(22), size_t(19));
    CHECK_EQ(editor.WordLeft(19), size_t(14));
    CHECK_EQ(editor.WordLeft(14), size_t(9));
    CHECK_EQ(editor.WordLeft(9), size_t(0));
    CHECK_EQ(editor.WordLeft(3), size_t(0));
    // Ideographic space (U+3000) separates words; CJK is word text.
    editor.SetText("\xE4\xB8\xAD\xE3\x80\x80\xE6\x96\x87");
    CHECK_EQ(editor.WordRight(0), size_t(3));
    CHECK_EQ(editor.WordRight(3), size_t(9));

    editor.SetText("alpha beta, gamma");
    editor.Select(17, 17);
    editor.HandleKey(Key::Left, rgui::kModCtrl, nullptr);
    CHECK_EQ(editor.Caret(), size_t(12));
    editor.HandleKey(Key::Left, rgui::kModCtrl | rgui::kModShift, nullptr);
    CHECK_EQ(editor.Caret(), size_t(6));
    CHECK_EQ(editor.Anchor(), size_t(12));
    editor.HandleKey(Key::End, 0, nullptr);
    editor.HandleKey(Key::Backspace, rgui::kModCtrl, nullptr);
    CHECK(editor.CopyText() == "alpha beta, ");
    editor.HandleKey(Key::Home, 0, nullptr);
    editor.HandleKey(Key::Delete, rgui::kModCtrl, nullptr);
    CHECK(editor.CopyText() == " beta, ");
    // Ctrl+Alt (AltGr) is never a shortcut.
    CHECK(!editor.HandleKey(Key::A, rgui::kModCtrl | rgui::kModAlt, nullptr));
    CHECK(!editor.HandleKey(Key::Left, rgui::kModCtrl | rgui::kModAlt, nullptr));
    CHECK(!editor.HandleKey(Key::S, rgui::kModCtrl, nullptr));   // the app's shortcut
}

RGUI_TEST("ui.editor_click_selection") {
    TextEditor editor;
    Grid(editor, 40);
    editor.SetText("hello, world!!  next\nsecond line");
    editor.PointerDown(CellX(2), RowY(0), 1, false);
    editor.PointerUp();
    CHECK_EQ(editor.Caret(), size_t(2));
    CHECK(!editor.HasSelection());
    editor.PointerDown(CellX(8), RowY(0), 2, false);   // in "world"
    editor.PointerUp();
    CHECK(editor.SelectedText() == "world");
    editor.PointerDown(CellX(13), RowY(0), 2, false);  // the "!!" run
    editor.PointerUp();
    CHECK(editor.SelectedText() == "!!");
    editor.PointerDown(CellX(14), RowY(0), 2, false);  // the run of two spaces
    editor.PointerUp();
    CHECK(editor.SelectedText() == "  ");
    editor.PointerDown(CellX(30), RowY(0), 2, false);  // past the line's end: its last word
    editor.PointerUp();
    CHECK(editor.SelectedText() == "next");
    editor.PointerDown(CellX(3), RowY(0), 3, false);
    editor.PointerUp();
    CHECK(editor.SelectedText() == "hello, world!!  next\n");
    editor.PointerDown(CellX(3), RowY(1), 3, false);
    editor.PointerUp();
    CHECK(editor.SelectedText() == "second line");
    // Shift+click extends from the anchor.
    editor.PointerDown(CellX(0), RowY(0), 1, false);
    editor.PointerUp();
    editor.PointerDown(CellX(5), RowY(1), 1, true);
    editor.PointerUp();
    CHECK(editor.SelectedText() == "hello, world!!  next\nsecon");
    // Dragging after a double click extends by words.
    editor.PointerDown(CellX(1), RowY(0), 2, false);
    editor.PointerDrag(CellX(9), RowY(0));
    editor.PointerUp();
    CHECK(editor.SelectedText() == "hello, world");
    // A plain drag selects characters.
    editor.PointerDown(CellX(7), RowY(0), 1, false);
    editor.PointerDrag(CellX(2), RowY(1));
    editor.PointerUp();
    CHECK(editor.SelectedText() == "world!!  next\nse");
    CHECK_EQ(editor.Caret(), size_t(23));
}

RGUI_TEST("ui.editor_rows_and_columns") {
    TextEditor editor;
    Grid(editor, 10, 4);
    // Row 0 "aaaa bbbb ", row 1 "cccc", row 2 "\xE4\xB8\xADxyz" (中 is 2 cells), row 3 "end".
    editor.SetText("aaaa bbbb cccc\n\xE4\xB8\xAD" "xyz\nend");
    CHECK_EQ(editor.WrapCells(), 10);
    CHECK_EQ(editor.Wrap().RowCount(), size_t(4));
    editor.Select(2, 2);
    editor.HandleKey(Key::End, 0, nullptr);            // the end of the visual row
    CHECK_EQ(editor.Caret(), size_t(10));
    CHECK_EQ(editor.CaretRow(), size_t(0));
    CHECK_EQ(editor.CaretColumn(), 10);
    editor.HandleKey(Key::Home, 0, nullptr);
    CHECK_EQ(editor.Caret(), size_t(0));
    editor.Select(12, 12);                             // row 1, column 2
    CHECK_EQ(editor.CaretRow(), size_t(1));
    editor.HandleKey(Key::Home, 0, nullptr);
    CHECK_EQ(editor.Caret(), size_t(10));
    editor.HandleKey(Key::Right, 0, nullptr);
    editor.HandleKey(Key::Right, 0, nullptr);
    editor.HandleKey(Key::Right, 0, nullptr);          // column 3
    editor.HandleKey(Key::Down, 0, nullptr);           // 中 covers columns 0-1: "x" at 2, "y" at 3
    CHECK_EQ(editor.Caret(), size_t(19));
    CHECK_EQ(editor.CaretColumn(), 3);
    editor.HandleKey(Key::Down, rgui::kModShift, nullptr);   // "end" is shorter: its end
    CHECK_EQ(editor.Caret(), size_t(25));
    CHECK_EQ(editor.Anchor(), size_t(19));
    editor.HandleKey(Key::Up, 0, nullptr);             // the desired column 3 is kept
    CHECK_EQ(editor.Caret(), size_t(19));
    editor.HandleKey(Key::Up, 0, nullptr);
    CHECK_EQ(editor.Caret(), size_t(13));
    editor.HandleKey(Key::Up, 0, nullptr);
    CHECK_EQ(editor.Caret(), size_t(3));
    editor.HandleKey(Key::Up, 0, nullptr);             // above the first row: the start
    CHECK_EQ(editor.Caret(), size_t(0));
    editor.HandleKey(Key::End, rgui::kModCtrl, nullptr);
    CHECK_EQ(editor.Caret(), editor.Buffer().Size());
    editor.HandleKey(Key::A, rgui::kModCtrl, nullptr);
    CHECK_EQ(editor.SelectionStart(), size_t(0));
    CHECK_EQ(editor.SelectionEnd(), editor.Buffer().Size());
    editor.HandleKey(Key::Left, 0, nullptr);           // collapses to the selection's start
    CHECK_EQ(editor.Caret(), size_t(0));
    CHECK(!editor.HasSelection());
    // A click past the end of a wrapped row puts the caret at its end, on that row.
    editor.PointerDown(CellX(15), RowY(0), 1, false);
    editor.PointerUp();
    CHECK_EQ(editor.Caret(), size_t(10));
    CHECK_EQ(editor.CaretRow(), size_t(0));
}

RGUI_TEST("ui.editor_caret_visibility") {
    TextEditor editor;
    Grid(editor, 20, 10);   // 10 rows of 20 px below the 16 px top padding
    std::string text;
    for (int i = 0; i < 100; i++) text += "line\n";
    editor.SetText(text);   // 101 rows
    CHECK_EQ(editor.ScrollY(), 0.0f);
    editor.HandleKey(Key::End, rgui::kModCtrl, nullptr);
    // The caret row 100 shows with one row of margin below: rows 92-101 visible.
    CHECK_NEAR(editor.ScrollY(), 92 * 20, 0.01);
    CHECK_NEAR(editor.ScrollY(), editor.MaxScrollY(), 0.01);
    editor.HandleKey(Key::PageUp, 0, nullptr);         // 9 rows up, the view with it
    CHECK_EQ(editor.CaretRow(), size_t(91));
    CHECK_NEAR(editor.ScrollY(), 83 * 20, 0.01);
    editor.Select(50 * 5, 50 * 5);                     // row 50: one row of margin above
    CHECK_NEAR(editor.ScrollY(), 49 * 20, 0.01);
    editor.HandleKey(Key::Down, 0, nullptr);           // still inside: no scroll
    CHECK_NEAR(editor.ScrollY(), 49 * 20, 0.01);
    for (int i = 0; i < 8; i++) editor.HandleKey(Key::Down, 0, nullptr);   // row 59 needs row 60 visible
    CHECK_NEAR(editor.ScrollY(), 51 * 20, 0.01);
    {
        const rgui::gfx::RectF caret = editor.CaretRect();
        CHECK_NEAR(caret.y, 16 + 59 * 20 - 51 * 20, 0.01);
        CHECK_NEAR(caret.w, 1, 0.0);
    }
    // Dragging below the pane scrolls toward the pointer and extends the selection.
    editor.PointerDown(CellX(1), RowY(2), 1, false);
    const float before = editor.ScrollY();
    editor.PointerDrag(CellX(1), 16 + 20 * 10 + 30);
    CHECK(editor.AutoScroll());
    CHECK(editor.ScrollY() > before);
    CHECK(editor.Caret() > editor.Anchor());
    editor.PointerUp();
    CHECK(!editor.AutoScroll());
}

RGUI_TEST("ui.editor_clipboard_and_crlf") {
    rgui::ui::Context ctx;
    FakeClipboard clipboard;
    clipboard.Attach(ctx);
    TextEditor editor;
    RunFrame(ctx, editor, 0);
    editor.SetText("one\r\ntwo\rthree");
    CHECK(editor.CopyText() == "one\ntwo\nthree");
    editor.Select(0, 3);
    CHECK(editor.HandleKey(Key::C, rgui::kModCtrl, &ctx));
    CHECK(clipboard.text == "one");
    CHECK(editor.HandleKey(Key::X, rgui::kModCtrl, &ctx));
    CHECK(editor.CopyText() == "\ntwo\nthree");
    clipboard.text = "a\r\nb";
    editor.HandleKey(Key::V, rgui::kModCtrl, &ctx);
    RunFrame(ctx, editor, 10);
    CHECK(editor.CopyText() == "\ntwo\nthree");       // the clipboard answers later
    clipboard.Answer();
    RunFrame(ctx, editor, 20);
    CHECK(editor.CopyText() == "a\nb\ntwo\nthree");
    editor.HandleKey(Key::Insert, rgui::kModShift, &ctx);
    clipboard.Answer();
    RunFrame(ctx, editor, 30);
    CHECK(editor.CopyText() == "a\nba\nb\ntwo\nthree");
    editor.HandleText("\r\n");
    CHECK(editor.CopyText().find('\r') == std::string::npos);
    CHECK(editor.HandleKey(Key::Z, rgui::kModCtrl, &ctx));
    CHECK(editor.HandleKey(Key::Z, rgui::kModCtrl | rgui::kModShift, &ctx));
    CHECK(editor.CopyText() == "a\nba\nb\n\ntwo\nthree");
    CHECK(editor.HandleKey(Key::Z, rgui::kModCtrl, &ctx));
    CHECK(editor.HandleKey(Key::Y, rgui::kModCtrl, &ctx));
    CHECK(editor.CopyText() == "a\nba\nb\n\ntwo\nthree");
}

namespace {

struct EditorFonts {
    rgui::text::FontIndex index;
    rgui::text::FontSet fonts;
    bool ok = false;

    EditorFonts() {
        index.SetDirectories({"/usr/share/fonts"});
        ok = index.Find("DejaVu Sans Mono", 400, false) != nullptr &&
            fonts.Load(index, {"DejaVu Sans Mono"}, {"DejaVu Sans"});
    }
};

rgui::InputEvent KeyEvent(Key key, uint8_t mods = 0) {
    rgui::InputEvent e;
    e.type = rgui::InputEvent::Type::KeyDown;
    e.key = key;
    e.mods = mods;
    return e;
}

rgui::InputEvent TextEvent(std::string text, rgui::InputEvent::Type type = rgui::InputEvent::Type::Text) {
    rgui::InputEvent e;
    e.type = type;
    e.text = std::move(text);
    return e;
}

rgui::InputEvent ButtonEvent(float x, float y, bool pressed) {
    rgui::InputEvent e;
    e.type = rgui::InputEvent::Type::Button;
    e.x = x;
    e.y = y;
    e.pressed = pressed;
    return e;
}

rgui::InputEvent WheelEvent(float x, float y, float notches, uint8_t mods = 0) {
    rgui::InputEvent e;
    e.type = rgui::InputEvent::Type::Wheel;
    e.x = x;
    e.y = y;
    e.notchesY = notches;
    e.mods = mods;
    return e;
}

// The glyph commands of a display list in `color`, and their glyph count.
size_t CountGlyphs(const rgui::ui::DisplayList& list, uint32_t color) {
    size_t n = 0;
    for (const auto& c : list.Commands()) {
        if (c.type == rgui::ui::CommandType::Glyphs && c.color == color) n += c.glyphCount;
    }
    return n;
}

} // namespace

RGUI_TEST("ui.editor_widget_frame") {
    EditorFonts f;
    if (!f.ok) {
        std::printf("skip ui.editor_widget_frame: DejaVu Sans Mono not installed\n");
        return;
    }
    using rgui::ui::Context;
    const rgui::ui::Tokens tokens = rgui::ui::BuildTokens(
        rgui::ui::BundledPalette(*rgui::ui::FindBundledTheme("tokyo-night")), rgui::ui::TokenSources{});
    Context ctx;
    ctx.SetFonts(&f.fonts);
    ctx.SetTokens(&tokens);
    TextEditor editor;
    int changes = 0;
    editor.SetOnChanged([&] { changes++; });
    const rgui::gfx::RectF pane{0, 0, 800, 600};
    double now = 1000;
    auto frame = [&](double at) -> const rgui::ui::FrameOutput& {
        ctx.BeginFrame(800, 600, 1.0, at);
        editor.Frame(ctx, pane);
        return ctx.EndFrame();
    };

    // Unfocused and empty: the placeholder in the muted colour, no text input.
    const rgui::ui::FrameOutput& first = frame(now);
    CHECK(!first.textInputEnabled);
    CHECK_EQ(CountGlyphs(*first.displayList, tokens.colors.mutedText.Premultiplied()), size_t(15));
    // The grid: DejaVu Sans Mono's 'M' at 14 px, 140 % lines.
    CHECK_NEAR(editor.ColumnWidth(), 1233.0 / 2048.0 * 14.0, 0.01);
    CHECK_EQ(editor.LineHeight(), 20.0f);
    CHECK_EQ(editor.WrapCells(), static_cast<int>((800 - 48) / editor.ColumnWidth()));

    // A click focuses; keys and text apply in arrival order; Tab stays in the editor; the app's
    // shortcuts stay unconsumed.
    ctx.AddEvent(ButtonEvent(100, 100, true), now += 10);
    ctx.AddEvent(ButtonEvent(100, 100, false), now);
    frame(now);
    ctx.AddEvent(TextEvent("ab"), now += 10);
    ctx.AddEvent(KeyEvent(Key::Backspace), now);
    ctx.AddEvent(KeyEvent(Key::Tab), now);
    ctx.AddEvent(KeyEvent(Key::S, rgui::kModCtrl), now);
    ctx.AddEvent(TextEvent("c\xE4\xB8\xAD"), now);
    ctx.BeginFrame(800, 600, 1.0, now);
    editor.Frame(ctx, pane);
    CHECK(ctx.IsFocused(editor.WidgetId()));
    CHECK(ctx.IsConsumed(2));    // Tab
    CHECK(!ctx.IsConsumed(3));   // Ctrl+S
    const rgui::ui::FrameOutput& typed = ctx.EndFrame();
    CHECK(editor.CopyText() == "a\tc\xE4\xB8\xAD");
    CHECK_EQ(changes, 4);
    CHECK(typed.textInputEnabled);
    // The caret after a(1) tab(3) c(1) 中(2): column 7.
    CHECK_EQ(typed.textInputRect.x0, static_cast<int>(24 + 7 * editor.ColumnWidth()));
    CHECK_EQ(CountGlyphs(*typed.displayList, tokens.colors.text.Premultiplied()), size_t(3));
    CHECK_EQ(CountGlyphs(*typed.displayList, tokens.colors.mutedText.Premultiplied()), size_t(0));

    // A preedit draws at the caret, underlined, outside the buffer.
    ctx.AddEvent(TextEvent("xy", rgui::InputEvent::Type::Preedit), now += 10);
    const rgui::ui::FrameOutput& pre = frame(now);
    CHECK_EQ(CountGlyphs(*pre.displayList, tokens.colors.text.Premultiplied()), size_t(5));
    CHECK(editor.CopyText() == "a\tc\xE4\xB8\xAD");
    ctx.AddEvent(TextEvent("xy"), now += 10);
    frame(now);
    CHECK(editor.CopyText() == "a\tc\xE4\xB8\xADxy");
    CHECK(editor.Preedit().empty());

    // Selected text draws in the selection's foreground.
    ctx.AddEvent(KeyEvent(Key::A, rgui::kModCtrl), now += 10);
    const rgui::ui::FrameOutput& selected = frame(now);
    CHECK_EQ(CountGlyphs(*selected.displayList, tokens.colors.selectedText.Premultiplied()), size_t(5));

    // The wheel eases 120 px per notch over 200 ms; Ctrl+wheel steps the text size.
    std::string many;
    for (int i = 0; i < 400; i++) many += "line of text\n";
    editor.SetText(many);
    frame(now += 10);
    ctx.AddEvent(WheelEvent(300, 300, 1), now += 10);
    const rgui::ui::FrameOutput& wheel = frame(now);
    CHECK(wheel.wantsFrame);
    frame(now += 100);
    CHECK(editor.ScrollY() > 0 && editor.ScrollY() < 120);
    frame(now += 150);
    CHECK_EQ(editor.ScrollY(), 120.0f);
    ctx.AddEvent(WheelEvent(300, 300, -1, rgui::kModCtrl), now += 10);
    frame(now);
    CHECK_EQ(editor.ZoomIndex(), rgui::ui::kEditorZoomDefault + 1);
    CHECK_EQ(editor.LineHeight(), 22.0f);   // round(1.4 x 15.4)
}
