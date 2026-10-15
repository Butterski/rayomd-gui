// SPDX-License-Identifier: Apache-2.0
// One app on the headless platform, for the app tests and the app benchmarks: its settings and
// files in a directory of its own, frames run as the app requests them.
#pragma once

#include "headless_platform.h"
#include "runner.h"

#include "app/app.h"
#include "common/file_io.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace rgui::test {

// An empty directory <build>/<group>/<name>.
inline std::filesystem::path FreshDir(const char* group, const char* name) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::u8path(BuildDir()) / group / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir, error);
    return dir;
}

inline std::string TesterPath() { return std::string(SourceDir()) + "/external/rayomd/tester.md"; }

// Whether the interface font the app tests draw with is installed.
inline bool HaveFonts() {
    static const bool have = [] {
        text::FontIndex index;
        index.SetDirectories({"/usr/share/fonts"});
        return index.Find("DejaVu Sans Mono", 400, false) != nullptr;
    }();
    return have;
}

struct AppHarness {
    static constexpr int kWidth = 1280;
    static constexpr int kHeight = 820;

    HeadlessPlatform platform;
    std::unique_ptr<App> app;
    std::filesystem::path dir;
    int framesSeen = 0;

    explicit AppHarness(const char* name, const char* group = "test-app-window") : dir(FreshDir(group, name)) {}

    std::filesystem::path SettingsPath() const { return dir / "settings.ini"; }

    // `settings` is the settings file's text (none when empty).
    void Start(const std::string& settings, const std::string& file = {}, const std::string& snapshotPath = {}) {
        if (!settings.empty()) RayoMd::File::WriteAll(SettingsPath(), settings);
        Startup startup;
        startup.args.mode = snapshotPath.empty() ? Args::Mode::Run : Args::Mode::Snapshot;
        startup.args.snapshotPath = snapshotPath;
        startup.args.file = file;
        startup.settingsPath = SettingsPath();
        startup.settings.Load(startup.settingsPath);
        startup.fontIndex = std::make_unique<text::FontIndex>();
        startup.fontIndex->SetDirectories({"/usr/share/fonts"});
        startup.fontIndex->Start();
        startup.fontPaths = {};
        app = App::Create(platform, std::move(startup));
        app->OnResize(kWidth, kHeight, 1.0);
    }

    MainWindow& Window() { return app->Window(); }

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
    // Until the preview shows the newest text and every visible page its current image.
    bool Settle(int timeoutMs = 30000) {
        return RunUntil(
            [&] { return Window().PreviewSettled() && Window().ShownGeneration() == Window().RequestedGeneration(); },
            timeoutMs);
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

} // namespace rgui::test
