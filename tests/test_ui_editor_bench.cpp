// SPDX-License-Identifier: Apache-2.0
// bench.typing: the cost of a keystroke in a large document, UI and raster.
#include "runner.h"

#include "common/file_io.h"
#include "gfx/surface.h"
#include "text/fontchoice.h"
#include "text/fontindex.h"
#include "text/glyphcache.h"
#include "ui/compositor.h"
#include "ui/icons.h"
#include "ui/texteditor.h"
#include "ui/theme.h"
#include "ui/tokens.h"
#include "ui/ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// Under AddressSanitizer the frames are ~30x slower and their times mean nothing: the bench then
// types 100 characters, to keep the large-document path covered within the unit run's time.
#if defined(__SANITIZE_ADDRESS__)
constexpr int kKeystrokes = 100;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int kKeystrokes = 100;
#else
constexpr int kKeystrokes = 1000;
#endif
#else
constexpr int kKeystrokes = 1000;
#endif

// kKeystrokes characters typed one at a time into the middle of a 1 MiB document (tester.md
// repeated), each a full frame at 1920 x 1080: the editor's frame (input, layout, display list)
// and the compositor's raster of the tiles that changed. Prints p50 and p99, and the display
// list's commands and the tiles redrawn per keystroke. RGUI_SYNTAX_COLORS=0 turns the editor's
// syntax colours off (the A/B of their cost).
RGUI_TEST("bench.typing") {
    rgui::text::FontIndex index;
    index.SetDirectories({"/usr/share/fonts"});
    rgui::text::FontSet fonts;
    if (index.Find("DejaVu Sans Mono", 400, false) == nullptr ||
        !fonts.Load(index, {"DejaVu Sans Mono"}, {"DejaVu Sans"})) {
        std::printf("skip bench.typing: DejaVu Sans Mono not installed\n");
        return;
    }
    std::string tester;
    CHECK(RayoMd::File::ReadAll(RayoMd::File::PathFromUtf8(std::string(rgui::test::SourceDir()) + "/external/rayomd/tester.md"), 8u << 20, tester));
    if (tester.empty()) return;
    std::string document;
    document.reserve((1u << 20) + tester.size());
    while (document.size() < (1u << 20)) document += tester;

    const rgui::ui::Tokens tokens = rgui::ui::BuildTokens(
        rgui::ui::BundledPalette(*rgui::ui::FindBundledTheme("tokyo-night")), rgui::ui::TokenSources{});
    rgui::ui::Context ctx;
    ctx.SetFonts(&fonts);
    ctx.SetTokens(&tokens);
    rgui::ui::TextEditor editor;
    const char* syntaxEnv = std::getenv("RGUI_SYNTAX_COLORS");
    const bool syntax = !(syntaxEnv && std::string_view(syntaxEnv) == "0");
    editor.SetSyntaxColors(syntax);
    editor.SetText(document);
    rgui::ui::Compositor compositor;
    rgui::text::GlyphCache glyphs;
    rgui::ui::IconCache icons;
    constexpr int kWidth = 1920, kHeight = 1080;
    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight);
    const rgui::gfx::Surface surface{pixels.data(), kWidth, kHeight, kWidth};
    const uint32_t background = tokens.colors.windowBackground.Premultiplied();
    const rgui::gfx::RectF pane{0, 0, kWidth, kHeight};

    double now = 1000;
    size_t damaged = 0, commands = 0;
    auto frame = [&] {
        ctx.BeginFrame(kWidth, kHeight, 1.0, now);
        editor.Frame(ctx, pane);
        const rgui::ui::FrameOutput& out = ctx.EndFrame();
        commands = out.displayList->Commands().size();
        glyphs.BeginFrame();
        damaged = compositor.Render(*out.displayList, background, surface, glyphs, icons).size();
    };
    // Focus, and the caret at the start of a line in the middle of the document.
    rgui::InputEvent press;
    press.type = rgui::InputEvent::Type::Button;
    press.x = 200;
    press.y = 200;
    press.pressed = true;
    ctx.AddEvent(press, now);
    press.pressed = false;
    ctx.AddEvent(press, now);
    frame();
    const size_t middle = editor.Buffer().LineStart(editor.Buffer().LineCount() / 2);
    editor.Select(middle, middle);
    now += 16;
    frame();
    CHECK(ctx.IsFocused(editor.WidgetId()));
    // The first scan of the document's syntax, a frame budget at a time, before the keystrokes.
    int settleFrames = 0;
    while (editor.Highlighter().Pending() && settleFrames < 100000) {
        now += 16;
        frame();
        settleFrames++;
    }

    const char typed[] = "The quick brown fox jumps over the lazy dog. ";
    std::vector<double> ms, commandCounts, tileCounts;
    ms.reserve(kKeystrokes);
    for (int i = 0; i < kKeystrokes; i++) {
        rgui::InputEvent text;
        text.type = rgui::InputEvent::Type::Text;
        text.text.assign(1, typed[i % (sizeof(typed) - 1)]);
        now += 50;
        ctx.AddEvent(text, now);
        const auto t0 = std::chrono::steady_clock::now();
        frame();
        const auto t1 = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        commandCounts.push_back(static_cast<double>(commands));
        tileCounts.push_back(static_cast<double>(damaged));
    }
    CHECK_EQ(editor.Buffer().Size(), document.size() + kKeystrokes);
    CHECK(damaged > 0);
    std::sort(ms.begin(), ms.end());
    std::sort(commandCounts.begin(), commandCounts.end());
    std::sort(tileCounts.begin(), tileCounts.end());
    std::printf("bench.typing: %d keystrokes, %zu-byte document, %dx%d, %d tile threads, syntax colours %s "
                "(first scan %d frames): p50 %.3f ms, p99 %.3f ms, max %.3f ms; commands p50 %.0f, tiles p50 %.0f "
                "max %.0f\n",
        kKeystrokes, document.size(), kWidth, kHeight, compositor.Threads(), syntax ? "on" : "off", settleFrames,
        ms[ms.size() / 2], ms[ms.size() * 99 / 100], ms.back(), commandCounts[commandCounts.size() / 2],
        tileCounts[tileCounts.size() / 2], tileCounts.back());

    // Line breaks (Enter, a list continuing) at the same place, each a frame.
    std::vector<double> enters;
    for (int i = 0; i < kKeystrokes / 4; i++) {
        now += 50;
        const auto t0 = std::chrono::steady_clock::now();
        editor.HandleKey(rgui::Key::Enter, 0, &ctx);
        frame();
        enters.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(enters.begin(), enters.end());
    // Frames with nothing to do once everything settled (the preview's repaints).
    std::vector<double> idle;
    for (int i = 0; i < 200; i++) {
        now += 16;
        const auto t0 = std::chrono::steady_clock::now();
        frame();
        idle.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(idle.begin(), idle.end());
    std::printf("bench.typing: %d line breaks: p50 %.3f ms, p99 %.3f ms, max %.3f ms; idle frame p50 %.3f ms, "
                "p99 %.3f ms\n",
        kKeystrokes / 4, enters[enters.size() / 2], enters[enters.size() * 99 / 100], enters.back(),
        idle[idle.size() / 2], idle[idle.size() * 99 / 100]);
}
