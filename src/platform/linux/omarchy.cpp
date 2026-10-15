// SPDX-License-Identifier: Apache-2.0
// Following Omarchy (see omarchy.h).
#include "platform/linux/omarchy.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>

#include <pwd.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace rgui::omarchy {
namespace {

int64_t NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::filesystem::path HomeDir() {
    const char* home = std::getenv("HOME");
    if (home && home[0] == '/') return home;
    passwd entry;
    passwd* result = nullptr;
    char buffer[4096];
    if (getpwuid_r(getuid(), &entry, buffer, sizeof buffer, &result) == 0 && result && result->pw_dir) {
        return result->pw_dir;
    }
    return "/";
}

std::filesystem::path XdgDir(const char* variable, const char* fallback) {
    const char* value = std::getenv(variable);
    if (value && value[0] == '/') return value;
    return HomeDir() / fallback;
}

} // namespace

Paths PathsFrom(const std::filesystem::path& stateHome, const std::filesystem::path& configHome) {
    Paths p;
    p.current = stateHome / "omarchy" / "current";
    p.themeDir = p.current / "theme";
    p.configDir = configHome / "omarchy";
    p.userShellToml = p.configDir / "shell.toml";
    p.fontconfigDir = configHome / "fontconfig";
    p.fontsConf = p.fontconfigDir / "fonts.conf";
    return p;
}

Paths PathsFromEnvironment() {
    return PathsFrom(XdgDir("XDG_STATE_HOME", ".local/state"), XdgDir("XDG_CONFIG_HOME", ".config"));
}

Watcher::~Watcher() {
    if (fd_ >= 0) close(fd_);
}

bool Watcher::Start(const Paths& paths, std::function<void()> onTheme, std::function<void()> onFonts) {
    if (fd_ < 0) fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0) return false;
    paths_ = paths;
    onTheme_ = std::move(onTheme);
    onFonts_ = std::move(onFonts);
    AddWatches();
    return true;
}

void Watcher::AddWatches() {
    // Adding a watch for a directory already watched returns its old descriptor, so nothing is
    // missed while re-adding; descriptors of directories no longer at their paths go afterwards.
    std::vector<std::pair<int, Kind>> old = std::move(watches_);
    watches_.clear();
    struct Spec {
        const std::filesystem::path* path;
        uint32_t mask;
        Kind kind;
    };
    const Spec specs[] = {
        {&paths_.current, IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE, Kind::Current},
        {&paths_.themeDir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF, Kind::Theme},
        {&paths_.configDir, IN_CLOSE_WRITE | IN_MOVED_TO, Kind::Config},
        {&paths_.fontconfigDir, IN_CLOSE_WRITE | IN_MOVED_TO, Kind::Fontconfig},
    };
    for (const Spec& spec : specs) {
        // A missing directory is not watched (not on Omarchy, or between rm -rf and mv: the
        // next re-add finds it).
        int wd = inotify_add_watch(fd_, spec.path->c_str(), spec.mask | IN_ONLYDIR);
        if (wd < 0) continue;
        bool known = false;
        for (const auto& watch : watches_) known = known || watch.first == wd;  // one directory, two paths
        if (!known) watches_.emplace_back(wd, spec.kind);
    }
    for (const auto& watch : old) {
        bool kept = false;
        for (const auto& now : watches_) kept = kept || now.first == watch.first;
        if (!kept) inotify_rm_watch(fd_, watch.first);  // already gone when deleted: fine
    }
}

void Watcher::ReadEvents() {
    alignas(inotify_event) char buffer[8192];
    for (;;) {
        ssize_t n = read(fd_, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        size_t offset = 0;
        while (offset + sizeof(inotify_event) <= static_cast<size_t>(n)) {
            inotify_event event;
            std::memcpy(&event, buffer + offset, sizeof event);
            size_t size = sizeof(inotify_event) + event.len;
            if (size > static_cast<size_t>(n) - offset) return;  // the kernel never splits events
            const char* rawName = buffer + offset + sizeof(inotify_event);
            std::string_view name(rawName, event.len ? strnlen(rawName, event.len) : 0);
            offset += size;

            bool theme = false;
            bool fonts = false;
            if (event.mask & IN_Q_OVERFLOW) {
                theme = fonts = true;  // events were lost
            } else if (!(event.mask & IN_IGNORED)) {
                for (const auto& watch : watches_) {
                    if (watch.first != event.wd) continue;
                    switch (watch.second) {
                    case Kind::Current:
                        theme = name == "theme" || name == "theme.name" || name == "theme.generation";
                        break;
                    case Kind::Theme:
                        theme = true;
                        break;
                    case Kind::Config:
                        theme = name == "shell.toml";
                        break;
                    case Kind::Fontconfig:
                        fonts = name == "fonts.conf";
                        break;
                    }
                }
            }
            if (theme || fonts) {
                themePending_ = themePending_ || theme;
                fontsPending_ = fontsPending_ || fonts;
                deadline_ = NowMs() + kDebounceMs;
            }
        }
    }
}

int Watcher::TimeoutMs() const {
    if (deadline_ < 0) return -1;
    int64_t left = deadline_ - NowMs();
    return left < 0 ? 0 : static_cast<int>(left);
}

void Watcher::Dispatch() {
    if (fd_ < 0) return;
    ReadEvents();
    if (deadline_ < 0 || NowMs() < deadline_) return;
    deadline_ = -1;
    bool theme = themePending_;
    bool fonts = fontsPending_;
    themePending_ = fontsPending_ = false;
    // The theme directory may be a new one now.
    AddWatches();
    if (theme && onTheme_) onTheme_();
    if (fonts && onFonts_) onFonts_();
}

} // namespace rgui::omarchy
