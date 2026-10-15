// SPDX-License-Identifier: Apache-2.0
// The app on the headless platform: frames of the main window, the live preview, the unsaved
// prompt, saving and exporting, file dialogs and their fallback, theme selection, settings and
// snapshot mode.
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
using rgui::test::HeadlessPlatform;

constexpr int kWidth = 1280;
constexpr int kHeight = 820;

fs::path TempDir(const char* name) {
    const fs::path dir = fs::u8path(rgui::test::BuildDir()) / "test-app-window" / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir, error);
    return dir;
}

std::string ReadFile(const fs::path& path) {
    std::string content;
    if (!RayoMd::File::ReadAll(path, 1u << 30, content)) return "<unreadable>";
    return content;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

std::string TesterPath() { return std::string(rgui::test::SourceDir()) + "/external/rayomd/tester.md"; }

bool HaveFonts() {
    static const bool have = [] {
        rgui::text::FontIndex index;
        index.SetDirectories({"/usr/share/fonts"});
        return index.Find("DejaVu Sans Mono", 400, false) != nullptr;
    }();
    return have;
}

// One app on a headless platform, its settings and files in a directory of its own.
struct Harness {
    HeadlessPlatform platform;
    std::unique_ptr<App> app;
    fs::path dir;
    int framesSeen = 0;

    explicit Harness(const char* name) : dir(TempDir(name)) {}

    fs::path SettingsPath() const { return dir / "settings.ini"; }

    // `settings` is the settings file's text (none when empty).
    void Start(const std::string& settings, const std::string& file = {}, const std::string& snapshotPath = {}) {
        if (!settings.empty()) RayoMd::File::WriteAll(SettingsPath(), settings);
        rgui::Startup startup;
        startup.args.mode = snapshotPath.empty() ? rgui::Args::Mode::Run : rgui::Args::Mode::Snapshot;
        startup.args.snapshotPath = snapshotPath;
        startup.args.file = file;
        startup.settingsPath = SettingsPath();
        startup.settings.Load(startup.settingsPath);
        startup.fontIndex = std::make_unique<rgui::text::FontIndex>();
        startup.fontIndex->SetDirectories({"/usr/share/fonts"});
        startup.fontIndex->Start();
        startup.fontPaths = {};
        app = App::Create(platform, std::move(startup));
        app->OnResize(kWidth, kHeight, 1.0);
    }

    rgui::MainWindow& Window() { return app->Window(); }

    // Runs what is posted and due, then a frame if one was requested.
    void Step() {
        platform.Pump();
        if (platform.frameRequests != framesSeen) {
            framesSeen = platform.frameRequests;
            app->OnFrame();
        }
    }
    // A frame now, requested or not.
    void Frame() {
        platform.Pump();
        framesSeen = platform.frameRequests;
        app->OnFrame();
    }
    // Steps until `done` (frames as they are requested, waiting for the workers in between).
    bool RunUntil(const std::function<bool()>& done, int timeoutMs = 30000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            Step();
            if (done()) return true;
            if (std::chrono::steady_clock::now() > deadline) return false;
            if (platform.frameRequests == framesSeen) {
                platform.PumpUntil([&] { return platform.frameRequests != framesSeen || done(); }, 20);
            }
        }
    }
    bool Settle() {
        return RunUntil([&] {
            return Window().PreviewSettled() && Window().ShownGeneration() == Window().RequestedGeneration();
        });
    }

    void KeyDown(Key key, uint8_t mods = 0) {
        InputEvent e;
        e.type = InputEvent::Type::KeyDown;
        e.key = key;
        e.mods = mods;
        app->OnInput(e);
        Frame();
    }
    void Type(const std::string& text) {
        InputEvent e;
        e.type = InputEvent::Type::Text;
        e.text = text;
        app->OnInput(e);
        Frame();
    }
    void Click(float x, float y, double gapMs = 0) {
        InputEvent e;
        e.type = InputEvent::Type::PointerMove;
        e.x = x;
        e.y = y;
        app->OnInput(e);
        e.type = InputEvent::Type::Button;
        e.pressed = true;
        app->OnInput(e);
        Frame();
        e.pressed = false;
        app->OnInput(e);
        Frame();
        if (gapMs > 0) platform.Advance(static_cast<int>(gapMs));
    }

    uint32_t Pixel(int x, int y) const {
        return platform.lastFrame[static_cast<size_t>(y) * static_cast<size_t>(platform.lastFrameWidth) +
                                  static_cast<size_t>(x)];
    }
};

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

// Typing reaches the preview 80 ms after the last change; a page the edit did not touch keeps
// its image.
RGUI_TEST("app.preview_pipeline") {
    if (!HaveFonts()) {
        std::printf("skip app.preview_pipeline: DejaVu Sans Mono not installed\n");
        return;
    }
    Harness h("preview_pipeline");
    h.Start("ui.theme=tokyo-night\n");
    CHECK(h.Settle());
    const int generation = h.Window().RequestedGeneration();
    h.Type("# Pipeline");
    CHECK(h.Window().Doc().modified);
    CHECK_EQ(h.platform.title, std::string("* untitled.md - RayoMD"));
    h.platform.Advance(79);
    CHECK_EQ(h.Window().RequestedGeneration(), generation);
    h.platform.Advance(1);
    CHECK_EQ(h.Window().RequestedGeneration(), generation + 1);
    // Typing again restarts the wait.
    h.Type(" test");
    h.platform.Advance(60);
    h.Type("ed");
    h.platform.Advance(60);
    CHECK_EQ(h.Window().RequestedGeneration(), generation + 1);
    h.platform.Advance(20);
    CHECK_EQ(h.Window().RequestedGeneration(), generation + 2);
    CHECK(h.Settle());
    CHECK_EQ(h.Window().Doc().text, std::string("# Pipeline tested"));
    CHECK(StartsWith(h.Window().ShownStatus(), "Preview: 1 page in "));
    CHECK(PageWithText(h));

    // Two pages; then an edit of page 2 only.
    rgui::MainWindow& w = h.Window();
    const uint64_t before = w.Preview().RenderedCount();
    w.Editor().SelectAll();
    w.Editor().PasteText("# One\n\nFirst page.\n\n\\pagebreak\n\nSecond page.\n");
    h.platform.Advance(80);
    CHECK(h.Settle());
    CHECK_EQ(w.Preview().PageCount(), 2);
    // Page 2, below the view, is rendered ahead of time.
    CHECK(h.RunUntil([&] { return w.Preview().RenderedCount() == before + 2 && w.Preview().Image(1) != nullptr; }));
    const rgui::gfx::Image* first = w.Preview().Image(0);
    const rgui::gfx::Image* second = w.Preview().Image(1);
    const size_t end = w.Editor().Buffer().Size();
    w.Editor().Select(end, end);
    w.Editor().HandleText("More.");
    h.platform.Advance(80);
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
    h.platform.Advance(80);
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

    // Drops: a Markdown file opens (the document is saved), anything else is refused, and an
    // edited document asks first.
    const fs::path dropped = h.dir / "dropped.markdown";
    RayoMd::File::WriteAll(dropped, "Dropped\n");
    h.app->OnDropFile(h.dir.u8string() + "/picture.PNG");
    CHECK_EQ(w.ShownStatus(&kind), std::string("Not a Markdown, text or PDF file: picture.PNG"));
    CHECK(kind == StatusKind::Warning);
    h.app->OnDropFile(dropped.u8string());
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

RGUI_TEST("app.recovery_copy") {
    if (!HaveFonts()) {
        std::printf("skip app.recovery_copy: DejaVu Sans Mono not installed\n");
        return;
    }
    // The compositor went away: the platform asks for a copy of the unsaved text before exiting.
    Harness h("recovery_copy");
    h.Start("ui.theme=tokyo-night\n");
    h.Frame();
    CHECK(!h.app->SaveRecoveryCopy());  // nothing typed: nothing to keep
    CHECK(!fs::exists(h.dir / "recovery"));
    h.Type("unsaved words");
    CHECK(h.Window().Doc().modified);
    CHECK(h.app->SaveRecoveryCopy());
    CHECK(h.app->SaveRecoveryCopy());  // a second copy in the same second gets its own name
    std::vector<fs::path> copies;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(h.dir / "recovery", error)) copies.push_back(entry.path());
    CHECK_EQ(copies.size(), size_t(2));
    for (const fs::path& copy : copies) {
        CHECK(StartsWith(copy.filename().u8string(), "untitled-"));
        CHECK_EQ(copy.extension().u8string(), std::string(".md"));
        CHECK_EQ(ReadFile(copy), std::string("unsaved words"));
    }
}

RGUI_TEST("app.no_settings_path") {
    if (!HaveFonts()) {
        std::printf("skip app.no_settings_path: DejaVu Sans Mono not installed\n");
        return;
    }
    // No settings directory (Windows without APPDATA): nothing is read or written anywhere, the
    // working directory included.
    const fs::path dir = TempDir("no_settings_path");
    std::error_code error;
    const fs::path cwd = fs::current_path(error);
    fs::current_path(dir, error);
    RayoMd::File::WriteAll(dir / "settings.ini", "ui.theme=nord\n");
    {
        HeadlessPlatform platform;
        rgui::Startup startup = rgui::BeginStartup(rgui::Args{}, fs::path(), std::chrono::steady_clock::now());
        CHECK(!startup.settings.Has("ui.theme"));
        startup.fontIndex = std::make_unique<rgui::text::FontIndex>();
        startup.fontIndex->SetDirectories({"/usr/share/fonts"});
        startup.fontIndex->Start();
        std::unique_ptr<App> app = App::Create(platform, std::move(startup));
        app->OnResize(kWidth, kHeight, 1.0);
        app->SaveSettings();
        CHECK(!app->SaveRecoveryCopy());
        platform.Advance(1000);
    }
    fs::current_path(cwd, error);
    size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        (void)entry;
        ++entries;
    }
    CHECK_EQ(entries, size_t(1));  // the settings.ini written above, nothing else
    CHECK_EQ(ReadFile(dir / "settings.ini"), std::string("ui.theme=nord\n"));
}

} // namespace
