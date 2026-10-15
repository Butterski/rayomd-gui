// SPDX-License-Identifier: Apache-2.0
// The app: the PlatformClient that turns the platform's events into frames of the main window,
// and the startup sequence the executables follow (plan Phase 8 step 9).
//
// main_linux.cpp / main_win32.cpp:
//
//     const auto processStart = std::chrono::steady_clock::now();      // first thing in main
//     rgui::Args args = rgui::ParseArgs(arguments);                     // UTF-8, without argv[0]
//     if (args.mode == rgui::Args::Mode::Version) { print "rayomd-gui " + rgui::Version(); return 0; }
//     if (args.mode == rgui::Args::Mode::Usage) { print Args::kUsage to stderr; return Args::kUsageExitCode; }
//     // Starts the font index scan and reads the settings. The path is ConfigDir() / the settings
//     // file name: Linux rgui::wl::XdgConfigDir() / "settings.ini", Windows
//     // window.ConfigDir() / L"rayomd-gui.ini" (the Win32Window object, before Create); empty
//     // when there is no config directory (no settings are read or written then).
//     rgui::Startup startup = rgui::BeginStartup(args, settingsPath, processStart);
//     // Windows only: startup.doubleClickMs = GetDoubleClickTime();
//     create the window with startup.windowWidth x startup.windowHeight (logical px) and
//         startup.maximized;
//     std::unique_ptr<rgui::App> app = rgui::App::Create(window, std::move(startup));
//         // picks the interface font, wears the theme, starts opening the file
//     window.SetClient(app.get());
//     run the event loop until Platform::Quit;  (Linux SIGINT / SIGTERM: app->SaveSettings(),
//         then leave the loop without prompting; the compositor gone: app->SaveRecoveryCopy()
//         and app->SaveSettings(), exit 1)
//     write app->ExitStdout() to stdout and app->ExitStderr() to stderr;
//     return app->ExitCode();
//
// The app saves its settings itself (500 ms after a change, and when it quits), never in
// snapshot mode. In snapshot mode it quits by itself: after the first frame whose preview is
// complete (ExitCode 0, the timings in ExitStdout) or after 10 s (ExitCode 1).
#pragma once

#include "app/args.h"
#include "app/mainwindow.h"
#include "app/platform.h"
#include "text/fontchoice.h"
#include "text/fontindex.h"
#include "text/glyphcache.h"
#include "ui/compositor.h"
#include "ui/icons.h"
#include "ui/settingsfile.h"
#include "ui/tokens.h"
#include "ui/ui.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rgui {

// What happens before the window exists.
struct Startup {
    Args args;
    std::filesystem::path settingsPath;
    std::chrono::steady_clock::time_point processStart = std::chrono::steady_clock::now();
    // Started (scanning on its thread); App::Create waits for it unless the interface font
    // comes from its known files (Windows).
    std::unique_ptr<text::FontIndex> fontIndex;
    // The fontconfig files that choose the interface family (FontChoicePaths::Default()).
    text::FontChoicePaths fontPaths;
    ui::SettingsFile settings;
    // The window to create: the saved size (logical px, at least 720 x 520) and state.
    int windowWidth = 1280;
    int windowHeight = 820;
    bool maximized = false;
    // The double-click time in ms (Windows: GetDoubleClickTime()).
    int doubleClickMs = 300;
    // Replaces TinyPdf::BuildPdf in the build worker (tests); nullptr: the engine.
    BuildWorker::BuildFunction build;
};

// Starts the font index on the default font directories and loads the settings at
// `settingsPath` (a missing file gives the defaults).
Startup BeginStartup(const Args& args, std::filesystem::path settingsPath,
                     std::chrono::steady_clock::time_point processStart);

class App final : public PlatformClient {
public:
    static constexpr int kMinWidth = 720;
    static constexpr int kMinHeight = 520;
    static constexpr int kSettingsDelayMs = 500;
    static constexpr int kSnapshotTimeoutMs = 10000;

    // Loads the interface font (FontSet::Load: from its known files where it can, else after
    // waiting for the font index), wears the theme, starts opening startup.args.file on the
    // loader's thread (or builds the empty document's preview) and requests the first frame.
    // `platform` must outlive the App.
    static std::unique_ptr<App> Create(Platform& platform, Startup startup);
    ~App() override;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ---- PlatformClient ----
    void OnInput(const InputEvent& event) override;
    void OnResize(int width, int height, double scale) override;
    void OnCloseRequest() override;
    void OnDropFile(std::string path) override;
    void OnPlatformError(std::string message) override;
    void OnSystemAppearance(bool dark, double textScale) override;
    void OnOmarchyChanged() override;
    void OnFontsChanged() override;
    void OnRoundingChanged(int rounding) override;
    void OnFrame() override;

    // Writes the settings now (not in snapshot mode, nor without a settings path).
    void SaveSettings();
    // When the document has unsaved changes, writes the editor's text to
    // <settings dir>/recovery/<name>-<YYYYmmdd-HHMMSS>.md and says so on stderr; for a platform
    // that lost its window (the compositor went away) and must exit. False when nothing was
    // written.
    bool SaveRecoveryCopy();

    // After the loop: the process's exit code and what to print.
    int ExitCode() const { return exitCode_; }
    const std::string& ExitStdout() const { return exitStdout_; }
    const std::string& ExitStderr() const { return exitStderr_; }

    // ---- For tests ----
    MainWindow& Window() { return *window_; }
    ui::Context& Context() { return ctx_; }
    const ui::Tokens& Tokens() const { return tokens_; }
    const ui::Palette& Palette() const { return palette_; }
    const std::string& ThemeSource() const { return themeSource_; }
    const text::FontSet& Fonts() const { return *fonts_; }
    // The framebuffer (device pixels) of the last frame.
    gfx::Surface Framebuffer();

private:
    App(Platform& platform, Startup startup);

    void LoadFonts();
    // The font index's scan has ended: text shaped without fallbacks it needed is shaped again.
    void OnFontIndexScanned();
    void WearTheme();
    void UpdateNotice();
    void SettingsChanged();
    void Quit();
    void ScheduleDeadline(double deadlineMs);
    void SnapshotFrame();
    double SinceStartMs() const;

    Platform& platform_;
    Args args_;
    std::filesystem::path settingsPath_;
    std::chrono::steady_clock::time_point processStart_;
    std::unique_ptr<text::FontIndex> fontIndex_;
    text::FontChoicePaths fontPaths_;
    ui::SettingsFile settings_;
    bool snapshot_ = false;

    std::unique_ptr<text::FontSet> fonts_;
    ui::Palette palette_;
    ui::Tokens tokens_;
    std::string themeSource_;
    std::string themeWarning_;
    bool systemDark_ = true;
    double systemTextScale_ = 1.0;
    std::optional<int> hyprlandRounding_;

    ui::Context ctx_;
    ui::Compositor compositor_;
    text::GlyphCache glyphs_;
    ui::IconCache icons_;
    std::unique_ptr<MainWindow> window_;

    // The framebuffer, device pixels.
    int width_ = 0;
    int height_ = 0;
    double scale_ = 1.0;
    std::vector<uint32_t> pixels_;
    bool presented_ = false;
    // The restored window size, logical px.
    int windowWidth_ = 1280;
    int windowHeight_ = 820;
    bool maximized_ = false;

    // What the platform was told last.
    std::string title_;
    Cursor cursor_ = Cursor::Arrow;
    bool textInputEnabled_ = false;
    gfx::RectI textInputRect_{};

    BuildWorker::BuildFunction build_;   // until the window takes it

    uint64_t saveTimer_ = 0;
    uint64_t deadlineTimer_ = 0;
    double deadlineMs_ = -1;
    uint64_t snapshotTimer_ = 0;
    bool quitting_ = false;
    std::shared_ptr<bool> alive_;

    // Snapshot mode.
    double firstFrameMs_ = -1;
    int exitCode_ = 0;
    std::string exitStdout_;
    std::string exitStderr_;
};

} // namespace rgui
