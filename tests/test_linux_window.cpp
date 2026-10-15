// SPDX-License-Identifier: Apache-2.0
// The parts of the Wayland window that need no compositor: the path of a dropped text/uri-list,
// the XDG directories, the limits of clipboard and drop transfers, the framebuffer size caps and
// the keymap fd check.
#include "platform/linux/data_transfer.h"
#include "platform/linux/loop.h"
#include "platform/linux/wayland_window.h"
#include "runner.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

// Sets an environment variable for the scope, restoring the old value after.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* old = getenv(name);
        hadOld_ = old != nullptr;
        if (old) old_ = old;
        if (value) setenv(name, value, 1);
        else unsetenv(name);
    }
    ~ScopedEnv() {
        if (hadOld_) setenv(name_, old_.c_str(), 1);
        else unsetenv(name_);
    }

private:
    const char* name_;
    bool hadOld_ = false;
    std::string old_;
};

} // namespace

RGUI_TEST("linux.window.uri_list") {
    using rgui::wl::PathFromUriList;
    CHECK_EQ(PathFromUriList("file:///home/u/My%20Doc.md\r\n"), std::string("/home/u/My Doc.md"));
    CHECK_EQ(PathFromUriList("# a comment\r\nhttps://example.com/x.md\r\nfile:///a.md\r\nfile:///b.md\r\n"),
             std::string("/a.md"));
    CHECK_EQ(PathFromUriList("file://localhost/tmp/x.md"), std::string("/tmp/x.md"));
    CHECK_EQ(PathFromUriList("file://no-such-host.invalid/tmp/x.md"), std::string());
    CHECK_EQ(PathFromUriList("file:///%E4%B8%AD.md\n"), std::string("/\xe4\xb8\xad.md"));
    CHECK_EQ(PathFromUriList("file:///a%zz%4"), std::string("/a%zz%4"));  // not escapes: kept
    CHECK_EQ(PathFromUriList("file:///a%00b\nfile:///c"), std::string("/c"));  // a NUL refuses the URI
    CHECK_EQ(PathFromUriList("file://"), std::string());
    CHECK_EQ(PathFromUriList(""), std::string());
}

RGUI_TEST("linux.window.xdg_dirs") {
    const std::filesystem::path root = std::filesystem::path(rgui::test::BuildDir()) / "test-xdg";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "config", ec);
    ScopedEnv home("HOME", (root / "home").c_str());
    ScopedEnv cache("XDG_CACHE_HOME", nullptr);
    {
        ScopedEnv config("XDG_CONFIG_HOME", "relative/ignored");
        CHECK(rgui::wl::XdgConfigDir() == root / "home" / ".config" / "rayomd-gui");
        CHECK(rgui::wl::XdgImageCacheDir() == root / "home" / ".cache" / "rayomd" / "images");
        CHECK(rgui::wl::XdgDocumentsDir() == root / "home" / "Documents");
    }
    ScopedEnv config("XDG_CONFIG_HOME", (root / "config").c_str());
    CHECK(rgui::wl::XdgConfigDir() == root / "config" / "rayomd-gui");
    const auto write = [&](const char* text) {
        FILE* f = fopen((root / "config" / "user-dirs.dirs").c_str(), "wb");
        CHECK(f != nullptr);
        if (!f) return;
        fputs(text, f);
        fclose(f);
    };
    write("# xdg-user-dirs\nXDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_DOCUMENTS_DIR=\"$HOME/Dokumenty\"\n");
    CHECK(rgui::wl::XdgDocumentsDir() == root / "home" / "Dokumenty");
    write("XDG_DOCUMENTS_DIR=\"/srv/docs\"\n");
    CHECK(rgui::wl::XdgDocumentsDir() == std::filesystem::path("/srv/docs"));
    write("XDG_DOCUMENTS_DIR=\"unterminated\n");
    CHECK(rgui::wl::XdgDocumentsDir() == root / "home" / "Documents");
    std::filesystem::remove_all(root, ec);
}

namespace {

void QuitLoop(uint64_t, void*) { rgui_loop_quit(); }

// Runs the event loop for `ms`.
void RunLoop(uint32_t ms) {
    rgui_loop_add_timer(ms, 0, QuitLoop, nullptr);
    rgui_loop_run();
}

// What a reader gets from a non-blocking pipe end now: "eof", "data" or "wait".
std::string Peek(int fd) {
    char c;
    const ssize_t n = read(fd, &c, 1);
    if (n == 0) return "eof";
    if (n > 0) return "data";
    return "wait";
}

// Reads a non-blocking pipe end until EOF (or `ms` of loop time without it).
bool DrainsToEof(int fd) {
    char buffer[65536];
    for (int i = 0; i < 1000; ++i) {
        const ssize_t n = read(fd, buffer, sizeof buffer);
        if (n == 0) return true;
        if (n < 0) return false;
    }
    return false;
}

} // namespace

RGUI_TEST("linux.window.transfer_limits") {
    // Another client asking for our selection again and again, never reading: at most 8 sends at
    // once (the rest closed at once), none into a regular file, and stalled ones end after the
    // idle timeout, so their descriptors come back.
    CHECK_EQ(rgui_loop_init(), 0);
    auto text = std::make_shared<const std::string>(std::string(1u << 20, 'x'));
    std::vector<int> readers;
    {
        rgui::wl::Transfers transfers(200);
        for (int i = 0; i < 10; ++i) {
            int fds[2];
            CHECK_EQ(pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
            readers.push_back(fds[0]);
            const bool accepted = transfers.Send(fds[1], text);
            CHECK_EQ(accepted, i < 8);
        }
        CHECK_EQ(transfers.Outgoing(), size_t(8));
        // The refused ones were closed at once: their readers see the end.
        CHECK_EQ(Peek(readers[8]), std::string("eof"));
        CHECK_EQ(Peek(readers[9]), std::string("eof"));

        // A regular file would block write(): refused, whatever the count.
        FILE* file = std::tmpfile();
        CHECK(file != nullptr);
        if (file) {
            const int fd = dup(fileno(file));
            CHECK(!transfers.Send(fd, text));
            std::fclose(file);
        }

        const uint64_t start = rgui_loop_now_ms();
        RunLoop(150);
        CHECK_EQ(transfers.Outgoing(), size_t(8));  // the pipes are full, nobody reads: not yet
        RunLoop(250);
        CHECK_EQ(transfers.Outgoing(), size_t(0));  // idle for 200 ms: given up
        CHECK(rgui_loop_now_ms() - start < 1000);
        for (int i = 0; i < 8; ++i) CHECK(DrainsToEof(readers[static_cast<size_t>(i)]));

        // A reader that reads gets everything, however long it takes.
        int fds[2];
        CHECK_EQ(pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
        CHECK(transfers.Send(fds[1], text));
        std::string got;
        char buffer[65536];
        for (int i = 0; i < 400 && transfers.Outgoing() > 0; ++i) {
            RunLoop(5);
            ssize_t n;
            while ((n = read(fds[0], buffer, sizeof buffer)) > 0) got.append(buffer, static_cast<size_t>(n));
        }
        ssize_t n;
        while ((n = read(fds[0], buffer, sizeof buffer)) > 0) got.append(buffer, static_cast<size_t>(n));
        CHECK_EQ(got.size(), text->size());
        close(fds[0]);
    }
    for (int fd : readers) close(fd);
}

RGUI_TEST("linux.window.transfer_receive") {
    CHECK_EQ(rgui_loop_init(), 0);
    rgui::wl::Transfers transfers(200, 16);
    // A selection owner that never writes nor closes: the paste ends empty after the timeout.
    int stalled[2];
    CHECK_EQ(pipe2(stalled, O_CLOEXEC | O_NONBLOCK), 0);
    int stalledCalls = 0;
    std::string stalledText = "unset";
    transfers.Receive(stalled[0], [&](std::string t) {
        ++stalledCalls;
        stalledText = std::move(t);
    });
    RunLoop(400);
    CHECK_EQ(stalledCalls, 1);
    CHECK_EQ(stalledText, std::string());
    CHECK_EQ(transfers.Incoming(), size_t(0));
    close(stalled[1]);

    // One paste at a time: a second Ctrl+V cancels the first, which ends empty at once.
    int first[2];
    int second[2];
    CHECK_EQ(pipe2(first, O_CLOEXEC | O_NONBLOCK), 0);
    CHECK_EQ(pipe2(second, O_CLOEXEC | O_NONBLOCK), 0);
    std::vector<std::string> pasted;
    transfers.ReceivePaste(first[0], [&](std::string t) { pasted.push_back("1:" + t); });
    transfers.ReceivePaste(second[0], [&](std::string t) { pasted.push_back("2:" + t); });
    CHECK_EQ(pasted.size(), size_t(1));
    if (!pasted.empty()) CHECK_EQ(pasted[0], std::string("1:"));
    CHECK_EQ(transfers.Incoming(), size_t(1));
    CHECK_EQ(write(second[1], "hello", 5), ssize_t(5));
    close(second[1]);
    RunLoop(50);
    CHECK_EQ(pasted.size(), size_t(2));
    if (pasted.size() == 2) CHECK_EQ(pasted[1], std::string("2:hello"));
    close(first[1]);

    // More than the byte limit: empty.
    int big[2];
    CHECK_EQ(pipe2(big, O_CLOEXEC | O_NONBLOCK), 0);
    std::string bigText = "unset";
    transfers.Receive(big[0], [&](std::string t) { bigText = std::move(t); });
    CHECK_EQ(write(big[1], "0123456789abcdefXYZ", 19), ssize_t(19));
    RunLoop(50);
    CHECK_EQ(bigText, std::string());
    close(big[1]);
}

RGUI_TEST("linux.window.geometry_caps") {
    using rgui::wl::FitScale120;
    auto device = [](int logical, uint32_t s) { return (static_cast<uint64_t>(logical) * s + 60) / 120; };
    auto fits = [&](int w, int h, uint32_t s) {
        const uint64_t dw = device(w, s);
        const uint64_t dh = device(h, s);
        return dw <= uint64_t(rgui::wl::kMaxDeviceSide) && dh <= uint64_t(rgui::wl::kMaxDeviceSide) &&
               dw * dh <= uint64_t(rgui::wl::kMaxDevicePixels);
    };
    // The biggest configure at the biggest scale: 131072 px per side asked, 16384 given.
    CHECK_EQ(FitScale120(16384, 16384, 960, true), 120u);
    CHECK_EQ(FitScale120(16384, 16384, 960, false), 120u);
    // Ordinary windows keep their scale.
    CHECK_EQ(FitScale120(1280, 820, 240, true), 240u);
    CHECK_EQ(FitScale120(1280, 820, 150, false), 150u);
    CHECK_EQ(FitScale120(1280, 820, 60, false), 60u);
    // A whole scale stays whole: 4 fits 4000x3000, 5 is over 16384 wide.
    CHECK_EQ(FitScale120(4000, 3000, 960, true), 480u);
    // Any request: the result fits, is the largest that does, never above the request.
    std::mt19937 random(7);
    for (int i = 0; i < 2000; ++i) {
        const int w = 720 + static_cast<int>(random() % (16384 - 720 + 1));
        const int h = 520 + static_cast<int>(random() % (16384 - 520 + 1));
        const bool whole = (random() & 1) != 0;
        const uint32_t asked = whole ? 120u * (1 + random() % 8) : 30 + random() % (960 - 30 + 1);
        const uint32_t got = FitScale120(w, h, asked, whole);
        CHECK(got <= asked);
        CHECK(fits(w, h, got));
        if (got < asked) CHECK(!fits(w, h, got + (whole ? 120 : 1)));
        if (whole) CHECK_EQ(got % 120, 0u);
    }
}

RGUI_TEST("linux.window.keymap_fd") {
    const std::string keymap = "xkb_keymap { };";
    const int fd = memfd_create("rgui-keymap", MFD_CLOEXEC);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK_EQ(write(fd, keymap.c_str(), keymap.size() + 1), ssize_t(keymap.size() + 1));
    std::string text;
    CHECK(rgui::wl::ReadKeymapFd(fd, static_cast<uint32_t>(keymap.size() + 1), &text));
    CHECK_EQ(text, keymap);
    // A size beyond the file's end: mapping it would raise SIGBUS on the first read past it.
    text = "unchanged";
    CHECK(!rgui::wl::ReadKeymapFd(fd, 4096, &text));
    CHECK_EQ(text, std::string("unchanged"));
    CHECK(!rgui::wl::ReadKeymapFd(fd, 0, &text));
    close(fd);
    // Not a regular file.
    int fds[2];
    CHECK_EQ(pipe2(fds, O_CLOEXEC), 0);
    CHECK(!rgui::wl::ReadKeymapFd(fds[0], 16, &text));
    close(fds[0]);
    close(fds[1]);
}
