// SPDX-License-Identifier: Apache-2.0
// rayomd-gui.exe: the startup sequence of app/app.h on the Win32 window.
#include "app/app.h"
#include "app/args.h"
#include "app/version.h"
#include "platform/win32/win32_window.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// DLLs from System32 only, before anything loads one: the application directory (Downloads) and
// the working directory (a document's folder) are never searched, for us or for the DLLs the
// system loads on our behalf (themes, IMEs, shell extensions in the file dialogs, WIC codecs).
// Images from remote shares are refused. Looked up at run time, so headers or systems that lack
// a function only lose that part; failures are ignored.
void RestrictDllSearch() {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return;
    auto lookup = [kernel32](const char* name) {
        return reinterpret_cast<void (*)()>(GetProcAddress(kernel32, name));
    };
    using SetDefaultDllDirectoriesFn = BOOL(WINAPI*)(DWORD);
    using SetDllDirectoryFn = BOOL(WINAPI*)(LPCWSTR);
    using SetProcessMitigationPolicyFn = BOOL(WINAPI*)(int, PVOID, SIZE_T);
    constexpr DWORD kSearchSystem32 = 0x00000800;  // LOAD_LIBRARY_SEARCH_SYSTEM32
    constexpr int kProcessImageLoadPolicy = 10;     // PROCESS_MITIGATION_POLICY
    constexpr DWORD kNoRemoteImages = 1u << 0;      // PROCESS_MITIGATION_IMAGE_LOAD_POLICY bits
    constexpr DWORD kPreferSystem32Images = 1u << 2;
    if (auto set = reinterpret_cast<SetDefaultDllDirectoriesFn>(lookup("SetDefaultDllDirectories"))) {
        set(kSearchSystem32);
    }
    if (auto set = reinterpret_cast<SetDllDirectoryFn>(lookup("SetDllDirectoryW"))) set(L"");
    if (auto set = reinterpret_cast<SetProcessMitigationPolicyFn>(lookup("SetProcessMitigationPolicy"))) {
        DWORD flags = kNoRemoteImages | kPreferSystem32Images;
        set(kProcessImageLoadPolicy, &flags, sizeof flags);
    }
}

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
    RestrictDllSearch();
    const auto processStart = std::chrono::steady_clock::now();

    std::vector<std::string> arguments;
    for (int i = 1; i < __argc; ++i) {
        std::string argument;
        if (!rgui::win32::NarrowPath(__wargv[i], &argument)) {
            // Replacing the unpaired surrogate would name another file.
            AttachParentConsole();
            Write(STD_ERROR_HANDLE, "rayomd-gui: argument " + std::to_string(i) + " is not valid Unicode\n");
            return rgui::Args::kUsageExitCode;
        }
        arguments.push_back(std::move(argument));
    }
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
    // Without a settings directory there are no settings: never a relative rayomd-gui.ini in the
    // working directory, which may be a downloaded folder.
    const std::filesystem::path configDir = window.ConfigDir();
    rgui::Startup startup = rgui::BeginStartup(
        args, configDir.empty() ? std::filesystem::path() : configDir / L"rayomd-gui.ini", processStart);
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
