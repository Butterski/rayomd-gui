// SPDX-License-Identifier: Apache-2.0
// The Win32 platform layer: one top-level window implementing rgui::Platform and driving an
// rgui::PlatformClient (the app).
//
// Use (main_win32.cpp):
//     rgui::win32::Win32Window window;
//     std::string error;
//     if (!window.Create({width, height, maximized, "RayoMD"}, &error)) { report error; return 1; }
//     App app(window);            // the app keeps the Platform&
//     window.SetClient(&app);
//     return window.Run();        // until Quit()
//
// Run() first calls OnSystemAppearance and OnResize, then OnFrame once, and shows the window only
// after that frame, so the first thing on screen is the app's. Afterwards OnFrame is called after
// RequestFrame: at once when the request came from input or a task, at the display's next
// vertical blank while frames are requested from inside OnFrame (animations; FramePacer), and
// from a 16 ms timer during the system's modal loops (moving or sizing the window, file dialogs).
// Each frame's damage is blitted before OnFrame's caller goes on.
//
// Frames: the framebuffer is a top-down 32-bpp DIB section in a memory DC; WM_PAINT blits from
// it. Present(frame, damage) copies the damage rectangles of `frame` into it, unless `frame` is
// Framebuffer() itself (the app rendered straight into the DIB): then nothing is copied. Either
// way each damage rectangle is invalidated. Framebuffer() changes on every OnResize.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/platform.h"
#include "platform/win32/frame_pacer.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace rgui::win32 {

struct WindowOptions {
    int logicalWidth = 1280;   // the client area, logical pixels (× DPI/96 on screen)
    int logicalHeight = 820;
    bool maximized = false;
    std::string title = "RayoMD";
};

// UTF-8 <-> UTF-16. Invalid input becomes U+FFFD: for text (the clipboard, the IME).
std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);
// For paths: false on an unpaired surrogate instead of replacing it, which would name another
// file (a\uD800.md would become a\uFFFD.md).
bool NarrowPath(std::wstring_view wide, std::string* utf8);

// AppsUseLightTheme (dark when 0; light when missing) and TextScaleFactor / 100 (1 when missing).
void ReadSystemAppearance(bool& dark, double& textScale);

class Win32Window final : public Platform {
public:
    // Smallest client area, logical pixels.
    static constexpr int kMinWidth = 720;
    static constexpr int kMinHeight = 520;

    Win32Window() = default;
    ~Win32Window() override;
    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    // Registers the class and creates the window, hidden. False, with `error`, on failure.
    bool Create(const WindowOptions& options, std::string* error);
    // The app; may be set after Create and before Run. Callbacks need a client.
    void SetClient(PlatformClient* client) { client_ = client; }
    // Runs the message loop until Quit (or WM_QUIT); returns the exit code.
    int Run();

    HWND Handle() const { return window_; }
    double Scale() const { return scale_; }
    // The DIB the window shows; render into it to skip Present's copy.
    gfx::Surface Framebuffer() const { return {pixels_, width_, height_, width_}; }
    bool Maximized() const;
    // The client size the window has when not maximized or minimized, logical pixels (for the
    // settings' windowWidth/windowHeight).
    void RestoredLogicalSize(int& width, int& height) const;

    // Platform.
    void SetTitle(std::string_view title) override;
    void SetCursor(Cursor cursor) override;
    void SetTextInputRect(gfx::RectI rect, bool enabled) override;
    void RequestFrame() override;
    void Present(const gfx::Surface& frame, const std::vector<gfx::RectI>& damage) override;
    bool NeedsClientDecorations() const override { return false; }
    void StartMove() override;
    void StartResize(Edge edge) override;
    void ToggleMaximize() override;
    void Minimize() override;
    bool IsMaximized() const override { return Maximized(); }
    void SetWindowColors(bool dark, uint32_t background, uint32_t foreground, uint32_t accent) override;
    void SetClipboardText(std::string text) override;
    void RequestClipboardText(std::function<void(std::string)> callback) override;
    void ShowFileDialog(FileDialog kind, std::string suggestedPath,
                        std::function<void(FileDialogResult)> callback) override;
    void OpenExternally(const std::string& path) override;
    void PostToUi(std::function<void()> task) override;
    uint64_t StartTimer(int ms, std::function<void()> callback) override;
    void CancelTimer(uint64_t id) override;
    void Quit() override;
    std::filesystem::path ConfigDir() const override;
    std::filesystem::path ImageCacheDir() const override;
    std::filesystem::path DocumentsDir() const override;
    std::optional<std::filesystem::path> OmarchyThemeDir() const override { return std::nullopt; }
    std::optional<std::filesystem::path> OmarchyUserShellToml() const override { return std::nullopt; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void HandleResize(int width, int height);
    bool CreateFramebuffer(int width, int height);
    void DestroyFramebuffer();
    void RunFrame();
    void EnsureFrameTimer();
    void EnterModal();
    void LeaveModal();
    void RunTasks();
    void RunDialog(FileDialog kind, const std::string& suggestedPath,
                   const std::function<void(FileDialogResult)>& callback);
    void UpdateImeWindows();
    void SendPreedit(std::wstring_view composition, LONG cursor);
    void SendText(std::string text);
    void Deliver(const InputEvent& event);
    void ReleaseButtons();
    uint8_t CurrentModifiers() const;
    float ToLogical(int devicePixels) const { return static_cast<float>(devicePixels / scale_); }
    LONG ToDevice(int logicalPixels) const;

    HWND window_ = nullptr;
    PlatformClient* client_ = nullptr;
    bool running_ = false;
    int exitCode_ = 0;
    bool comInitialized_ = false;
    bool showMaximized_ = false;

    // Framebuffer.
    HDC memoryDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    uint32_t* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    UINT dpi_ = 96;
    double scale_ = 1.0;
    bool started_ = false;       // Run delivered the first OnResize
    int reportedWidth_ = 0;      // what the client last got through OnResize
    int reportedHeight_ = 0;
    double reportedScale_ = 0;

    // Frames.
    bool framePending_ = false;  // RequestFrame since the last OnFrame
    bool frameNow_ = false;      // ... from outside OnFrame: run it without waiting for the timer
    bool inFrame_ = false;
    bool frameTimer_ = false;
    int modalDepth_ = 0;
    FramePacer pacer_;           // animation frames: the next vertical blank

    // Input.
    Cursor cursor_ = Cursor::Arrow;
    bool trackingLeave_ = false;
    unsigned buttonsDown_ = 0;   // bit per MouseButton
    float lastX_ = 0;
    float lastY_ = 0;
    wchar_t highSurrogate_ = 0;
    // The last wheel message with a delta that is not whole notches (a touchpad's), GetMessageTime.
    bool fineWheel_ = false;
    DWORD fineWheelTime_ = 0;
    gfx::RectI textInputRect_{};
    bool textInputEnabled_ = true;
    bool composing_ = false;

    // Timers: SetTimer ids are the Platform ids; 1 is the frame timer.
    uint64_t nextTimerId_ = 2;
    std::map<uint64_t, std::function<void()>> timers_;

    // Tasks from any thread.
    std::mutex taskMutex_;
    std::deque<std::function<void()>> tasks_;
    bool taskMessagePosted_ = false;

    // Appearance, as last told to the client.
    bool dark_ = true;
    double textScale_ = 1.0;
};

} // namespace rgui::win32
