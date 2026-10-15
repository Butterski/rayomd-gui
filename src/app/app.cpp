// SPDX-License-Identifier: Apache-2.0
#include "app/app.h"

#include "app/themeselect.h"
#include "common/exact_math.h"
#include "gfx/png_write.h"

#include <algorithm>
#include <cstdio>

namespace rgui {

namespace {

namespace fs = std::filesystem;

uint32_t Opaque(ui::Rgba color) { return gfx::PackArgb(255, color.r, color.g, color.b); }

int LogicalSize(int devicePixels, double scale) {
    return static_cast<int>(RayoMd::ExactMath::LlRound(devicePixels / (scale > 0 ? scale : 1.0)));
}

} // namespace

Startup BeginStartup(const Args& args, std::filesystem::path settingsPath,
                     std::chrono::steady_clock::time_point processStart) {
    Startup startup;
    startup.args = args;
    startup.processStart = processStart;
    startup.fontIndex = std::make_unique<text::FontIndex>();
    startup.fontIndex->Start();
    startup.fontPaths = text::FontChoicePaths::Default();
    startup.settings.Load(settingsPath);
    startup.settingsPath = std::move(settingsPath);
    startup.windowWidth = std::max(App::kMinWidth, startup.settings.GetInt("ui.windowWidth", 1280));
    startup.windowHeight = std::max(App::kMinHeight, startup.settings.GetInt("ui.windowHeight", 820));
    startup.maximized = startup.settings.GetBool("ui.maximized", false);
    return startup;
}

App::App(Platform& platform, Startup startup)
    : platform_(platform), args_(std::move(startup.args)), settingsPath_(std::move(startup.settingsPath)),
      processStart_(startup.processStart), fontIndex_(std::move(startup.fontIndex)),
      fontPaths_(std::move(startup.fontPaths)), settings_(std::move(startup.settings)),
      snapshot_(args_.mode == Args::Mode::Snapshot), windowWidth_(std::max(kMinWidth, startup.windowWidth)),
      windowHeight_(std::max(kMinHeight, startup.windowHeight)), maximized_(startup.maximized),
      alive_(std::make_shared<bool>(true)) {
    if (!fontIndex_) {
        fontIndex_ = std::make_unique<text::FontIndex>();
        fontIndex_->Start();
    }
    ctx_.SetTokens(&tokens_);
    ctx_.SetDoubleClickTime(startup.doubleClickMs);
    ctx_.SetClipboard([this](std::string text) { platform_.SetClipboardText(std::move(text)); },
                      [this](std::function<void(std::string)> done) { platform_.RequestClipboardText(std::move(done)); });
    ctx_.SetRequestFrame([this] { platform_.RequestFrame(); });
}

std::unique_ptr<App> App::Create(Platform& platform, Startup startup) {
    std::unique_ptr<App> app(new App(platform, std::move(startup)));
    App& a = *app;
    a.LoadFonts();
    MainWindow::Hooks hooks;
    hooks.requestFrame = [&a] { a.platform_.RequestFrame(); };
    hooks.settingsChanged = [&a] { a.SettingsChanged(); };
    hooks.themeChanged = [&a] { a.WearTheme(); };
    hooks.quit = [&a] { a.Quit(); };
    a.window_ = std::make_unique<MainWindow>(platform, a.fontIndex_.get(), std::move(hooks));
    a.window_->LoadSettings(a.settings_);
    a.WearTheme();
    if (a.args_.file.empty() || !a.window_->Open(a.args_.file)) a.window_->PreviewNow();
    if (a.snapshot_) {
        a.snapshotTimer_ = platform.StartTimer(kSnapshotTimeoutMs, [alive = a.alive_, &a] {
            if (!*alive) return;
            a.snapshotTimer_ = 0;
            a.exitCode_ = 1;
            a.exitStderr_ = "snapshot: timed out\n";
            a.Quit();
        });
    }
    platform.RequestFrame();
    return app;
}

App::~App() {
    *alive_ = false;
    for (uint64_t timer : {saveTimer_, deadlineTimer_, snapshotTimer_}) {
        if (timer != 0) platform_.CancelTimer(timer);
    }
    window_.reset();
}

// ---- Fonts and theme ----

void App::LoadFonts() {
    auto fonts = std::make_unique<text::FontSet>();
    if (!fonts->Load(*fontIndex_, fontPaths_)) std::fprintf(stderr, "rayomd-gui: no usable font was found\n");
    // Nothing may keep glyphs of the faces about to go.
    glyphs_.Clear();
    compositor_.InvalidateAll();
    ctx_.SetFonts(fonts.get());
    fonts_ = std::move(fonts);
    if (window_) window_->Editor().InvalidateLayout();
    UpdateNotice();
}

void App::WearTheme() {
    ThemeRequest request;
    request.setting = window_ ? window_->ThemeSetting() : std::string(kThemeOmarchy);
    request.omarchyThemeDir = platform_.OmarchyThemeDir();
    request.userShellToml = platform_.OmarchyUserShellToml();
    request.systemDark = systemDark_;
    request.systemTextScale = systemTextScale_;
    request.hyprlandRounding = hyprlandRounding_;
    LoadedTheme theme = LoadTheme(request);
    palette_ = std::move(theme.palette);
    tokens_ = std::move(theme.tokens);
    themeSource_ = std::move(theme.source);
    themeWarning_ = std::move(theme.warning);
    UpdateNotice();
    platform_.SetWindowColors(tokens_.dark, Opaque(tokens_.colors.windowBackground), Opaque(tokens_.colors.text),
                              Opaque(tokens_.colors.accent));
    platform_.RequestFrame();
}

void App::UpdateNotice() {
    if (!window_) return;
    std::string notice = fonts_ ? fonts_->Status() : std::string();
    if (!themeWarning_.empty()) notice += (notice.empty() ? "" : "; ") + themeWarning_;
    window_->SetNotice(std::move(notice));
}

// ---- Settings and quitting ----

void App::SettingsChanged() {
    if (snapshot_) return;
    if (saveTimer_ != 0) platform_.CancelTimer(saveTimer_);
    saveTimer_ = platform_.StartTimer(kSettingsDelayMs, [alive = alive_, this] {
        if (!*alive) return;
        saveTimer_ = 0;
        SaveSettings();
    });
}

void App::SaveSettings() {
    if (snapshot_) return;
    if (saveTimer_ != 0) {
        platform_.CancelTimer(saveTimer_);
        saveTimer_ = 0;
    }
    settings_.SetInt("ui.windowWidth", windowWidth_);
    settings_.SetInt("ui.windowHeight", windowHeight_);
    settings_.SetBool("ui.maximized", maximized_);
    window_->StoreSettings(settings_);
    if (!settings_.Save(settingsPath_)) {
        std::fprintf(stderr, "rayomd-gui: cannot save %s\n", settingsPath_.u8string().c_str());
    }
}

void App::Quit() {
    if (quitting_) return;
    quitting_ = true;
    SaveSettings();
    platform_.Quit();
}

// ---- PlatformClient ----

void App::OnInput(const InputEvent& event) {
    ctx_.AddEvent(event, platform_.NowMs());
    platform_.RequestFrame();
}

void App::OnResize(int width, int height, double scale) {
    width_ = std::max(0, width);
    height_ = std::max(0, height);
    scale_ = scale > 0 ? scale : 1.0;
    pixels_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
    presented_ = false;
    const bool maximized = platform_.IsMaximized();
    bool changed = maximized != maximized_;
    maximized_ = maximized;
    if (!maximized && width_ > 0 && height_ > 0) {
        const int w = std::max(kMinWidth, LogicalSize(width_, scale_));
        const int h = std::max(kMinHeight, LogicalSize(height_, scale_));
        changed = changed || w != windowWidth_ || h != windowHeight_;
        windowWidth_ = w;
        windowHeight_ = h;
    }
    if (changed) SettingsChanged();
    platform_.RequestFrame();
}

void App::OnCloseRequest() {
    if (snapshot_) {
        exitCode_ = 1;
        exitStderr_ = "snapshot: the window was closed\n";
        Quit();
        return;
    }
    window_->RequestClose();
}

void App::OnDropFile(std::string path) { window_->DropFile(path); }

void App::OnSystemAppearance(bool dark, double textScale) {
    systemDark_ = dark;
    systemTextScale_ = textScale;
    WearTheme();
}

void App::OnOmarchyChanged() {
    window_->SetOmarchyAvailable(platform_.OmarchyThemeDir().has_value());
    WearTheme();
}

void App::OnFontsChanged() {
    LoadFonts();
    platform_.RequestFrame();
}

void App::OnRoundingChanged(int rounding) {
    hyprlandRounding_ = rounding;
    WearTheme();
}

gfx::Surface App::Framebuffer() { return {pixels_.data(), width_, height_, width_}; }

void App::OnFrame() {
    if (width_ <= 0 || height_ <= 0 || quitting_) return;
    ctx_.BeginFrame(width_, height_, scale_, platform_.NowMs());
    window_->Frame(ctx_);
    const ui::FrameOutput& out = ctx_.EndFrame();
    glyphs_.BeginFrame();
    const gfx::Surface frame = Framebuffer();
    const std::vector<gfx::RectI>& damage =
        compositor_.Render(*out.displayList, tokens_.colors.windowBackground.Premultiplied(), frame, glyphs_, icons_);
    if (!damage.empty() || !presented_) {
        platform_.Present(frame, damage);
        presented_ = true;
    }
    if (out.title != title_) {
        title_ = out.title;
        platform_.SetTitle(title_);
    }
    if (out.cursor != cursor_) {
        cursor_ = out.cursor;
        platform_.SetCursor(cursor_);
    }
    if (out.textInputEnabled != textInputEnabled_ || out.textInputRect != textInputRect_) {
        textInputEnabled_ = out.textInputEnabled;
        textInputRect_ = out.textInputRect;
        platform_.SetTextInputRect(textInputRect_, textInputEnabled_);
    }
    if (out.wantsFrame) platform_.RequestFrame();
    if (out.nextDeadlineMs >= 0) ScheduleDeadline(out.nextDeadlineMs);
    if (snapshot_) SnapshotFrame();
}

void App::ScheduleDeadline(double deadlineMs) {
    if (deadlineTimer_ != 0) {
        if (deadlineMs_ <= deadlineMs) return;
        platform_.CancelTimer(deadlineTimer_);
    }
    deadlineMs_ = deadlineMs;
    const double wait = deadlineMs - platform_.NowMs();
    const int ms = wait < 1 ? 1 : static_cast<int>(RayoMd::ExactMath::Ceil(wait));
    deadlineTimer_ = platform_.StartTimer(ms, [alive = alive_, this] {
        if (!*alive) return;
        deadlineTimer_ = 0;
        deadlineMs_ = -1;
        platform_.RequestFrame();
    });
}

double App::SinceStartMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - processStart_).count();
}

void App::SnapshotFrame() {
    if (firstFrameMs_ < 0) firstFrameMs_ = SinceStartMs();
    if (!window_->PreviewSettled()) return;
    const double readyMs = SinceStartMs();
    if (!gfx::WritePng(fs::u8path(args_.snapshotPath), Framebuffer())) {
        exitCode_ = 1;
        exitStderr_ = "snapshot: cannot write " + args_.snapshotPath + "\n";
    } else {
        char line[128];
        std::snprintf(line, sizeof line, "snapshot: first frame %.0f ms, preview ready %.0f ms\n", firstFrameMs_, readyMs);
        exitCode_ = 0;
        exitStdout_ = line;
    }
    Quit();
}

} // namespace rgui
