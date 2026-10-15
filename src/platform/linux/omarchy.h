// SPDX-License-Identifier: Apache-2.0
// Following Omarchy: where its current theme, the user's shell.toml and fonts.conf live, and an
// inotify watcher that reports theme and font changes.
//
// `omarchy-theme-set` replaces the theme directory on every switch (rm -rf current/theme, then
// mv current/next-theme current/theme), then writes theme.generation and theme.name (omarchy
// 246f33a2, bin/omarchy-theme-set:491-504). The watcher therefore watches current/ for those
// names as well as the theme directory itself, waits 100 ms after the last event, then reports
// and watches every path again, so the new theme directory is watched.
//
// The event loop polls Fd() for POLLIN and calls Dispatch() when it is readable and when
// TimeoutMs() has passed. Callbacks run from Dispatch() only.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

namespace rgui::omarchy {

struct Paths {
    std::filesystem::path current;        // $XDG_STATE_HOME/omarchy/current
    std::filesystem::path themeDir;       // current/theme
    std::filesystem::path configDir;      // $XDG_CONFIG_HOME/omarchy
    std::filesystem::path userShellToml;  // configDir/shell.toml
    std::filesystem::path fontconfigDir;  // $XDG_CONFIG_HOME/fontconfig
    std::filesystem::path fontsConf;      // fontconfigDir/fonts.conf
};

// XDG_STATE_HOME (default ~/.local/state) and XDG_CONFIG_HOME (default ~/.config); values that
// are not absolute paths are ignored, as the XDG Base Directory specification says.
Paths PathsFromEnvironment();
Paths PathsFrom(const std::filesystem::path& stateHome, const std::filesystem::path& configHome);

class Watcher {
public:
    static constexpr int kDebounceMs = 100;

    Watcher() = default;
    ~Watcher();
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    // Starts watching. `onTheme` follows a theme switch, a change inside the theme directory or
    // the user's shell.toml; `onFonts` a change of fonts.conf. False when inotify is unavailable.
    bool Start(const Paths& paths, std::function<void()> onTheme, std::function<void()> onFonts);

    int Fd() const { return fd_; }
    // Milliseconds until the debounce ends, -1 when no change is pending.
    int TimeoutMs() const;
    // Reads the pending events and, once the debounce has ended, re-adds the watches and runs
    // the callbacks.
    void Dispatch();

private:
    enum class Kind : uint8_t { Current, Theme, Config, Fontconfig };

    void AddWatches();
    void ReadEvents();

    int fd_ = -1;
    Paths paths_;
    std::function<void()> onTheme_;
    std::function<void()> onFonts_;
    std::vector<std::pair<int, Kind>> watches_;
    bool themePending_ = false;
    bool fontsPending_ = false;
    int64_t deadline_ = -1;  // CLOCK_MONOTONIC ms
};

} // namespace rgui::omarchy
