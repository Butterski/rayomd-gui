// SPDX-License-Identifier: Apache-2.0
// The parts of the Wayland window that need no compositor: the path of a dropped text/uri-list
// and the XDG directories.
#include "platform/linux/wayland_window.h"
#include "runner.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
