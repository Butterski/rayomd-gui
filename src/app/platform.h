// SPDX-License-Identifier: Apache-2.0
// What the app needs from the operating system (Platform, implemented by the Wayland and Win32
// layers and by the tests' headless platform) and what the platform tells the app
// (PlatformClient, implemented by the app). Every Platform call and every PlatformClient
// callback happens on the UI thread, except PostToUi, which any thread may call.
#pragma once

#include "gfx/geometry.h"
#include "gfx/surface.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rgui {

enum class Cursor : uint8_t { Arrow, IBeam, Hand, ResizeH, ResizeV, ResizeNWSE, ResizeNESW };

// The window edge or corner a client-side resize starts from.
enum class Edge : uint8_t { Top, Bottom, Left, Right, TopLeft, TopRight, BottomLeft, BottomRight };

enum class FileDialog : uint8_t { OpenDocument, SaveMarkdown, ExportPdf, ChooseTheme };

struct FileDialogResult {
    // Unavailable: no portal, or the system dialog could not be shown; the app then asks for the
    // path itself. Failed: the system dialog reported a failure (the portal's Response code 2);
    // the app shows the status `File dialog failed`.
    enum Kind : uint8_t { Chosen, Cancelled, Unavailable, Failed } kind = Cancelled;
    std::string path;  // UTF-8, when Chosen
};

// Keys the app reacts to. Letters and digits are their US positions' names: the platform picks
// the Latin key a shortcut means (see the keyboard layer).
enum class Key : uint16_t {
    None,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    Enter, Escape, Tab, Backspace, Delete, Left, Right, Up, Down, Home, End, PageUp, PageDown, Insert,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Equal, Minus, Plus, Comma, Slash, Space,
};

enum Modifiers : uint8_t {
    kModCtrl = 1,
    kModShift = 2,
    kModAlt = 4,
    kModSuper = 8,
};

enum class MouseButton : uint8_t { Left, Right, Middle };

struct InputEvent {
    enum class Type : uint8_t {
        KeyDown,       // key, mods, repeat
        Text,          // text: committed UTF-8 (typed characters, Compose results, IME commits)
        Preedit,       // text: the IME's preedit, preeditCursor: its caret (byte offset, -1 none)
        PointerMove,   // x, y
        PointerLeave,  // the pointer left the window
        Button,        // x, y, button, pressed, mods
        Wheel,         // x, y, notches and/or pixels, mods
        FocusIn,
        FocusOut,
    };
    Type type = Type::KeyDown;
    Key key = Key::None;
    uint8_t mods = 0;      // Modifiers held
    bool repeat = false;   // KeyDown from key repeat
    std::string text;
    int preeditCursor = -1;
    float x = 0;           // pointer position, logical pixels from the window's top left
    float y = 0;
    MouseButton button = MouseButton::Left;
    bool pressed = false;
    // Wheel: discrete notches (a mouse wheel; positive scrolls the content down / right, as
    // rolling the wheel toward the user does) and continuous pixels (a touchpad, logical pixels,
    // same sign).
    float notchesX = 0;
    float notchesY = 0;
    float pixelsX = 0;
    float pixelsY = 0;
};

class Platform {
public:
    virtual ~Platform() = default;

    // Window and frames.
    virtual void SetTitle(std::string_view title) = 0;
    virtual void SetCursor(Cursor cursor) = 0;
    // Where the text caret is (logical pixels), for the IME's candidate window.
    virtual void SetTextInputRect(gfx::RectI rect, bool enabled) = 0;
    // Asks for one OnFrame call when the compositor is ready for a new frame.
    virtual void RequestFrame() = 0;
    // Shows `frame` (device pixels, the size of the last OnResize); `damage` lists the device
    // rectangles that changed since the previous Present.
    virtual void Present(const gfx::Surface& frame, const std::vector<gfx::RectI>& damage) = 0;
    // True when the window has no title bar of the system's: the app draws its own header.
    virtual bool NeedsClientDecorations() const = 0;
    virtual void StartMove() = 0;
    virtual void StartResize(Edge edge) = 0;
    virtual void ToggleMaximize() = 0;
    virtual void Minimize() = 0;
    // Whether the window is maximized now (the app then keeps the restored size it saves, and
    // draws no client-side resize edges). Platforms that cannot tell answer false.
    virtual bool IsMaximized() const { return false; }
    // Matches the system's window frame to the theme where the system draws one in colour
    // (Windows: dark title bar, and on Windows 11 caption, caption text and border colours).
    // Colours are 0xAARRGGBB, alpha ignored. Platforms without such a frame do nothing.
    virtual void SetWindowColors(bool /*dark*/, uint32_t /*background*/, uint32_t /*foreground*/,
                                 uint32_t /*accent*/) {}

    // Clipboard.
    virtual void SetClipboardText(std::string text) = 0;
    // Calls back on the UI thread with the clipboard's text, empty when it has none.
    virtual void RequestClipboardText(std::function<void(std::string)> callback) = 0;

    // Dialogs and files.
    virtual void ShowFileDialog(FileDialog kind, std::string suggestedPath,
                                std::function<void(FileDialogResult)> callback) = 0;
    virtual void OpenExternally(const std::string& path) = 0;

    // Process.
    virtual void PostToUi(std::function<void()> task) = 0;  // any thread
    // A one-shot timer; the id is never 0.
    virtual uint64_t StartTimer(int ms, std::function<void()> callback) = 0;
    virtual void CancelTimer(uint64_t id) = 0;
    virtual void Quit() = 0;
    // The clock of StartTimer and of the app's frames, monotonic milliseconds (a steady clock;
    // the tests' platform answers its fake time).
    virtual double NowMs() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Directories (created by their users when missing).
    virtual std::filesystem::path ConfigDir() const = 0;
    virtual std::filesystem::path ImageCacheDir() const = 0;
    virtual std::filesystem::path DocumentsDir() const = 0;

    // Omarchy: the current theme's directory and the user's shell.toml, when they exist.
    virtual std::optional<std::filesystem::path> OmarchyThemeDir() const = 0;
    virtual std::optional<std::filesystem::path> OmarchyUserShellToml() const = 0;
};

class PlatformClient {
public:
    virtual ~PlatformClient() = default;

    virtual void OnInput(const InputEvent& event) = 0;
    // The framebuffer's size in device pixels and the device pixels per logical pixel.
    virtual void OnResize(int width, int height, double scale) = 0;
    virtual void OnCloseRequest() = 0;
    virtual void OnDropFile(std::string path) = 0;
    // Light or dark, and the system's text scale factor.
    virtual void OnSystemAppearance(bool dark, double textScale) = 0;
    virtual void OnOmarchyChanged() = 0;
    virtual void OnFontsChanged() = 0;
    virtual void OnRoundingChanged(int rounding) = 0;
    // Draw and Present now, if anything changed.
    virtual void OnFrame() = 0;
};

} // namespace rgui
