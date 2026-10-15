// SPDX-License-Identifier: Apache-2.0
// rayomd-gui.exe: the startup sequence of app/app.h on the Win32 window.
#include "app/app.h"
#include "app/args.h"
#include "app/version.h"
#include "platform/win32/win32_window.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// A GUI-subsystem process has no console; attach to the parent's (a terminal) once, so
// --version, --snapshot and usage errors print there. Redirected handles work without it.
void AttachParentConsole() {
    static bool attached = false;
    if (attached) return;
    attached = true;
    AttachConsole(ATTACH_PARENT_PROCESS);
}

void Write(DWORD which, const std::string& text) {
    if (text.empty()) return;
    HANDLE handle = GetStdHandle(which);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
    size_t offset = 0;
    while (offset < text.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(text.size() - offset, 1u << 20));
        if (!WriteFile(handle, text.data() + offset, chunk, &written, nullptr) || written == 0) return;
        offset += written;
    }
}

std::string WithNewline(std::string text) {
    if (!text.empty() && text.back() != '\n') text += '\n';
    return text;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto processStart = std::chrono::steady_clock::now();

    std::vector<std::string> arguments;
    for (int i = 1; i < __argc; ++i) arguments.push_back(rgui::win32::Narrow(__wargv[i]));
    const rgui::Args args = rgui::ParseArgs(arguments);

    if (args.mode == rgui::Args::Mode::Version) {
        AttachParentConsole();
        Write(STD_OUTPUT_HANDLE, std::string("rayomd-gui ") + rgui::Version() + "\n");
        return 0;
    }
    if (args.mode == rgui::Args::Mode::Usage) {
        AttachParentConsole();
        Write(STD_ERROR_HANDLE, std::string(rgui::Args::kUsage) + "\n");
        return rgui::Args::kUsageExitCode;
    }
    const bool console = args.mode == rgui::Args::Mode::Snapshot;
    if (console) AttachParentConsole();

    rgui::win32::Win32Window window;
    rgui::Startup startup = rgui::BeginStartup(args, window.ConfigDir() / L"rayomd-gui.ini", processStart);
    startup.doubleClickMs = static_cast<int>(GetDoubleClickTime());

    rgui::win32::WindowOptions options;
    options.logicalWidth = startup.windowWidth;
    options.logicalHeight = startup.windowHeight;
    options.maximized = startup.maximized;
    std::string error;
    if (!window.Create(options, &error)) {
        AttachParentConsole();
        Write(STD_ERROR_HANDLE, "rayomd-gui: " + WithNewline(error));
        return 1;
    }
    std::unique_ptr<rgui::App> app = rgui::App::Create(window, std::move(startup));
    window.SetClient(app.get());
    window.Run();
    window.SetClient(nullptr);

    if (!app->ExitStdout().empty() || !app->ExitStderr().empty()) AttachParentConsole();
    Write(STD_OUTPUT_HANDLE, WithNewline(app->ExitStdout()));
    Write(STD_ERROR_HANDLE, WithNewline(app->ExitStderr()));
    return app->ExitCode();
}
