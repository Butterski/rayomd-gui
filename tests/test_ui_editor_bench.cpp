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
#include <string>
#include <vector>

// 1,000 characters typed one at a time into the middle of a 1 MiB document (tester.md
// repeated), each a full frame at 1920 x 1080: the editor's frame (input, layout, display list)
// and the compositor's raster of the tiles that changed. Prints p50 and p99.
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
    CHECK(RayoMd::File::ReadAll(std::string(rgui::test::SourceDir()) + "/external/rayomd/tester.md", 8u << 20, tester));
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
    size_t damaged = 0;
    auto frame = [&] {
        ctx.BeginFrame(kWidth, kHeight, 1.0, now);
        editor.Frame(ctx, pane);
        const rgui::ui::FrameOutput& out = ctx.EndFrame();
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

    const char typed[] = "The quick brown fox jumps over the lazy dog. ";
    std::vector<double> ms;
    ms.reserve(1000);
    for (int i = 0; i < 1000; i++) {
        rgui::InputEvent text;
        text.type = rgui::InputEvent::Type::Text;
        text.text.assign(1, typed[i % (sizeof(typed) - 1)]);
        now += 50;
        ctx.AddEvent(text, now);
        const auto t0 = std::chrono::steady_clock::now();
        frame();
        const auto t1 = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    CHECK_EQ(editor.Buffer().Size(), document.size() + 1000);
    CHECK(damaged > 0);
    std::sort(ms.begin(), ms.end());
    std::printf("bench.typing: 1000 keystrokes, %zu-byte document, %dx%d, %d tile threads: "
                "p50 %.3f ms, p99 %.3f ms, max %.3f ms\n",
        document.size(), kWidth, kHeight, compositor.Threads(), ms[499], ms[989], ms.back());
}
