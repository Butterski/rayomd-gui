// SPDX-License-Identifier: Apache-2.0
// The source map between the editor's lines and the preview's pages, and the source sync of the
// main window: an editor press shows its line in the preview, a double click on the preview puts
// the caret on its line, and the panes scroll together.
#include "app_harness.h"
#include "headless_platform.h"
#include "runner.h"

#include "app/mainwindow.h"
#include "app/sourcemap.h"
#include "pdf/reader.h"
#include "rayomd/tiny_pdf.h"
#include "ui/textbuffer.h"
#include "ui/texteditor.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using rgui::InputEvent;
using rgui::Key;
using rgui::SourceMap;
using rgui::test::HaveFonts;
using rgui::test::TesterPath;
using Harness = rgui::test::AppHarness;

double Distance(double a, double b) { return a > b ? a - b : b - a; }

InputEvent Wheel(float x, float y, float notches) {
    InputEvent e;
    e.type = InputEvent::Type::Wheel;
    e.x = x;
    e.y = y;
    e.notchesY = notches;
    return e;
}

} // namespace

RGUI_TEST("app.sourcemap") {
    // Ten lines of three bytes; a block of line 0 and one of lines 2-5 on a page of 842 points.
    std::string text;
    for (int i = 0; i < 10; i++) text += "l" + std::to_string(i) + "\n";
    const std::vector<TinyPdf::SourceBlock> blocks = {{0, 1, 0, 0, 54, 80}, {2, 6, 0, 0, 80, 200}};
    const SourceMap map = SourceMap::FromEngine(blocks, text, {842.0});
    rgui::ui::TextBuffer buffer;
    buffer.SetText(text);
    CHECK_EQ(map.Spans().size(), size_t(2));
    CHECK_NEAR(map.PageOfLine(buffer, 0), 54.0 / 842, 1e-12);
    // Inside a block, linear in its lines; between blocks (the blank line 1), where they meet.
    CHECK_NEAR(map.PageOfLine(buffer, 3), (80 + 0.25 * 120) / 842, 1e-12);
    CHECK_NEAR(map.PageOfLine(buffer, 1.5), 80.0 / 842, 1e-12);
    for (const double line : {0.5, 2.0, 3.0, 5.5}) {
        CHECK_NEAR(map.LineOfPage(buffer, map.PageOfLine(buffer, line)), line, 1e-9);
    }
    // A map older than the text (three lines now) still answers within the text and the pages.
    rgui::ui::TextBuffer shorter;
    shorter.SetText("a\nb\nc");
    for (double x = 0; x <= 3; x += 0.25) {
        const double page = map.PageOfLine(shorter, x);
        CHECK(page >= 0 && page <= 1);
        const double line = map.LineOfPage(shorter, x / 3);
        CHECK(line >= 0 && line <= 3);
    }
    // The blocks of one HTML block share its lines: one span, down to the last one's bottom.
    const SourceMap merged = SourceMap::FromEngine({{2, 6, 0, 0, 80, 120}, {2, 6, 0, 0, 120, 200}}, text, {842.0});
    CHECK_EQ(merged.Spans().size(), size_t(1));
    if (merged.Spans().size() == 1) CHECK_NEAR(merged.Spans()[0].stop, 200.0 / 842, 1e-12);
    CHECK_EQ(SourceMap().PageOfLine(buffer, 1), -1.0);
    CHECK_EQ(SourceMap().LineOfPage(buffer, 0.5), -1.0);

    // The engine's map of a document: a block for each top-level block, the footnote's none.
    const std::string document =
        "# Title\n\n"
        "First paragraph\ncontinues here.\n\n"
        "- one\n- two\n\n"
        "```text\ncode\n```\n\n"
        "| a | b |\n|---|---|\n| 1 | 2 |\n\n"
        "Note[^n].\n\n"
        "[^n]: The note.\n";
    std::vector<TinyPdf::SourceBlock> laidOut;
    TinyPdf::PdfOptions options;
    options.sourceMap = &laidOut;
    std::string bytes;
    CHECK(TinyPdf::BuildPdf(document, options, bytes).Ok());
    const std::shared_ptr<const rgui::pdf::PdfFile> pdf = rgui::pdf::PdfFile::Parse(bytes, nullptr);
    CHECK(pdf != nullptr);
    if (!pdf) return;
    std::vector<double> heights;
    for (int i = 0; i < pdf->PageCount(); i++) heights.push_back(pdf->PageMediaBox(i).y1 - pdf->PageMediaBox(i).y0);
    const SourceMap built = SourceMap::FromEngine(laidOut, document, heights);
    const std::vector<rgui::SourceSpan>& spans = built.Spans();
    CHECK_EQ(spans.size(), size_t(7));
    if (spans.size() != 7) return;
    CHECK_EQ(spans[1].begin, document.find("First paragraph"));
    CHECK_EQ(spans[1].end, document.find("continues here.\n") + 16);   // line 4
    for (size_t k = 0; k < spans.size(); k++) {
        CHECK(spans[k].start <= spans[k].stop);
        if (k > 0) CHECK(spans[k - 1].stop <= spans[k].start);
    }
}

RGUI_TEST("app.source_sync") {
    if (!HaveFonts()) {
        std::printf("skip app.source_sync: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("source_sync");
    h.Start("ui.theme=tokyo-night\n", TesterPath());
    CHECK(h.Settle());
    rgui::MainWindow& w = h.Window();
    CHECK(w.SyncScroll());
    CHECK(!w.Sources().Empty());
    if (w.Sources().Empty()) return;
    rgui::ui::TextEditor& editor = w.Editor();
    const rgui::ui::TextBuffer& buffer = editor.Buffer();
    constexpr float kView = Harness::kHeight - rgui::MainWindow::kFooterHeight;   // the preview pane
    constexpr uint32_t kAccent = 0xFF7AA2F7u;   // tokyo-night's
    // Where line 0 shows with the preview at its top.
    const float reference = w.PreviewYOfPage(w.Sources().PageOfLine(buffer, 0));
    // The line at the top of the editor, and the one at the same place of the preview.
    const auto editorLine = [&] { return editor.LineAtScrollY(editor.ScrollY()); };
    const auto previewLine = [&] {
        return w.Sources().LineOfPage(buffer, w.PageOfPreviewY(w.PreviewScroll() + reference));
    };

    // A press in the editor shows its line in the preview, with a marker beside it for a moment.
    const size_t heading = editor.CopyText().rfind("\n## ");
    CHECK(heading != std::string::npos);
    const size_t line = buffer.LineOf(heading + 1);
    editor.ScrollTo(editor.ScrollYOfLine(static_cast<double>(line)) - 100, false, h.platform.NowMs());
    h.Frame();
    h.Click(200, rgui::ui::kEditorPadTop + editor.ScrollYOfLine(static_cast<double>(line)) - editor.ScrollY() + 5);
    h.platform.Advance(250);
    h.Frame();
    h.Frame();
    CHECK_EQ(buffer.LineOf(editor.Caret()), line);
    const float y = w.PreviewYOfPage(w.Sources().PageOfLine(buffer, static_cast<double>(line)));
    CHECK(y >= w.PreviewScroll() && y <= w.PreviewScroll() + kView);
    const int markerX = static_cast<int>(w.PageX() + 7);
    const int markerY = static_cast<int>(y - w.PreviewScroll() + 2);
    CHECK_EQ(h.Pixel(markerX, markerY), kAccent);
    h.platform.Advance(1300);
    h.Frame();
    CHECK(h.Pixel(markerX, markerY) != kAccent);

    // A double click on a page puts the caret on the line shown there, in view, and gives the
    // editor the keys. A press in the 16 px between two pages does nothing: aim inside one (a
    // position in it has a fraction; the gap's is the next page's top).
    float py = 400;
    double page = w.PageOfPreviewY(w.PreviewScroll() + py);
    if (page == static_cast<double>(static_cast<int>(page))) {
        py += 24;
        page = w.PageOfPreviewY(w.PreviewScroll() + py);
    }
    const size_t expected = static_cast<size_t>(w.Sources().LineOfPage(buffer, page));
    h.Click(w.PageX() + 300, py);
    h.Click(w.PageX() + 300, py);
    h.Frame();
    h.platform.Advance(250);
    h.Frame();
    const size_t at = buffer.LineStart(expected);
    CHECK_EQ(editor.Caret(), at);
    const float rowTop = editor.ScrollYOfLine(static_cast<double>(expected));
    CHECK(rowTop >= editor.ScrollY() && rowTop + editor.LineHeight() <= editor.ScrollY() + editor.ViewHeight());
    h.Type("Z");
    CHECK_EQ(editor.CopyText().substr(at, 1), std::string("Z"));
    h.KeyDown(Key::Z, rgui::kModCtrl);
    CHECK(h.Settle());

    // The editor's wheel scrolls the preview to the same place (away from the editor's last
    // screenful, which brings the preview to its end).
    editor.ScrollTo(editor.ScrollYOfLine(20), false, h.platform.NowMs());
    h.Frame();
    h.app->OnInput(Wheel(300, 400, 3));
    h.Frame();
    h.platform.Advance(250);
    h.Frame();
    h.Frame();
    CHECK(editor.ScrollY() < editor.MaxScrollY() - editor.ViewHeight());
    CHECK(Distance(previewLine(), editorLine()) < 1.0);

    // The preview's wheel scrolls the editor (a frame later).
    h.app->OnInput(Wheel(1000, 400, 2));
    h.Frame();
    h.platform.Advance(250);
    h.Frame();
    h.Frame();
    CHECK(Distance(previewLine(), editorLine()) < 1.0);

    // The editor's end brings the preview to its end.
    h.app->OnInput(Wheel(300, 400, 1000));
    h.Frame();
    h.platform.Advance(250);
    h.Frame();
    h.Frame();
    CHECK_EQ(editor.ScrollY(), editor.MaxScrollY());
    CHECK(w.PreviewYOfPage(static_cast<double>(w.Preview().PageCount())) <= w.PreviewScroll() + kView + 16);

    // Off: the editor scrolls alone.
    w.SetSyncScroll(false);
    h.Frame();
    const float before = w.PreviewScroll();
    h.app->OnInput(Wheel(300, 400, -5));
    h.Frame();
    h.platform.Advance(250);
    h.Frame();
    h.Frame();
    CHECK(editor.ScrollY() < editor.MaxScrollY());
    CHECK_EQ(w.PreviewScroll(), before);
}
