// SPDX-License-Identifier: Apache-2.0
// Following Omarchy (the theme switch of omarchy-theme-set, shell.toml, fonts.conf), Hyprland's
// rounding over a fake IPC socket, and xdg-open.
#include "runner.h"

#include "platform/linux/hypr_ipc.h"
#include "platform/linux/omarchy.h"
#include "platform/linux/open_external.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

int64_t Ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string MakeTempDir(const char* prefix) {
    std::string pattern = std::string("/tmp/") + prefix + "-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    return mkdtemp(buffer.data()) ? std::string(buffer.data()) : std::string();
}

void WriteFile(const fs::path& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

std::string ReadFile(const fs::path& path) {
    std::string text;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return text;
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) text.append(buffer, n);
    std::fclose(f);
    return text;
}

// Drives the watcher as the event loop would: poll its fd up to its debounce deadline.
void Drive(rgui::omarchy::Watcher& watcher, const std::function<bool()>& until, int ms) {
    int64_t deadline = Ms() + ms;
    while (!until()) {
        int64_t left = deadline - Ms();
        if (left <= 0) return;
        int timeout = watcher.TimeoutMs();
        if (timeout < 0 || timeout > left) timeout = static_cast<int>(left);
        pollfd p{watcher.Fd(), POLLIN, 0};
        poll(&p, 1, timeout);
        watcher.Dispatch();
    }
}

// What bin/omarchy-theme-set does at omarchy 246f33a2 (lines 491-504): the next theme is
// prepared beside the current one, the current one is removed with rm -rf, the next one is
// moved into place, then theme.generation and theme.name are written.
void SwitchTheme(const rgui::omarchy::Paths& paths, const std::string& name, const std::string& background) {
    fs::path next = paths.current / "next-theme";
    fs::create_directories(next);
    WriteFile(next / "colors.toml", "background = \"" + background + "\"\n");
    WriteFile(next / "shell.toml", "[font]\nbase-size = 12\n");
    std::error_code error;
    fs::remove_all(paths.themeDir, error);
    fs::rename(next, paths.themeDir, error);
    WriteFile(paths.current / "theme.generation", std::to_string(Ms()) + "\n");
    WriteFile(paths.current / "theme.name", name + "\n");
}

} // namespace

RGUI_TEST("linux.omarchy_paths") {
    using rgui::omarchy::PathsFrom;
    rgui::omarchy::Paths p = PathsFrom("/s", "/c");
    CHECK_EQ(p.themeDir.string(), std::string("/s/omarchy/current/theme"));
    CHECK_EQ(p.userShellToml.string(), std::string("/c/omarchy/shell.toml"));
    CHECK_EQ(p.fontsConf.string(), std::string("/c/fontconfig/fonts.conf"));

    const char* keep[] = {"HOME", "XDG_STATE_HOME", "XDG_CONFIG_HOME"};
    std::string saved[3];
    bool had[3];
    for (int i = 0; i < 3; ++i) {
        const char* v = std::getenv(keep[i]);
        had[i] = v != nullptr;
        saved[i] = v ? v : "";
    }
    setenv("HOME", "/home/u", 1);
    unsetenv("XDG_STATE_HOME");
    setenv("XDG_CONFIG_HOME", "relative/ignored", 1);
    p = rgui::omarchy::PathsFromEnvironment();
    CHECK_EQ(p.themeDir.string(), std::string("/home/u/.local/state/omarchy/current/theme"));
    CHECK_EQ(p.fontsConf.string(), std::string("/home/u/.config/fontconfig/fonts.conf"));
    setenv("XDG_STATE_HOME", "/xs", 1);
    setenv("XDG_CONFIG_HOME", "/xc", 1);
    p = rgui::omarchy::PathsFromEnvironment();
    CHECK_EQ(p.themeDir.string(), std::string("/xs/omarchy/current/theme"));
    CHECK_EQ(p.userShellToml.string(), std::string("/xc/omarchy/shell.toml"));
    for (int i = 0; i < 3; ++i) {
        if (had[i]) {
            setenv(keep[i], saved[i].c_str(), 1);
        } else {
            unsetenv(keep[i]);
        }
    }
}

RGUI_TEST("linux.omarchy_watch") {
    std::string root = MakeTempDir("rgui-omarchy");
    CHECK(!root.empty());
    if (root.empty()) return;
    rgui::omarchy::Paths paths = rgui::omarchy::PathsFrom(fs::path(root) / "state", fs::path(root) / "config");
    fs::create_directories(paths.themeDir);
    WriteFile(paths.themeDir / "colors.toml", "background = \"#1a1b26\"\n");
    fs::create_directories(paths.configDir);
    fs::create_directories(paths.fontconfigDir);

    int themes = 0;
    int fonts = 0;
    std::string seenBackground;
    rgui::omarchy::Watcher watcher;
    CHECK(watcher.Start(
        paths,
        [&] {
            ++themes;
            seenBackground = ReadFile(paths.themeDir / "colors.toml");
        },
        [&] { ++fonts; }));
    CHECK_EQ(watcher.TimeoutMs(), -1);

    // A switch fires once (the events debounced), within a second, and the new files are there.
    int64_t start = Ms();
    SwitchTheme(paths, "Retro 82", "#123456");
    Drive(watcher, [&] { return themes >= 1; }, 1000);
    CHECK_EQ(themes, 1);
    CHECK(Ms() - start < 1000);
    CHECK(seenBackground.find("#123456") != std::string::npos);
    Drive(watcher, [] { return false; }, 300);
    CHECK_EQ(themes, 1);
    CHECK_EQ(watcher.TimeoutMs(), -1);

    // A second switch fires again: the replaced directory did not end the following.
    start = Ms();
    SwitchTheme(paths, "Tokyo Night", "#654321");
    Drive(watcher, [&] { return themes >= 2; }, 1000);
    CHECK_EQ(themes, 2);
    CHECK(Ms() - start < 1000);
    CHECK(seenBackground.find("#654321") != std::string::npos);

    // An edit inside the new theme directory is seen: it is the one watched now.
    WriteFile(paths.themeDir / "colors.toml", "background = \"#abcdef\"\n");
    Drive(watcher, [&] { return themes >= 3; }, 1000);
    CHECK_EQ(themes, 3);

    // Other files in current/ (the background link, logs) are not a theme change.
    WriteFile(paths.current / "background", "x");
    WriteFile(paths.current / "theme-set.log", "x");
    Drive(watcher, [] { return false; }, 300);
    CHECK_EQ(themes, 3);

    // The user's shell.toml is part of the theme; fonts.conf (written by a rename, as editors
    // and omarchy-font-set's tools may) is the fonts only; other files are neither.
    WriteFile(paths.userShellToml, "[font]\nbase-size = 15\n");
    Drive(watcher, [&] { return themes >= 4; }, 1000);
    CHECK_EQ(themes, 4);
    CHECK_EQ(fonts, 0);
    WriteFile(paths.fontconfigDir / "fonts.conf.tmp", "<fontconfig/>");
    std::error_code error;
    fs::rename(paths.fontconfigDir / "fonts.conf.tmp", paths.fontsConf, error);
    Drive(watcher, [&] { return fonts >= 1; }, 1000);
    CHECK_EQ(fonts, 1);
    CHECK_EQ(themes, 4);
    WriteFile(paths.fontconfigDir / "other.conf", "x");
    WriteFile(paths.configDir / "other.toml", "x");
    Drive(watcher, [] { return false; }, 300);
    CHECK_EQ(fonts, 1);
    CHECK_EQ(themes, 4);

    fs::remove_all(root, error);
}

namespace {

// A Hyprland IPC socket that answers one connection with `reply` (or holds it open for
// `holdMs` without answering).
struct FakeHyprland {
    std::string path;
    int listener = -1;
    std::string request;
    std::thread thread;

    bool Start(const std::string& socketPath, std::string reply, int holdMs = 0) {
        path = socketPath;
        listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, path.c_str(), sizeof sa.sun_path - 1);
        if (bind(listener, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || listen(listener, 1) != 0) return false;
        timeval limit{5, 0};
        setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
        thread = std::thread([this, reply, holdMs] {
            int fd = accept(listener, nullptr, nullptr);
            if (fd < 0) return;
            char buffer[256];
            ssize_t n = recv(fd, buffer, sizeof buffer, 0);  // Hyprland reads the request once
            if (n > 0) request.assign(buffer, static_cast<size_t>(n));
            if (holdMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
            } else {
                size_t sent = 0;
                while (sent < reply.size()) {
                    ssize_t w = send(fd, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
                    if (w <= 0) break;
                    sent += static_cast<size_t>(w);
                }
            }
            close(fd);
        });
        return true;
    }

    ~FakeHyprland() {
        if (thread.joinable()) thread.join();
        if (listener >= 0) close(listener);
        unlink(path.c_str());
    }
};

// The loop's use of the query, in one blocking call: poll, step, until done or the deadline.
bool QueryRounding(const std::string& socketPath, int timeoutMs, int* rounding) {
    rgui_hypr_query* q = rgui_hypr_query_start(socketPath.c_str(), timeoutMs);
    if (!q) return false;
    int status = RGUI_HYPR_PENDING;
    while (status == RGUI_HYPR_PENDING) {
        pollfd p{rgui_hypr_query_fd(q), rgui_hypr_query_events(q), 0};
        poll(&p, 1, rgui_hypr_query_timeout_ms(q));
        status = rgui_hypr_query_step(q, rounding);
    }
    rgui_hypr_query_free(q);
    return status == RGUI_HYPR_DONE;
}

} // namespace

RGUI_TEST("linux.hypr_ipc") {
    int r = -1;
    CHECK(rgui_hypr_parse_rounding("{\"int\":12}", 10, &r));
    CHECK_EQ(r, 12);
    r = 5;
    CHECK(!rgui_hypr_parse_rounding("{\"int\": -1}", 11, &r));
    CHECK(!rgui_hypr_parse_rounding("{\"int\": 1.5}", 12, &r));
    // Clamped to 0..64: a theme's rounding is the radius of cards and popups.
    CHECK(rgui_hypr_parse_rounding("{\"int\": 99999999999}", 20, &r));
    CHECK_EQ(r, 64);
    CHECK(rgui_hypr_parse_rounding("{\"int\": 65}", 11, &r));
    CHECK_EQ(r, 64);
    CHECK(rgui_hypr_parse_rounding("{\"int\": 64}", 11, &r));
    CHECK_EQ(r, 64);
    r = 5;
    CHECK(!rgui_hypr_parse_rounding("{\"float\": 2.0}", 14, &r));
    CHECK(!rgui_hypr_parse_rounding("{\"int\":", 7, &r));
    CHECK_EQ(r, 5);

    std::string dir = MakeTempDir("rgui-hypr");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    std::string socketPath = dir + "/.socket.sock";
    {
        // The JSON Hyprland answers `j/getoption decoration:rounding` with.
        FakeHyprland server;
        CHECK(server.Start(socketPath, "{\n    \"option\": \"decoration:rounding\",\n    \"int\": 8,\n    \"set\": true\n}"));
        r = -1;
        CHECK(QueryRounding(socketPath, 1000, &r));
        CHECK_EQ(r, 8);
        server.thread.join();
        CHECK_EQ(server.request, std::string("j/getoption decoration:rounding"));
    }
    {
        // No integer in the answer (an unknown option): the value stays.
        FakeHyprland server;
        CHECK(server.Start(socketPath, "no such option"));
        r = 3;
        CHECK(!QueryRounding(socketPath, 1000, &r));
        CHECK_EQ(r, 3);
    }
    {
        // A server that never answers: the wait is bounded.
        FakeHyprland server;
        CHECK(server.Start(socketPath, "", 800));
        int64_t start = Ms();
        CHECK(!QueryRounding(socketPath, 150, &r));
        int64_t waited = Ms() - start;
        CHECK(waited >= 100 && waited < 600);
        CHECK_EQ(r, 3);
    }
    {
        // The UI thread never waits on Hyprland: a step returns at once while no answer came.
        FakeHyprland server;
        CHECK(server.Start(socketPath, "", 400));
        rgui_hypr_query* q = rgui_hypr_query_start(socketPath.c_str(), 2000);
        CHECK(q != nullptr);
        if (q) {
            int64_t start = Ms();
            int status = RGUI_HYPR_PENDING;
            for (int i = 0; i < 5 && status == RGUI_HYPR_PENDING; ++i) status = rgui_hypr_query_step(q, &r);
            CHECK_EQ(status, int(RGUI_HYPR_PENDING));
            CHECK(Ms() - start < 50);
            CHECK_EQ(rgui_hypr_query_events(q), short(POLLIN));
            CHECK(rgui_hypr_query_timeout_ms(q) > 1000);
            rgui_hypr_query_free(q);
        }
    }
    {
        // An answer over 64 KiB is refused, even with a rounding at its end.
        FakeHyprland server;
        CHECK(server.Start(socketPath, std::string(70000, ' ') + "\"int\": 4"));
        CHECK(!QueryRounding(socketPath, 2000, &r));
        CHECK_EQ(r, 3);
    }
    // No socket at all.
    CHECK(!QueryRounding(dir + "/missing.sock", 200, &r));
    CHECK_EQ(r, 3);

    // From the environment: $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock, and
    // nothing at all outside Hyprland.
    const char* savedSig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* savedRuntime = std::getenv("XDG_RUNTIME_DIR");
    std::string keepSig = savedSig ? savedSig : "";
    std::string keepRuntime = savedRuntime ? savedRuntime : "";
    fs::create_directories(fs::path(dir) / "hypr" / "abc_123");
    {
        FakeHyprland server;
        CHECK(server.Start(dir + "/hypr/abc_123/.socket.sock", "{\"option\": \"decoration:rounding\", \"int\": 10}"));
        setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
        setenv("HYPRLAND_INSTANCE_SIGNATURE", "abc_123", 1);
        char path[108];
        CHECK(rgui_hypr_socket_path(path, sizeof path));
        CHECK_EQ(std::string(path), dir + "/hypr/abc_123/.socket.sock");
        CHECK(QueryRounding(path, 1000, &r));
        CHECK_EQ(r, 10);
    }
    char path[108];
    setenv("HYPRLAND_INSTANCE_SIGNATURE", "a/b", 1);
    CHECK(!rgui_hypr_socket_path(path, sizeof path));
    unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    CHECK(!rgui_hypr_socket_path(path, sizeof path));
    if (savedSig) setenv("HYPRLAND_INSTANCE_SIGNATURE", keepSig.c_str(), 1);
    if (savedRuntime) {
        setenv("XDG_RUNTIME_DIR", keepRuntime.c_str(), 1);
    } else {
        unsetenv("XDG_RUNTIME_DIR");
    }
    std::error_code error;
    fs::remove_all(dir, error);
}

RGUI_TEST("linux.open_external") {
    // A stand-in xdg-open on PATH records its argument and its blocked and ignored signals.
    std::string dir = MakeTempDir("rgui-open");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    std::string out = dir + "/out.txt";
    std::string script = dir + "/xdg-open";
    // A descriptor without close-on-exec, as the engine's fopen leaves them: it must not leak.
    const int devNull = open("/dev/null", O_RDONLY);
    const int leaky = fcntl(devNull, F_DUPFD, 200);
    close(devNull);
    CHECK(leaky >= 200);
    const std::string leakyPath = "/proc/$$/fd/" + std::to_string(leaky);
    WriteFile(script, "#!/bin/sh\n"
                      "{ printf '%s\\n' \"$1\"; if [ -e " + leakyPath + " ]; then echo leaked; else echo closed; fi\n"
                      "while read -r k v; do\n"
                      "  case \"$k\" in SigBlk:|SigIgn:) printf '%s %s\\n' \"$k\" \"$v\";; esac\n"
                      "done < /proc/$$/status; } > \"" + out + ".tmp\" && mv \"" + out + ".tmp\" \"" + out + "\"\n");
    chmod(script.c_str(), 0755);
    const char* savedPath = std::getenv("PATH");
    std::string keepPath = savedPath ? savedPath : "/usr/bin:/bin";
    setenv("PATH", (dir + ":" + keepPath).c_str(), 1);

    // The app's loop blocks SIGINT/SIGTERM (signalfd) and ignores SIGPIPE; xdg-open must not
    // inherit that.
    sigset_t block;
    sigset_t oldMask;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    pthread_sigmask(SIG_BLOCK, &block, &oldMask);
    struct sigaction ignore {};
    struct sigaction oldPipe {};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ignore, &oldPipe);

    const std::string target = "/tmp/My Notes/-odd name.pdf";
    pid_t pid = rgui_open_externally(target.c_str());

    pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
    sigaction(SIGPIPE, &oldPipe, nullptr);
    CHECK(pid > 0);
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    close(leaky);
    std::string text = ReadFile(out);
    // xdg-open gets a file:// URI: a name starting with '-' is no option, spaces are encoded.
    CHECK_EQ(text.substr(0, text.find('\n')), std::string("file:///tmp/My%20Notes/-odd%20name.pdf"));
    CHECK(text.find("\nclosed\n") != std::string::npos);
    CHECK(text.find("SigBlk: 0000000000000000") != std::string::npos);
    size_t ign = text.find("SigIgn: ");
    CHECK(ign != std::string::npos);
    if (ign != std::string::npos) {
        unsigned long long mask = std::strtoull(text.c_str() + ign + 8, nullptr, 16);
        CHECK((mask & (1ull << (SIGPIPE - 1))) == 0);
    }

    // The URI: absolute against the working directory, everything but unreserved bytes encoded.
    auto uri = [](const char* path) {
        char* u = rgui_file_uri(path);
        std::string result = u ? u : "(null)";
        std::free(u);
        return result;
    };
    CHECK_EQ(uri("/a b/%#?:;@&=+$,\xc3\xa9.pdf"), std::string("file:///a%20b/%25%23%3F%3A%3B%40%26%3D%2B%24%2C%C3%A9.pdf"));
    CHECK_EQ(uri("/x/A-z_0.9~"), std::string("file:///x/A-z_0.9~"));
    char* cwd = getcwd(nullptr, 0);
    CHECK(cwd != nullptr);
    if (cwd) {
        CHECK_EQ(uri("mailto:x.pdf"), std::string("file://") + (std::string(cwd) == "/" ? "" : cwd) + "/mailto%3Ax.pdf");
        CHECK_EQ(uri("-x.pdf"), std::string("file://") + (std::string(cwd) == "/" ? "" : cwd) + "/-x.pdf");
        std::free(cwd);
    }
    CHECK_EQ(uri(""), std::string("(null)"));

    // Without xdg-open on PATH: -1 at once.
    std::string empty = dir + "/empty";
    fs::create_directories(empty);
    setenv("PATH", empty.c_str(), 1);
    CHECK_EQ(rgui_open_externally("/tmp/x.pdf"), pid_t(-1));
    setenv("PATH", keepPath.c_str(), 1);
    std::error_code error;
    fs::remove_all(dir, error);
}
