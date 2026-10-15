// SPDX-License-Identifier: Apache-2.0
// rayomd-gui on Linux: the startup sequence of app/app.h over the Wayland window, with the desktop
// portal (appearance, file dialogs), xdg-open, the Omarchy watcher and Hyprland's rounding wired
// into the event loop.
#include "app/app.h"
#include "app/args.h"
#include "app/version.h"
#include "platform/linux/hypr_ipc.h"
#include "platform/linux/loop.h"
#include "platform/linux/omarchy.h"
#include "platform/linux/open_external.h"
#include "platform/linux/portal.h"
#include "platform/linux/wayland_window.h"

#include <poll.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr int kPortalConnectTimeoutMs = 1000;
constexpr int kHyprTimeoutMs = 250;
// Omarchy's theme switch ends with `hyprctl reload`: ask for the rounding after it.
constexpr uint32_t kRoundingDelayMs = 500;

// SIGINT / SIGTERM: save the settings and leave the loop without prompting. The signal is read
// from a signalfd by the loop, so this runs on the UI thread.
rgui::App* g_app = nullptr;

void OnSignal(void*) {
    if (g_app) g_app->SaveSettings();
    rgui_loop_quit();
}

// A non-blocking connection the loop polls: its fd for `events()`, and `dispatch()` when the fd
// is ready or when `timeoutMs()` has passed. Events and the timer are refreshed before every poll.
class LoopSource {
public:
    LoopSource(int fd, std::function<short()> events, std::function<int()> timeoutMs, std::function<bool()> dispatch)
        : fd_(fd), events_(std::move(events)), timeoutMs_(std::move(timeoutMs)), dispatch_(std::move(dispatch)) {}
    ~LoopSource() { Stop(); }
    LoopSource(const LoopSource&) = delete;
    LoopSource& operator=(const LoopSource&) = delete;

    bool Start() {
        if (rgui_loop_add_fd(fd_, static_cast<uint32_t>(events_()), &LoopSource::OnFd, this) != 0) return false;
        if (rgui_loop_add_prepare(&LoopSource::OnPrepare, this) != 0) {
            rgui_loop_remove_fd(fd_);
            return false;
        }
        running_ = true;
        return true;
    }

    void Stop() {
        if (!running_) return;
        running_ = false;
        rgui_loop_remove_fd(fd_);
        rgui_loop_remove_prepare(&LoopSource::OnPrepare, this);
        if (timer_) rgui_loop_cancel_timer(timer_);
        timer_ = 0;
    }

private:
    void Dispatch() {
        if (!running_) return;
        if (!dispatch_()) Stop();
    }

    static void OnFd(int, uint32_t, void* user) { static_cast<LoopSource*>(user)->Dispatch(); }

    static void OnTimer(uint64_t, void* user) {
        auto* self = static_cast<LoopSource*>(user);
        self->timer_ = 0;
        self->Dispatch();
    }

    static void OnPrepare(void* user) {
        auto* self = static_cast<LoopSource*>(user);
        if (!self->running_) return;
        rgui_loop_set_fd_events(self->fd_, static_cast<uint32_t>(self->events_()));
        const int timeout = self->timeoutMs_();
        if (timeout < 0) {
            if (self->timer_) rgui_loop_cancel_timer(self->timer_);
            self->timer_ = 0;
            return;
        }
        const uint64_t deadline = rgui_loop_now_ms() + static_cast<uint64_t>(timeout);
        if (self->timer_ && deadline == self->deadline_) return;
        if (self->timer_) rgui_loop_cancel_timer(self->timer_);
        self->deadline_ = deadline;
        self->timer_ = rgui_loop_add_timer(static_cast<uint32_t>(timeout), 0, &LoopSource::OnTimer, self);
    }

    int fd_;
    std::function<short()> events_;
    std::function<int()> timeoutMs_;
    std::function<bool()> dispatch_;
    bool running_ = false;
    uint64_t timer_ = 0;
    uint64_t deadline_ = 0;
};

bool Exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

void WriteAll(FILE* stream, const std::string& text) {
    if (!text.empty()) fwrite(text.data(), 1, text.size(), stream);
    fflush(stream);
}

// Hyprland's window rounding, when we run under Hyprland.
void QueryRounding(rgui::App& app) {
    int rounding = 0;
    if (rgui_hypr_query_rounding(kHyprTimeoutMs, &rounding)) app.OnRoundingChanged(rounding);
}

uint64_t g_roundingTimer = 0;

void OnRoundingTimer(uint64_t, void*) {
    g_roundingTimer = 0;
    if (g_app) QueryRounding(*g_app);
}

} // namespace

int main(int argc, char** argv) {
    const auto processStart = std::chrono::steady_clock::now();

    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    const rgui::Args args = rgui::ParseArgs(arguments);
    if (args.mode == rgui::Args::Mode::Version) {
        printf("rayomd-gui %s\n", rgui::Version());
        return 0;
    }
    if (args.mode == rgui::Args::Mode::Usage) {
        fprintf(stderr, "%s\n", rgui::Args::kUsage);
        return rgui::Args::kUsageExitCode;
    }

    // Before any thread starts: threads inherit the blocked SIGINT / SIGTERM.
    if (rgui_loop_init() != 0 || rgui_loop_watch_signals(&OnSignal, nullptr) != 0) {
        perror("rayomd-gui: event loop");
        return 1;
    }

    rgui::Startup startup = rgui::BeginStartup(args, rgui::wl::XdgConfigDir() / "settings.ini", processStart);

    rgui::wl::WindowOptions options;
    options.width = startup.windowWidth;
    options.height = startup.windowHeight;
    options.maximized = startup.maximized;
    std::unique_ptr<rgui::wl::WaylandWindow> window = rgui::wl::WaylandWindow::Create(options);
    if (!window) return 1;

    // The desktop portal: without a session bus every request answers as if there were none.
    rgui::dbus::Portal portal;
    portal.Connect(kPortalConnectTimeoutMs);
    const rgui::omarchy::Paths omarchyPaths = rgui::omarchy::PathsFromEnvironment();

    rgui::wl::WindowHooks hooks;
    hooks.showFileDialog = [&portal](rgui::FileDialog kind, std::string suggestedPath,
                                     std::function<void(rgui::FileDialogResult)> done) {
        portal.ShowFileDialog(kind, suggestedPath, std::move(done));
    };
    hooks.openExternally = [](const std::string& path) { rgui_open_externally(path.c_str()); };
    hooks.omarchyThemeDir = [&omarchyPaths]() -> std::optional<std::filesystem::path> {
        if (Exists(omarchyPaths.themeDir)) return omarchyPaths.themeDir;
        return std::nullopt;
    };
    hooks.omarchyUserShellToml = [&omarchyPaths]() -> std::optional<std::filesystem::path> {
        if (Exists(omarchyPaths.userShellToml)) return omarchyPaths.userShellToml;
        return std::nullopt;
    };
    window->SetHooks(std::move(hooks));

    std::unique_ptr<rgui::App> app = rgui::App::Create(*window, std::move(startup));
    g_app = app.get();
    window->SetClient(app.get());

    std::unique_ptr<LoopSource> portalSource;
    if (portal.Connected()) {
        portalSource = std::make_unique<LoopSource>(
            portal.Fd(), [&portal] { return portal.PollEvents(); }, [&portal] { return portal.TimeoutMs(); },
            [&portal] { return portal.Dispatch(); });
        if (portalSource->Start()) {
            auto apply = [](rgui::dbus::Appearance appearance) {
                if (g_app) g_app->OnSystemAppearance(appearance.dark, appearance.textScale);
            };
            portal.ReadAppearance(apply);
            portal.WatchAppearance(apply);
        }
    }

    rgui::omarchy::Watcher watcher;
    std::unique_ptr<LoopSource> watcherSource;
    const bool watching = watcher.Start(
        omarchyPaths,
        [] {
            if (!g_app) return;
            g_app->OnOmarchyChanged();
            if (g_roundingTimer) rgui_loop_cancel_timer(g_roundingTimer);
            g_roundingTimer = rgui_loop_add_timer(kRoundingDelayMs, 0, &OnRoundingTimer, nullptr);
        },
        [] {
            if (g_app) g_app->OnFontsChanged();
        });
    if (watching) {
        watcherSource = std::make_unique<LoopSource>(
            watcher.Fd(), [] { return static_cast<short>(POLLIN); }, [&watcher] { return watcher.TimeoutMs(); },
            [&watcher] {
                watcher.Dispatch();
                return true;
            });
        watcherSource->Start();
    }

    QueryRounding(*app);

    const int loopStatus = rgui_loop_run();

    watcherSource.reset();
    portalSource.reset();
    if (g_roundingTimer) rgui_loop_cancel_timer(g_roundingTimer);
    g_roundingTimer = 0;

    if (window->ConnectionLost() || loopStatus != 0) {
        if (loopStatus != 0) perror("rayomd-gui: event loop");
        app->SaveSettings();
        g_app = nullptr;
        app.reset();
        return 1;
    }

    WriteAll(stdout, app->ExitStdout());
    WriteAll(stderr, app->ExitStderr());
    const int code = app->ExitCode();
    g_app = nullptr;
    app.reset();
    return code;
}
