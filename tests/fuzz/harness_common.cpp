// SPDX-License-Identifier: Apache-2.0
#include "harness.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#define RGUI_GETPID _getpid
#else
#include <unistd.h>
#define RGUI_GETPID getpid
#endif

namespace rgui::fuzz {

void Fail(const char* what) {
    std::fprintf(stderr, "fuzz invariant broken: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

namespace {

std::string& TempDirPath() {
    static std::string path;
    return path;
}

void RemoveTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(TempDirPath(), ec);
}

} // namespace

const char* TempDir() {
    std::string& path = TempDirPath();
    if (path.empty()) {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec) dir = ".";
        dir /= "rgui-fuzz-" + std::to_string(RGUI_GETPID());
        std::filesystem::create_directories(dir, ec);
        if (ec) Fail("cannot create the temporary directory");
        path = dir.string();
        std::atexit(RemoveTempDir);
    }
    return path.c_str();
}

} // namespace rgui::fuzz
