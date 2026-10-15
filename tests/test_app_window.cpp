// SPDX-License-Identifier: Apache-2.0
// The app on the headless platform: frames of the main window, the live preview, the unsaved
// prompt, saving and exporting, file dialogs and their fallback, theme selection, settings and
// snapshot mode.
#include "app_harness.h"
#include "headless_platform.h"
#include "runner.h"

#include "app/app.h"
#include "app/themeselect.h"
#include "common/file_io.h"
#include "gfx/png_write.h"
#include "pdf/reader.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using rgui::App;
using rgui::InputEvent;
using rgui::Key;
using rgui::StatusKind;
using rgui::test::HaveFonts;
using rgui::test::HeadlessPlatform;
using rgui::test::TesterPath;
using Harness = rgui::test::AppHarness;

constexpr int kWidth = Harness::kWidth;
constexpr int kHeight = Harness::kHeight;

fs::path TempDir(const char* name) { return rgui::test::FreshDir("test-app-window", name); }

std::string ReadFile(const fs::path& path) {
    std::string content;
    if (!RayoMd::File::ReadAll(path, 1u << 30, content)) return "<unreadable>";
    return content;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

int Channel(uint32_t p, int shift) { return static_cast<int>((p >> shift) & 0xFF); }

// Pixels near the background-to-accent line, at least halfway to the accent: antialiased strokes
// of an accent icon count, text and hairlines do not.
int AccentPixels(const Harness& h, int x0, int y0, int x1, int y1, uint32_t bg, uint32_t accent) {
    const int ar = Channel(accent, 16) - Channel(bg, 16), ag = Channel(accent, 8) - Channel(bg, 8),
              ab = Channel(accent, 0) - Channel(bg, 0);
    const double aa = static_cast<double>(ar * ar + ag * ag + ab * ab);
    int count = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            const uint32_t p = h.Pixel(x, y);
            const int dr = Channel(p, 16) - Channel(bg, 16), dg = Channel(p, 8) - Channel(bg, 8),
                      db = Channel(p, 0) - Channel(bg, 0);
            const double t = (dr * ar + dg * ag + db * ab) / aa;
            const double er = dr - t * ar, eg = dg - t * ag, eb = db - t * ab;
            if (t >= 0.5 && er * er + eg * eg + eb * eb <= 400.0) count++;
        }
    }
    return count;
}

int Luminance(uint32_t p) { return (Channel(p, 16) * 299 + Channel(p, 8) * 587 + Channel(p, 0) * 114) / 1000; }

// In the right half: a row with more than 30 % of the pane white (a page), and the dark pixels
// (text) in the columns of the first such row's white run.
bool PageWithText(const Harness& h, int* darkOut = nullptr) {
    const int x0 = kWidth / 2;
    for (int y = 0; y < kHeight - 40; y++) {
        int first = -1, last = -1, white = 0;
        for (int x = x0; x < kWidth; x++) {
            if ((h.Pixel(x, y) & 0xFFFFFF) == 0xFFFFFF) {
                white++;
                if (first < 0) first = x;
                last = x;
            }
        }
        if (white * 10 <= (kWidth - x0) * 3) continue;
        int dark = 0;
        for (int yy = 0; yy < kHeight - 32; yy++) {
            for (int x = first; x <= last; x++) dark += Luminance(h.Pixel(x, yy)) < 100;
        }
        if (darkOut) *darkOut = dark;
        return dark > 500;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------

RGUI_TEST("app.headless_frame") {
    if (!HaveFonts()) {
        std::printf("skip app.headless_frame: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("headless_frame");
    const auto start = std::chrono::steady_clock::now();
    h.Start("ui.theme=tokyo-night\n", TesterPath());
    h.Frame();
    const double firstFrame = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(h.Settle());
    const double ready = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("app.headless_frame: tester.md, first frame %.0f ms, preview ready %.0f ms\n", firstFrame, ready);

    CHECK_EQ(h.platform.lastFrameWidth, kWidth);
    CHECK_EQ(h.platform.lastFrameHeight, kHeight);
    if (h.platform.lastFrameWidth != kWidth || h.platform.lastFrameHeight != kHeight) return;
    CHECK_EQ(h.app->ThemeSource(), std::string("tokyo-night"));
    CHECK_EQ(h.Pixel(8, 400) & 0xFFFFFF, 0x1a1b26u);
    // The export icon in the accent colour among the footer's buttons.
    CHECK(AccentPixels(h, 0, kHeight - 32, 220, kHeight, 0x1a1b26, 0x7aa2f7) > 5);
    // The preview: a white page with text on it.
    int dark = 0;
    CHECK(PageWithText(h, &dark));
    CHECK(dark > 500);
    CHECK_EQ(h.platform.title, std::string("tester.md - RayoMD"));
    StatusKind kind = StatusKind::Error;
    CHECK(StartsWith(h.Window().ShownStatus(&kind), "Preview: "));
    CHECK(kind != StatusKind::Error);
    const fs::path png = h.dir / "headless_frame.png";
    rgui::gfx::Surface surface{h.platform.lastFrame.data(), kWidth, kHeight, kWidth};
    CHECK(rgui::gfx::WritePng(png, surface));
}

// Typing reaches the engine within the keystroke's frame, with no delay; what is typed while a
// build runs goes into the one build after it. A page the edit did not touch keeps its image.
RGUI_TEST("app.preview_pipeline") {
    if (!HaveFonts()) {
        std::printf("skip app.preview_pipeline: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("preview_pipeline");
    rgui::test::CancellableBuild engine;
    h.build = engine.Function();
    h.Start("ui.theme=tokyo-night\n");
    CHECK(h.Settle());
    rgui::MainWindow& w = h.Window();
    const int generation = w.RequestedGeneration();
    const int calls = engine.calls;
    engine.hold = true;
    h.Type("# Pipeline");
    CHECK(w.Doc().modified);
    CHECK_EQ(h.platform.title, std::string("* untitled.md - RayoMD"));
    // Started in the keystroke's frame: no timer runs first.
    CHECK_EQ(w.RequestedGeneration(), generation + 1);
    CHECK(w.PreviewBuilding());
    // While it builds, nothing more starts or waits in the queue.
    h.Type(" test");
    h.Type("ed");
    CHECK_EQ(w.RequestedGeneration(), generation + 1);
    CHECK(!w.PreviewSettled());
    // When it is done, the next build starts at once, with all of that typing.
    engine.hold = false;
    CHECK(h.Settle());
    CHECK_EQ(w.RequestedGeneration(), generation + 2);
    CHECK_EQ(engine.calls.load(), calls + 2);
    CHECK_EQ(std::string(w.Editor().Text()), std::string("# Pipeline tested"));
    CHECK(StartsWith(w.ShownStatus(), "Preview: 1 page in "));
    // The worker counted the words and lines of the text it built.
    CHECK_EQ(w.Stats().words, size_t(3));
    CHECK_EQ(w.Stats().lines, size_t(1));
    CHECK(PageWithText(h));
    // Two changes in one frame: one build.
    InputEvent text;
    text.type = InputEvent::Type::Text;
    text.text = "!";
    h.app->OnInput(text);
    h.app->OnInput(text);
    h.Frame();
    CHECK_EQ(w.RequestedGeneration(), generation + 3);
    CHECK(h.Settle());
    CHECK_EQ(w.RequestedGeneration(), generation + 3);

    // Two pages; then an edit of page 2 only.
    const uint64_t before = w.Preview().RenderedCount();
    w.Editor().SelectAll();
    w.Editor().PasteText("# One\n\nFirst page.\n\n\\pagebreak\n\nSecond page.\n");
    CHECK(h.Settle());
    CHECK_EQ(w.Preview().PageCount(), 2);
    // Page 2, below the view, is rendered ahead of time.
    CHECK(h.RunUntil([&] { return w.Preview().RenderedCount() == before + 2 && w.Preview().Image(1) != nullptr; }));
    const rgui::gfx::Image* first = w.Preview().Image(0);
    const rgui::gfx::Image* second = w.Preview().Image(1);
    const size_t end = w.Editor().Buffer().Size();
    w.Editor().Select(end, end);
    w.Editor().HandleText("More.");
    CHECK(h.Settle());
    CHECK(h.RunUntil([&] { return w.Preview().RenderedCount() == before + 3; }));
    // Nothing else renders: page 1 kept its image.
    h.platform.PumpUntil([] { return false; }, 100);
    h.Step();
    CHECK_EQ(w.Preview().RenderedCount(), before + 3);
    CHECK(w.Preview().Image(0) == first);
    CHECK(w.Preview().Image(1) != second);
}

// The preview keeps its place (the page at its top, and how far into it) across builds and zooms.
RGUI_TEST("app.preview_scroll") {
    if (!HaveFonts()) {
        std::printf("skip app.preview_scroll: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("preview_scroll");
    h.Start("ui.theme=tokyo-night\n", TesterPath());
    CHECK(h.Settle());
    rgui::MainWindow& w = h.Window();
    InputEvent wheel;
    wheel.type = InputEvent::Type::Wheel;
    wheel.x = 1000;
    wheel.y = 400;
    wheel.pixelsY = 1500;
    h.app->OnInput(wheel);
    h.Frame();
    h.Frame();
    CHECK_NEAR(w.PreviewScroll(), 1500.0f, 1.0f);
    CHECK(h.Settle());

    // An edit on page 1 rebuilds every page; the view stays.
    const int generation = w.ShownGeneration();
    w.Editor().Select(0, 0);
    w.Editor().HandleText("X");
    CHECK(h.RunUntil([&] { return w.ShownGeneration() > generation && w.PreviewSettled(); }));
    CHECK_NEAR(w.PreviewScroll(), 1500.0f, 1.0f);

    // Ctrl+wheel away from the user zooms in a step, around the same place of the same page:
    // everything but the 16 px margins grows by 1.1.
    wheel.pixelsY = 0;
    wheel.notchesY = -1;
    wheel.mods = rgui::kModCtrl;
    h.app->OnInput(wheel);
    h.Frame();
    h.Frame();
    CHECK_EQ(w.PreviewZoomIndex(), rgui::kPreviewZoomDefault + 1);
    const float zoomed = w.PreviewScroll();
    CHECK(zoomed > 1500.0f * 1.07f && zoomed < 1500.0f * 1.1f);
    CHECK(h.Settle());
}

RGUI_TEST("app.unsaved_prompt") {
    if (!HaveFonts()) {
        std::printf("skip app.unsaved_prompt: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("unsaved_prompt");
    const fs::path doc = h.dir / "doc.md";
    RayoMd::File::WriteAll(doc, "Hello\n");
    h.Start("ui.theme=tokyo-night\n", doc.u8string());
    CHECK(h.WaitLoaded());
    h.Frame();
    rgui::MainWindow& w = h.Window();
    h.Type("A");
    CHECK(w.Doc().modified);
    CHECK_EQ(h.platform.title, std::string("* doc.md - RayoMD"));

    // AltGr (Ctrl+Alt) is no shortcut.
    h.KeyDown(Key::N, rgui::kModCtrl | rgui::kModAlt);
    CHECK(!w.UnsavedPromptOpen());
    // New: Esc cancels, the text stays.
    h.KeyDown(Key::N, rgui::kModCtrl);
    CHECK(w.UnsavedPromptOpen());
    h.Frame();
    h.KeyDown(Key::Escape);
    CHECK(!w.UnsavedPromptOpen());
    w.Editor().Select(0, 0);
    CHECK_EQ(std::string(w.Editor().Text()), std::string("AHello\n"));
    // (Keys go to the window again from the frame after the one that closed the prompt.)
    h.Frame();
    // New: Alt+D discards.
    h.KeyDown(Key::N, rgui::kModCtrl);
    h.Frame();
    h.KeyDown(Key::D, rgui::kModAlt);
    CHECK(!w.UnsavedPromptOpen());
    CHECK(w.Editor().Buffer().Empty());
    CHECK(!w.Doc().modified);
    CHECK(w.Doc().filePath.empty());
    h.Frame();
    CHECK_EQ(h.platform.title, std::string("untitled.md - RayoMD"));
    CHECK_EQ(ReadFile(doc), std::string("Hello\n"));

    // Open with an unsaved new document: Enter saves (asking where), then the open dialog comes.
    h.Type("B");
    const fs::path saved = h.dir / "new.md";
    h.platform.ScriptDialog({rgui::FileDialogResult::Chosen, saved.u8string()});
    h.KeyDown(Key::O, rgui::kModCtrl);
    CHECK(w.UnsavedPromptOpen());
    h.Frame();
    h.KeyDown(Key::Enter);
    h.platform.Pump();
    h.Frame();
    CHECK_EQ(h.platform.dialogs.size(), size_t(2));
    if (h.platform.dialogs.size() == 2) {
        CHECK(h.platform.dialogs[0].kind == rgui::FileDialog::SaveMarkdown);
        // In the directory of the last file opened.
        CHECK_EQ(h.platform.dialogs[0].suggestedPath, (h.dir / "untitled.md").u8string());
        CHECK(h.platform.dialogs[1].kind == rgui::FileDialog::OpenDocument);
    }
    CHECK_EQ(ReadFile(saved), std::string("B"));
    CHECK_EQ(w.Doc().filePath, saved.u8string());
    CHECK(!w.Doc().modified);

    // Closing: Alt+C cancels; Alt+S saves to the document's path and quits.
    h.Type("C");
    h.app->OnCloseRequest();
    h.Frame();
    CHECK(w.UnsavedPromptOpen());
    h.KeyDown(Key::C, rgui::kModAlt);
    CHECK(!w.UnsavedPromptOpen());
    CHECK(!h.platform.quit);
    h.app->OnCloseRequest();
    h.Frame();
    h.KeyDown(Key::S, rgui::kModAlt);
    CHECK(h.platform.quit);
    CHECK_EQ(ReadFile(saved), std::string("BC"));
    CHECK_EQ(h.platform.dialogs.size(), size_t(2));
}

RGUI_TEST("app.save_and_export") {
    if (!HaveFonts()) {
        std::printf("skip app.save_and_export: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("save_and_export");
    h.Start("ui.theme=tokyo-night\n");
    h.Frame();
    rgui::MainWindow& w = h.Window();
    h.Type("# Export test");

    // Ctrl+S without a path asks where.
    const fs::path md = h.dir / "exp.md";
    h.platform.ScriptDialog({rgui::FileDialogResult::Chosen, md.u8string()});
    h.KeyDown(Key::S, rgui::kModCtrl);
    h.platform.Pump();
    CHECK_EQ(ReadFile(md), std::string("# Export test"));
    CHECK_EQ(w.Doc().displayName, std::string("exp.md"));

    // The first export asks, suggesting the document's name; later ones go to the same file.
    const fs::path pdf = h.dir / "out.pdf";
    h.platform.ScriptDialog({rgui::FileDialogResult::Chosen, pdf.u8string()});
    h.KeyDown(Key::E, rgui::kModCtrl);
    CHECK_EQ(h.platform.dialogs.size(), size_t(2));
    if (h.platform.dialogs.size() == 2) {
        CHECK(h.platform.dialogs[1].kind == rgui::FileDialog::ExportPdf);
        CHECK_EQ(h.platform.dialogs[1].suggestedPath, (h.dir / "exp.pdf").u8string());
    }
    CHECK(h.RunUntil([&] { return StartsWith(w.ShownStatus(), "Exported out.pdf (1 page, "); }));
    CHECK(StartsWith(ReadFile(pdf), "%PDF-"));
    CHECK_EQ(h.platform.openedExternally.size(), size_t(1));
    std::error_code error;
    fs::remove(pdf, error);
    h.KeyDown(Key::E, rgui::kModCtrl);
    CHECK_EQ(h.platform.dialogs.size(), size_t(2));
    CHECK(h.RunUntil([&] { return fs::exists(pdf); }));

    // Ctrl+Shift+E asks again; without a system dialog the app asks itself, with the path in a
    // field (Enter accepts).
    const fs::path other = h.dir / "fallback.pdf";
    h.platform.ScriptDialog({rgui::FileDialogResult::Unavailable, {}});
    h.KeyDown(Key::E, rgui::kModCtrl | rgui::kModShift);
    CHECK_EQ(h.platform.dialogs.size(), size_t(3));
    h.platform.Pump();
    CHECK(w.PathPromptOpen());
    h.Frame();
    h.KeyDown(Key::A, rgui::kModCtrl);
    h.Type(other.u8string());
    h.KeyDown(Key::Enter);
    CHECK(!w.PathPromptOpen());
    CHECK(h.RunUntil([&] { return StartsWith(w.ShownStatus(), "Exported fallback.pdf"); }));
    CHECK(fs::exists(other));
    // Esc on the fallback prompt cancels.
    h.platform.ScriptDialog({rgui::FileDialogResult::Unavailable, {}});
    h.KeyDown(Key::E, rgui::kModCtrl | rgui::kModShift);
    h.platform.Pump();
    h.Frame();
    CHECK(w.PathPromptOpen());
    h.KeyDown(Key::Escape);
    CHECK(!w.PathPromptOpen());
    h.Frame();
    const size_t dialogs = h.platform.dialogs.size();

    // A failed system dialog says so.
    h.platform.ScriptDialog({rgui::FileDialogResult::Failed, {}});
    h.KeyDown(Key::E, rgui::kModCtrl | rgui::kModShift);
    h.platform.Pump();
    CHECK_EQ(h.platform.dialogs.size(), dialogs + 1);
    StatusKind kind = StatusKind::Info;
    CHECK_EQ(w.ShownStatus(&kind), std::string("File dialog failed"));
    CHECK(kind == StatusKind::Error);

    // A target without the .pdf extension gets it, also as the remembered target.
    h.platform.ScriptDialog({rgui::FileDialogResult::Chosen, (h.dir / "noext").u8string()});
    h.KeyDown(Key::E, rgui::kModCtrl | rgui::kModShift);
    CHECK(h.RunUntil([&] { return StartsWith(w.ShownStatus(), "Exported noext.pdf ("); }));
    CHECK(fs::exists(h.dir / "noext.pdf"));
    CHECK(!fs::exists(h.dir / "noext"));
    fs::remove(h.dir / "noext.pdf", error);
    const size_t asked = h.platform.dialogs.size();
    h.KeyDown(Key::E, rgui::kModCtrl);
    CHECK(h.RunUntil([&] { return fs::exists(h.dir / "noext.pdf"); }));
    CHECK_EQ(h.platform.dialogs.size(), asked);
    CHECK(!fs::exists(h.dir / "noext"));

    // Drops: a Markdown file opens (the document is saved), anything else is refused, and an
    // edited document asks first.
    const fs::path dropped = h.dir / "dropped.markdown";
    RayoMd::File::WriteAll(dropped, "Dropped\n");
    h.app->OnDropFile(h.dir.u8string() + "/picture.PNG");
    CHECK_EQ(w.ShownStatus(&kind), std::string("Not a Markdown, text or PDF file: picture.PNG"));
    CHECK(kind == StatusKind::Warning);
    h.app->OnDropFile(dropped.u8string());
    CHECK(h.WaitLoaded());
    CHECK_EQ(w.Doc().filePath, dropped.u8string());
    CHECK_EQ(w.Doc().text, std::string("Dropped\n"));
    h.Frame();
    h.Type("x");
    h.app->OnDropFile(md.u8string());
    CHECK(w.UnsavedPromptOpen());
    CHECK_EQ(w.Doc().filePath, dropped.u8string());
}

RGUI_TEST("app.theme_selection") {
    const fs::path dir = TempDir("theme_selection");
    const fs::path omarchy = dir / "omarchy";
    fs::create_directories(omarchy);
    RayoMd::File::WriteAll(omarchy / "colors.toml",
                           "accent = \"#ff5555\"\nbackground = \"#123456\"\nforeground = \"#c0caf5\"\n");

    rgui::ThemeRequest request;
    request.setting = rgui::kThemeOmarchy;
    request.omarchyThemeDir = omarchy;
    rgui::LoadedTheme theme = rgui::LoadTheme(request);
    CHECK_EQ(theme.source, std::string("omarchy"));
    CHECK(theme.warning.empty());
    CHECK(theme.tokens.colors.windowBackground == (rgui::ui::Rgba{0x12, 0x34, 0x56, 255}));
    CHECK(theme.tokens.colors.accent == (rgui::ui::Rgba{0xff, 0x55, 0x55, 255}));
    // `auto` follows Omarchy too where it exists.
    request.setting = rgui::kThemeAuto;
    CHECK_EQ(rgui::LoadTheme(request).source, std::string("omarchy"));

    // An unreadable Omarchy theme falls back to Tokyo Night and says so.
    request.setting = rgui::kThemeOmarchy;
    request.omarchyThemeDir = dir / "missing";
    theme = rgui::LoadTheme(request);
    CHECK_EQ(theme.source, std::string("tokyo-night"));
    CHECK_EQ(theme.warning, std::string(rgui::kOmarchyUnreadable));

    // Automatic without Omarchy: the system's light or dark.
    request.omarchyThemeDir.reset();
    request.systemDark = false;
    CHECK_EQ(rgui::LoadTheme(request).source, std::string("catppuccin-latte"));
    CHECK(!rgui::LoadTheme(request).tokens.dark);
    request.systemDark = true;
    CHECK_EQ(rgui::LoadTheme(request).source, std::string("tokyo-night"));
    request.setting = "nord";
    CHECK_EQ(rgui::LoadTheme(request).source, std::string("nord"));
    request.setting = "no-such-theme";
    CHECK_EQ(rgui::LoadTheme(request).source, std::string("tokyo-night"));

    // The menu.
    const std::vector<std::string> withOmarchy = rgui::ThemeMenuItems(true);
    const std::vector<std::string> without = rgui::ThemeMenuItems(false);
    CHECK_EQ(withOmarchy[0], std::string("Follow Omarchy"));
    CHECK_EQ(without[0], std::string("Automatic"));
    const int nord = rgui::ThemeMenuIndex("nord");
    CHECK_EQ(without[static_cast<size_t>(nord)], std::string("Nord"));
    CHECK_EQ(rgui::ThemeSettingOf(nord, true), std::string("nord"));
    CHECK_EQ(rgui::ThemeMenuIndex("auto"), 0);
    CHECK_EQ(rgui::ThemeSettingOf(0, false), std::string("auto"));
    CHECK_EQ(rgui::ThemeSettingOf(0, true), std::string("omarchy"));

    if (!HaveFonts()) {
        std::printf("skip app.theme_selection (app part): DejaVu Sans Mono not installed\n");
        return;
    }
    // The app follows Omarchy by default, tells the window frame, and falls back live.
    Harness h("theme_selection_app");
    h.platform.omarchyThemeDir = omarchy;
    h.Start({});
    h.Frame();
    CHECK_EQ(h.app->ThemeSource(), std::string("omarchy"));
    CHECK(h.platform.windowColorCalls >= 1);
    CHECK_EQ(h.platform.windowBackground, 0xFF123456u);
    CHECK_EQ(h.platform.windowAccent, 0xFFFF5555u);
    CHECK_EQ(h.Pixel(8, 400) & 0xFFFFFF, 0x123456u);
    CHECK(h.Settle());

    fs::remove(omarchy / "colors.toml");
    const int calls = h.platform.windowColorCalls;
    h.app->OnOmarchyChanged();
    h.Frame();
    CHECK_EQ(h.app->ThemeSource(), std::string("tokyo-night"));
    CHECK_EQ(h.platform.windowColorCalls, calls + 1);
    StatusKind kind = StatusKind::Info;
    CHECK_EQ(h.Window().ShownStatus(&kind), std::string(rgui::kOmarchyUnreadable));
    CHECK(kind == StatusKind::Warning);
    CHECK_EQ(h.Pixel(8, 400) & 0xFFFFFF, 0x1a1b26u);

    // Without Omarchy, the system's appearance picks.
    h.platform.omarchyThemeDir.reset();
    h.app->OnOmarchyChanged();
    h.app->OnSystemAppearance(false, 1.0);
    CHECK_EQ(h.app->ThemeSource(), std::string("catppuccin-latte"));
    CHECK(!h.platform.windowDark);
    h.app->OnSystemAppearance(true, 1.0);
    CHECK_EQ(h.app->ThemeSource(), std::string("tokyo-night"));
}

RGUI_TEST("app.settings_saved") {
    if (!HaveFonts()) {
        std::printf("skip app.settings_saved: DejaVu Sans Mono not installed\n");
        return;
    }
    {
        Harness h("settings_saved");
        h.Start("ui.theme=nord\n");
        h.Frame();
        CHECK(!h.Window().OptionsVisible());
        h.KeyDown(Key::Comma, rgui::kModCtrl);
        CHECK(h.Window().OptionsVisible());
        h.platform.Advance(499);
        rgui::ui::SettingsFile file;
        CHECK(file.Load(h.SettingsPath()));
        CHECK(!file.Has("ui.optionsVisible"));
        h.platform.Advance(1);
        CHECK(file.Load(h.SettingsPath()));
        CHECK(file.GetBool("ui.optionsVisible", false));
        CHECK_EQ(file.GetString("ui.theme"), std::string("nord"));
        CHECK_EQ(file.GetInt("ui.windowWidth", 0), kWidth);
        // The options panel draws.
        h.Frame();

        // Quitting saves at once.
        h.KeyDown(Key::Equal, rgui::kModCtrl);
        h.KeyDown(Key::Q, rgui::kModCtrl);
        CHECK(h.platform.quit);
        CHECK(file.Load(h.SettingsPath()));
        CHECK_NEAR(file.GetDouble("ui.editorZoom", 0), 1.1, 1e-6);
    }
    {
        // Snapshot mode saves nothing.
        Harness h("settings_snapshot");
        h.Start({}, {}, (h.dir / "unused.png").u8string());
        h.Frame();
        h.KeyDown(Key::Comma, rgui::kModCtrl);
        h.platform.Advance(1000);
        h.app->SaveSettings();
        CHECK(!fs::exists(h.SettingsPath()));
    }
}

RGUI_TEST("app.client_decorations") {
    if (!HaveFonts()) {
        std::printf("skip app.client_decorations: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("client_decorations");
    h.platform.clientDecorations = true;
    h.Start("ui.theme=tokyo-night\n");
    h.Frame();
    // The header: a press drags, a double click maximizes.
    h.Click(300, 18, 1000);
    CHECK_EQ(h.platform.moves, 1);
    h.Click(300, 18);
    h.Click(300, 18);
    CHECK_EQ(h.platform.maximizeToggles, 1);
    h.platform.Advance(1000);
    // The edges resize, the corners diagonally.
    h.Click(kWidth - 2, 400, 1000);
    h.Click(2, kHeight - 2, 1000);
    CHECK_EQ(h.platform.resizes.size(), size_t(2));
    if (h.platform.resizes.size() == 2) {
        CHECK(h.platform.resizes[0] == rgui::Edge::Right);
        CHECK(h.platform.resizes[1] == rgui::Edge::BottomLeft);
    }
    // Maximized: no resize edges.
    h.platform.maximized = true;
    h.Frame();
    h.Click(kWidth - 2, 400, 1000);
    CHECK_EQ(h.platform.resizes.size(), size_t(2));
    // The close button (right of the header) quits an unmodified document.
    h.Click(kWidth - 4 - 14, 18);
    CHECK(h.platform.quit);
}

// Files open on the loader's thread; the newest request wins; a failure keeps the document.
RGUI_TEST("app.open_async") {
    if (!HaveFonts()) {
        std::printf("skip app.open_async: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("open_async");
    const fs::path a = h.dir / "a.md";
    const fs::path b = h.dir / "b.md";
    const fs::path missing = h.dir / "missing.md";
    RayoMd::File::WriteAll(a, "# A\n\nalpha beta\n");
    RayoMd::File::WriteAll(b, "# B\n");
    h.Start("ui.theme=tokyo-night\n");
    CHECK(h.Settle());
    rgui::MainWindow& w = h.Window();

    // While the file opens, the UI says so and keeps drawing; the old document stays.
    w.Open(a.u8string());
    CHECK(w.Loading());
    CHECK_EQ(w.ShownStatus(), std::string("Opening a.md\xE2\x80\xA6"));
    CHECK(w.Editor().Buffer().Empty());
    h.app->OnFrame();
    CHECK(!w.PreviewSettled());
    CHECK(h.WaitLoaded());
    CHECK_EQ(std::string(w.Editor().Text()), std::string("# A\n\nalpha beta\n"));
    CHECK_EQ(w.Doc().filePath, a.u8string());
    CHECK(!w.Doc().modified);
    // Counted on the loader's thread.
    CHECK_EQ(w.Stats().words, size_t(4));
    CHECK_EQ(w.Stats().lines, size_t(4));
    CHECK(h.Settle());
    CHECK_EQ(h.platform.title, std::string("a.md - RayoMD"));

    // A later open replaces one still opening.
    w.Open(missing.u8string());
    w.Open(b.u8string());
    CHECK(h.WaitLoaded());
    CHECK_EQ(w.Doc().filePath, b.u8string());
    CHECK_EQ(w.ShownStatus(), std::string());
    CHECK(h.Settle());

    // A file that cannot be opened: the document stays, the status says why.
    w.Open(missing.u8string());
    CHECK(h.WaitLoaded());
    StatusKind kind = StatusKind::Info;
    CHECK_EQ(w.ShownStatus(&kind), std::string("Cannot read missing.md"));
    CHECK(kind == StatusKind::Error);
    CHECK_EQ(w.Doc().filePath, b.u8string());
    CHECK_EQ(std::string(w.Editor().Text()), std::string("# B\n"));
    CHECK(h.Settle());

    // A new document while a file opens: the file is not shown when it comes.
    w.Open(a.u8string());
    InputEvent key;
    key.type = InputEvent::Type::KeyDown;
    key.key = Key::N;
    key.mods = rgui::kModCtrl;
    h.app->OnInput(key);
    h.app->OnFrame();   // before anything posted runs
    CHECK(!w.Loading());
    CHECK(h.Settle());
    h.platform.PumpUntil([] { return false; }, 50);
    h.Step();
    CHECK(w.Doc().filePath.empty());
    CHECK(w.Editor().Buffer().Empty());
}

// The preview build in flight is cancelled when the document changes, when an export comes
// (and built again after it), when it runs over its time budget, and when the window goes.
RGUI_TEST("app.preview_cancel") {
    if (!HaveFonts()) {
        std::printf("skip app.preview_cancel: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("preview_cancel");
    rgui::test::CancellableBuild engine;
    h.build = engine.Function();
    h.Start("ui.theme=tokyo-night\n");
    CHECK(h.Settle());
    rgui::MainWindow& w = h.Window();

    // Opening another document.
    const fs::path doc = h.dir / "doc.md";
    RayoMd::File::WriteAll(doc, "# Doc\n");
    engine.shots = 1;
    h.Type("slow");
    CHECK(engine.WaitBlocked());
    w.Open(doc.u8string());
    CHECK(h.Settle());
    CHECK_EQ(engine.cancelled.load(), 1);
    CHECK_EQ(std::string(w.Editor().Text()), std::string("# Doc\n"));
    CHECK(StartsWith(w.ShownStatus(), "Preview: 1 page in "));

    // An export: the preview waits for it, then builds, with no new request.
    engine.shots = 1;
    h.Type("x");
    CHECK(engine.WaitBlocked());
    const int generation = w.RequestedGeneration();
    const fs::path pdf = h.dir / "out.pdf";
    h.platform.ScriptDialog({rgui::FileDialogResult::Chosen, pdf.u8string()});
    h.KeyDown(Key::E, rgui::kModCtrl);
    CHECK(h.RunUntil([&] { return fs::exists(pdf); }));
    CHECK(h.Settle());
    CHECK_EQ(engine.cancelled.load(), 2);
    CHECK_EQ(w.RequestedGeneration(), generation);
    CHECK_EQ(w.ShownGeneration(), generation);
    CHECK(StartsWith(ReadFile(pdf), "%PDF-"));

    // Over the time budget: stopped, said, not built again until the next change.
    engine.shots = 1;
    h.Type("y");
    CHECK(engine.WaitBlocked());
    const int stopped = w.RequestedGeneration();
    h.platform.Advance(rgui::MainWindow::kPreviewBudgetMs - 1);
    CHECK(w.PreviewBuilding());
    CHECK_EQ(engine.cancelled.load(), 2);
    h.platform.Advance(1);
    CHECK(h.RunUntil([&] { return !w.PreviewBuilding(); }));
    CHECK_EQ(engine.cancelled.load(), 3);
    StatusKind kind = StatusKind::Info;
    CHECK_EQ(w.ShownStatus(&kind), std::string("Preview stopped: the document takes too long to build"));
    CHECK(kind == StatusKind::Warning);
    h.platform.PumpUntil([] { return false; }, 50);
    h.Step();
    CHECK_EQ(w.RequestedGeneration(), stopped);
    CHECK(!w.PreviewSettled());
    h.Type("z");
    CHECK(h.Settle());
    CHECK_EQ(w.RequestedGeneration(), stopped + 1);
    CHECK(StartsWith(w.ShownStatus(), "Preview: "));

    // Quitting with a build that would not end by itself.
    engine.shots = 1;
    h.Type("q");
    CHECK(engine.WaitBlocked());
    const auto start = std::chrono::steady_clock::now();
    h.app.reset();
    const double quitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(quitMs < 1000);
    CHECK_EQ(engine.cancelled.load(), 4);
}

RGUI_TEST("app.snapshot") {
    if (!HaveFonts()) {
        std::printf("skip app.snapshot: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("snapshot");
    const fs::path png = h.dir / "snapshot.png";
    h.Start("ui.theme=tokyo-night\n", TesterPath(), png.u8string());
    CHECK(h.RunUntil([&] { return h.platform.quit; }));
    CHECK_EQ(h.app->ExitCode(), 0);
    CHECK(StartsWith(h.app->ExitStdout(), "snapshot: first frame "));
    CHECK(h.app->ExitStderr().empty());
    std::printf("app.snapshot: %s", h.app->ExitStdout().c_str());
    const std::string bytes = ReadFile(png);
    CHECK(StartsWith(bytes, "\x89PNG\r\n\x1a\n"));
    // The settings file was not written.
    CHECK_EQ(ReadFile(h.SettingsPath()), std::string("ui.theme=tokyo-night\n"));

    // No frame with the preview in 10 s (none is drawn here): timed out, exit code 1.
    Harness late("snapshot_timeout");
    late.Start("ui.theme=tokyo-night\n", {}, (late.dir / "late.png").u8string());
    late.platform.Advance(App::kSnapshotTimeoutMs - 1);
    CHECK(!late.platform.quit);
    late.platform.Advance(1);
    CHECK(late.platform.quit);
    CHECK_EQ(late.app->ExitCode(), 1);
    CHECK_EQ(late.app->ExitStderr(), std::string("snapshot: timed out\n"));
    CHECK(!fs::exists(late.dir / "late.png"));
}

} // namespace
