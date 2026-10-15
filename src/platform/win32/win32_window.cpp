// SPDX-License-Identifier: Apache-2.0
#include "platform/win32/win32_window.h"

#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <imm.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include "common/exact_math.h"

#include <algorithm>
#include <cstring>
#include <system_error>
#include <utility>

namespace rgui::win32 {

namespace {

constexpr wchar_t kClassName[] = L"RayoMDGui";
constexpr UINT kTaskMessage = WM_APP + 1;
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameIntervalMs = 16;
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD kWindowExStyle = 0;
constexpr size_t kDialogBufferChars = 32768;
constexpr size_t kMaxClipboardChars = size_t{64} << 20;  // 128 MiB of UTF-16

// DwmSetWindowAttribute attributes (dwmapi.h of older SDKs lacks the Windows 11 ones).
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmTextColor = 36;

// User32 functions newer than Windows 8.1, looked up so the executable still starts there.
template <typename Function>
Function User32Procedure(const char* name) {
    HMODULE module = GetModuleHandleW(L"user32.dll");
    if (!module) return nullptr;
    return reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

UINT WindowDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow = User32Procedure<GetDpiForWindowFn>("GetDpiForWindow");
    if (getDpiForWindow) {
        const UINT dpi = getDpiForWindow(window);
        if (dpi != 0) return dpi;
    }
    HDC dc = GetDC(window);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(window, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

// The window size whose client area is width x height device pixels.
SIZE WindowSizeForClient(int width, int height, UINT dpi) {
    using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjustForDpi = User32Procedure<AdjustForDpiFn>("AdjustWindowRectExForDpi");
    RECT rect{0, 0, width, height};
    if (adjustForDpi) {
        adjustForDpi(&rect, kWindowStyle, FALSE, kWindowExStyle, dpi);
    } else {
        AdjustWindowRectEx(&rect, kWindowStyle, FALSE, kWindowExStyle);
    }
    return {rect.right - rect.left, rect.bottom - rect.top};
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

Key KeyFromVirtualKey(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(vk - 'A'));
    if (vk >= '0' && vk <= '9') {
        return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(vk - '0'));
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(vk - VK_NUMPAD0));
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(vk - VK_F1));
    }
    switch (vk) {
    case VK_RETURN: return Key::Enter;
    case VK_ESCAPE: return Key::Escape;
    case VK_TAB: return Key::Tab;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_INSERT: return Key::Insert;
    case VK_SPACE: return Key::Space;
    case VK_OEM_PLUS: return Key::Equal;  // the =/+ key of the US layout
    case VK_ADD: return Key::Plus;
    case VK_OEM_MINUS:
    case VK_SUBTRACT: return Key::Minus;
    case VK_OEM_COMMA: return Key::Comma;
    case VK_OEM_2:
    case VK_DIVIDE: return Key::Slash;
    default: return Key::None;
    }
}

// Windows clipboard text uses CRLF; the app's text uses LF.
std::wstring ToClipboardText(std::string_view utf8) {
    const std::wstring wide = Widen(utf8);
    std::wstring out;
    out.reserve(wide.size() + wide.size() / 16 + 1);
    for (size_t i = 0; i < wide.size(); ++i) {
        if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r')) out.push_back(L'\r');
        out.push_back(wide[i]);
    }
    return out;
}

std::string FromClipboardText(std::wstring_view wide) {
    std::wstring text;
    text.reserve(wide.size());
    for (size_t i = 0; i < wide.size(); ++i) {
        if (wide[i] == L'\r' && i + 1 < wide.size() && wide[i + 1] == L'\n') continue;
        text.push_back(wide[i]);
    }
    return Narrow(text);
}

bool OpenClipboardRetrying(HWND window) {
    // Another program may hold the clipboard for a moment.
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(window)) return true;
        Sleep(2);
    }
    return false;
}

std::wstring CompositionString(HIMC context, DWORD index) {
    const LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (bytes <= 0) return {};
    std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
    const LONG got = ImmGetCompositionStringW(context, index, text.data(), static_cast<DWORD>(bytes));
    if (got <= 0) return {};
    text.resize(std::min(text.size(), static_cast<size_t>(got) / sizeof(wchar_t)));
    return text;
}

// An absolute directory from the environment, else empty: a relative one would put settings
// or the cache in whatever folder the app was started from.
std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    if (length == 0 || length >= size) return {};
    value.resize(length);
    std::filesystem::path path(value);
    return path.is_absolute() ? path : std::filesystem::path();
}

std::filesystem::path KnownFolder(int csidl) {
    wchar_t buffer[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buffer))) {
        std::filesystem::path path(buffer);
        if (path.is_absolute()) return path;
    }
    return {};
}

COLORREF ToColorRef(uint32_t argb) {
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

HCURSOR CursorHandle(Cursor cursor) {
    const wchar_t* name = IDC_ARROW;
    switch (cursor) {
    case Cursor::Arrow: name = IDC_ARROW; break;
    case Cursor::IBeam: name = IDC_IBEAM; break;
    case Cursor::Hand: name = IDC_HAND; break;
    case Cursor::ResizeH: name = IDC_SIZEWE; break;
    case Cursor::ResizeV: name = IDC_SIZENS; break;
    case Cursor::ResizeNWSE: name = IDC_SIZENWSE; break;
    case Cursor::ResizeNESW: name = IDC_SIZENESW; break;
    }
    return LoadCursorW(nullptr, name);
}

struct DialogSpec {
    const wchar_t* title;
    const wchar_t* filter;  // pairs of NUL-terminated strings, ending with an empty string
    const wchar_t* defaultExtension;
    bool save;
};

DialogSpec DialogFor(FileDialog kind) {
    switch (kind) {
    case FileDialog::OpenDocument:
        return {L"Open Markdown or RayoMD PDF",
                L"Markdown (*.md;*.markdown;*.txt)\0*.md;*.markdown;*.txt\0"
                L"RayoMD PDF (*.pdf)\0*.pdf\0All files (*.*)\0*.*\0",
                nullptr, false};
    case FileDialog::SaveMarkdown:
        return {L"Save Markdown", L"Markdown (*.md)\0*.md\0", L"md", true};
    case FileDialog::ExportPdf:
        return {L"Export PDF", L"PDF (*.pdf)\0*.pdf\0", L"pdf", true};
    case FileDialog::ChooseTheme:
        return {L"Choose PDF theme file", L"Theme files (*.theme;*.txt)\0*.theme;*.txt\0All files (*.*)\0*.*\0",
                nullptr, false};
    }
    return {L"", L"All files (*.*)\0*.*\0", nullptr, false};
}

} // namespace

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int count = static_cast<int>(std::min<size_t>(utf8.size(), 0x7FFFFFFF));
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), count, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), count, wide.data(), length);
    return wide;
}

std::string Narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int count = static_cast<int>(std::min<size_t>(wide.size(), 0x7FFFFFFF));
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, utf8.data(), length, nullptr, nullptr);
    return utf8;
}

bool NarrowPath(std::wstring_view wide, std::string* utf8) {
    utf8->clear();
    if (wide.empty()) return true;
    if (wide.size() > 0x7FFFFFFF) return false;
    const int count = static_cast<int>(wide.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return false;
    utf8->assign(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count, utf8->data(), length, nullptr, nullptr) !=
        length) {
        utf8->clear();
        return false;
    }
    return true;
}

void ReadSystemAppearance(bool& dark, double& textScale) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    dark = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value == 0;
    value = 0;
    size = sizeof(value);
    textScale = 1.0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
        value > 0) {
        textScale = value / 100.0;
    }
}

Win32Window::~Win32Window() {
    if (window_) DestroyWindow(window_);
    DestroyFramebuffer();
    if (comInitialized_) CoUninitialize();
}

bool Win32Window::Create(const WindowOptions& options, std::string* error) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Win32Window::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON),
                                                        GetSystemMetrics(SM_CYSMICON), 0));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error) *error = "RegisterClassExW failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    // The common dialogs and ShellExecute want a single-threaded COM apartment on this thread.
    comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

    const std::wstring title = Widen(options.title);
    HWND created = CreateWindowExW(kWindowExStyle, kClassName, title.c_str(), kWindowStyle, CW_USEDEFAULT,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, this);
    if (!created) {
        if (error) *error = "CreateWindowExW failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    window_ = created;
    dpi_ = WindowDpi(window_);
    scale_ = dpi_ / 96.0;
    const SIZE size = WindowSizeForClient(ToDevice(std::max(options.logicalWidth, kMinWidth)),
                                          ToDevice(std::max(options.logicalHeight, kMinHeight)), dpi_);
    SetWindowPos(window_, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT client{};
    GetClientRect(window_, &client);
    HandleResize(client.right - client.left, client.bottom - client.top);
    if (!pixels_) {
        if (error) *error = "cannot create the window's framebuffer";
        return false;
    }
    showMaximized_ = options.maximized;
    DragAcceptFiles(window_, TRUE);
    ReadSystemAppearance(dark_, textScale_);

    // Tasks posted before the window existed.
    bool post = false;
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        if (!tasks_.empty() && !taskMessagePosted_) post = taskMessagePosted_ = true;
    }
    if (post) PostMessageW(window_, kTaskMessage, 0, 0);
    return true;
}

int Win32Window::Run() {
    if (!window_) return 1;
    if (client_) client_->OnSystemAppearance(dark_, textScale_);
    started_ = true;
    if (client_) {
        reportedWidth_ = width_;
        reportedHeight_ = height_;
        reportedScale_ = scale_;
        client_->OnResize(width_, height_, scale_);
        RunFrame();
    }
    // Tasks posted while starting (the opened file's text) run before the window is shown: the
    // first ShowWindow and paint take tens of milliseconds, which the first preview build then
    // overlaps instead of following (CI: the file's text was taken 35 ms after the first frame).
    RunTasks();
    ShowWindow(window_, showMaximized_ ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(window_);

    running_ = true;
    while (running_) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running_ = false;
                exitCode_ = static_cast<int>(message.wParam);
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running_) break;
        if (framePending_ && frameNow_) {
            RunFrame();
            continue;  // drain again: the frame's WM_PAINT, and anything that came meanwhile
        }
        WaitMessage();
    }
    return exitCode_;
}

bool Win32Window::Maximized() const {
    return window_ && IsZoomed(window_);
}

void Win32Window::RestoredLogicalSize(int& width, int& height) const {
    width = reportedWidth_ > 0 ? static_cast<int>(RayoMd::ExactMath::LRound(reportedWidth_ / scale_)) : 0;
    height = reportedHeight_ > 0 ? static_cast<int>(RayoMd::ExactMath::LRound(reportedHeight_ / scale_)) : 0;
    if (!window_) return;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window_, &placement)) return;
    // rcNormalPosition is the whole window; take the frame off.
    const SIZE frame = WindowSizeForClient(0, 0, dpi_);
    const int deviceWidth = placement.rcNormalPosition.right - placement.rcNormalPosition.left - frame.cx;
    const int deviceHeight = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top - frame.cy;
    if (deviceWidth > 0 && deviceHeight > 0) {
        width = std::max(kMinWidth, static_cast<int>(RayoMd::ExactMath::LRound(deviceWidth / scale_)));
        height = std::max(kMinHeight, static_cast<int>(RayoMd::ExactMath::LRound(deviceHeight / scale_)));
    }
}

LONG Win32Window::ToDevice(int logicalPixels) const {
    return static_cast<LONG>(RayoMd::ExactMath::LRound(logicalPixels * scale_));
}

// --- Window and frames ---------------------------------------------------------------------

void Win32Window::SetTitle(std::string_view title) {
    if (window_) SetWindowTextW(window_, Widen(title).c_str());
}

void Win32Window::SetCursor(Cursor cursor) {
    cursor_ = cursor;
    // While the pointer is over the client area (or captured), show it at once.
    if (trackingLeave_ || buttonsDown_ != 0) ::SetCursor(CursorHandle(cursor_));
}

void Win32Window::SetTextInputRect(gfx::RectI rect, bool enabled) {
    textInputRect_ = rect;
    if (enabled != textInputEnabled_ && window_) {
        textInputEnabled_ = enabled;
        // Without a text field the IME stays off, so shortcuts reach the app as keys.
        ImmAssociateContextEx(window_, nullptr, enabled ? IACE_DEFAULT : 0);
    }
    if (enabled) UpdateImeWindows();
}

void Win32Window::RequestFrame() {
    framePending_ = true;
    if (inFrame_ || modalDepth_ > 0) {
        EnsureFrameTimer();
    } else {
        frameNow_ = true;
    }
}

void Win32Window::EnsureFrameTimer() {
    if (!frameTimer_ && window_) frameTimer_ = SetTimer(window_, kFrameTimer, kFrameIntervalMs, nullptr) != 0;
}

void Win32Window::RunFrame() {
    if (!client_ || !started_ || inFrame_) return;
    framePending_ = false;
    frameNow_ = false;
    inFrame_ = true;
    client_->OnFrame();
    inFrame_ = false;
    if (!framePending_ && frameTimer_) {
        KillTimer(window_, kFrameTimer);
        frameTimer_ = false;
    }
}

void Win32Window::EnterModal() {
    ++modalDepth_;
    if (framePending_) EnsureFrameTimer();
}

void Win32Window::LeaveModal() {
    if (modalDepth_ > 0) --modalDepth_;
    if (framePending_) frameNow_ = true;
}

void Win32Window::Present(const gfx::Surface& frame, const std::vector<gfx::RectI>& damage) {
    if (!pixels_ || !window_) return;
    GdiFlush();  // GDI may still read the DIB
    const bool inPlace = frame.pixels == pixels_;
    gfx::RectI bounds{0, 0, width_, height_};
    if (!inPlace) bounds = bounds.Intersect({0, 0, frame.width, frame.height});
    for (const gfx::RectI& rect : damage) {
        const gfx::RectI area = rect.Intersect(bounds);
        if (area.Empty()) continue;
        if (!inPlace && frame.pixels) {
            for (int y = area.y0; y < area.y1; ++y) {
                std::memcpy(pixels_ + static_cast<ptrdiff_t>(y) * width_ + area.x0, frame.Row(y) + area.x0,
                            static_cast<size_t>(area.Width()) * sizeof(uint32_t));
            }
        }
        const RECT invalid{area.x0, area.y0, area.x1, area.y1};
        InvalidateRect(window_, &invalid, FALSE);
    }
}

void Win32Window::StartMove() {
    PostToUi([this] {
        if (!window_) return;
        ReleaseCapture();
        POINT point{};
        GetCursorPos(&point);
        PostMessageW(window_, WM_NCLBUTTONDOWN, HTCAPTION,
                     MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)));
    });
}

void Win32Window::StartResize(Edge edge) {
    WPARAM hit = HTBOTTOMRIGHT;
    switch (edge) {
    case Edge::Top: hit = HTTOP; break;
    case Edge::Bottom: hit = HTBOTTOM; break;
    case Edge::Left: hit = HTLEFT; break;
    case Edge::Right: hit = HTRIGHT; break;
    case Edge::TopLeft: hit = HTTOPLEFT; break;
    case Edge::TopRight: hit = HTTOPRIGHT; break;
    case Edge::BottomLeft: hit = HTBOTTOMLEFT; break;
    case Edge::BottomRight: hit = HTBOTTOMRIGHT; break;
    }
    PostToUi([this, hit] {
        if (!window_) return;
        ReleaseCapture();
        POINT point{};
        GetCursorPos(&point);
        PostMessageW(window_, WM_NCLBUTTONDOWN, hit,
                     MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)));
    });
}

// Deferred to the loop: ShowWindow resizes at once, and OnResize must not arrive inside the
// app's own call.
void Win32Window::ToggleMaximize() {
    PostToUi([this] {
        if (window_) ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
    });
}

void Win32Window::Minimize() {
    PostToUi([this] {
        if (window_) ShowWindow(window_, SW_MINIMIZE);
    });
}

void Win32Window::SetWindowColors(bool dark, uint32_t background, uint32_t foreground, uint32_t accent) {
    if (!window_) return;
    // Failures are ignored: attribute 20 needs Windows 10 20H1, 34-36 Windows 11.
    const BOOL darkMode = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window_, kDwmUseImmersiveDarkMode, &darkMode, sizeof(darkMode));
    const COLORREF caption = ToColorRef(background);
    const COLORREF text = ToColorRef(foreground);
    const COLORREF border = ToColorRef(accent);
    DwmSetWindowAttribute(window_, kDwmCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(window_, kDwmTextColor, &text, sizeof(text));
    DwmSetWindowAttribute(window_, kDwmBorderColor, &border, sizeof(border));
}

// --- Clipboard --------------------------------------------------------------------------------

void Win32Window::SetClipboardText(std::string text) {
    if (!window_) return;
    const std::wstring wide = ToClipboardText(text);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    void* locked = GlobalLock(memory);
    if (!locked) {
        GlobalFree(memory);
        return;
    }
    std::memcpy(locked, wide.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboardRetrying(window_)) {
        GlobalFree(memory);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);  // else the system owns it
    CloseClipboard();
}

void Win32Window::RequestClipboardText(std::function<void(std::string)> callback) {
    std::string text;
    if (window_ && OpenClipboardRetrying(window_)) {
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data) {
            const auto* chars = static_cast<const wchar_t*>(GlobalLock(data));
            if (chars) {
                const size_t capacity = std::min(GlobalSize(data) / sizeof(wchar_t), kMaxClipboardChars);
                size_t length = 0;
                while (length < capacity && chars[length] != L'\0') ++length;
                text = FromClipboardText(std::wstring_view(chars, length));
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }
    // Read now, answered from the loop, as the other platforms answer.
    PostToUi([callback = std::move(callback), text = std::move(text)]() mutable {
        if (callback) callback(std::move(text));
    });
}

// --- Dialogs and files ------------------------------------------------------------------------

void Win32Window::ShowFileDialog(FileDialog kind, std::string suggestedPath,
                                 std::function<void(FileDialogResult)> callback) {
    // The dialog runs its own modal loop; start it from the loop, not inside the app's call.
    PostToUi([this, kind, suggestedPath = std::move(suggestedPath), callback = std::move(callback)] {
        RunDialog(kind, suggestedPath, callback);
    });
}

void Win32Window::RunDialog(FileDialog kind, const std::string& suggestedPath,
                            const std::function<void(FileDialogResult)>& callback) {
    const DialogSpec spec = DialogFor(kind);
    std::wstring buffer(kDialogBufferChars, L'\0');
    std::wstring initialDirectory;
    if (!suggestedPath.empty()) {
        const std::filesystem::path suggested(Widen(suggestedPath));
        std::error_code ec;
        if (std::filesystem::is_directory(suggested, ec)) {
            initialDirectory = suggested.wstring();
        } else {
            initialDirectory = suggested.parent_path().wstring();
            const std::wstring name = suggested.filename().wstring();
            if (spec.save && name.size() < kDialogBufferChars) std::copy(name.begin(), name.end(), buffer.begin());
        }
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = spec.filter;
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(kDialogBufferChars);
    dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
    dialog.lpstrTitle = spec.title;
    dialog.lpstrDefExt = spec.defaultExtension;
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (spec.save) dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;

    EnterModal();
    const BOOL chosen = spec.save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    const DWORD failure = chosen ? 0 : CommDlgExtendedError();
    LeaveModal();

    FileDialogResult result;
    if (chosen) {
        buffer.resize(std::wcslen(buffer.c_str()));
        if (NarrowPath(buffer, &result.path)) {
            result.kind = FileDialogResult::Chosen;
        } else {
            result.kind = FileDialogResult::Cancelled;
            if (client_) client_->OnPlatformError("The chosen file name is not valid Unicode; RayoMD cannot use it");
        }
    } else {
        result.kind = failure == 0 ? FileDialogResult::Cancelled : FileDialogResult::Unavailable;
    }
    if (callback) callback(std::move(result));
}

void Win32Window::OpenExternally(const std::string& path) {
    // Always the PDF handler, whatever the file is called: a target named .hta or .bat must not
    // run its handler on our bytes. No error dialogs of the shell's own.
    const std::wstring wide = Widen(path);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_CLASSNAME | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = wide.c_str();
    info.lpClass = L".pdf";
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&info);
}

// --- Process ----------------------------------------------------------------------------------

void Win32Window::PostToUi(std::function<void()> task) {
    bool post = false;
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        tasks_.push_back(std::move(task));
        if (!taskMessagePosted_ && window_) post = taskMessagePosted_ = true;
    }
    if (post && !PostMessageW(window_, kTaskMessage, 0, 0)) {
        std::lock_guard<std::mutex> lock(taskMutex_);
        taskMessagePosted_ = false;
    }
}

void Win32Window::RunTasks() {
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        taskMessagePosted_ = false;
        count = tasks_.size();
    }
    // Only the tasks queued so far: a task that posts another does not starve the loop. Tasks
    // may re-enter (a file dialog's modal loop runs the next ones), so each is taken under the
    // lock just before it runs.
    for (size_t i = 0; i < count; ++i) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(taskMutex_);
            if (tasks_.empty()) break;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        if (task) task();
    }
    bool post = false;
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        if (!tasks_.empty() && !taskMessagePosted_ && window_) post = taskMessagePosted_ = true;
    }
    if (post) PostMessageW(window_, kTaskMessage, 0, 0);
}

uint64_t Win32Window::StartTimer(int ms, std::function<void()> callback) {
    const uint64_t id = nextTimerId_++;
    timers_[id] = std::move(callback);
    if (window_) SetTimer(window_, static_cast<UINT_PTR>(id), static_cast<UINT>(std::max(ms, 0)), nullptr);
    return id;
}

void Win32Window::CancelTimer(uint64_t id) {
    if (timers_.erase(id) != 0 && window_) KillTimer(window_, static_cast<UINT_PTR>(id));
}

void Win32Window::Quit() {
    running_ = false;
    PostQuitMessage(0);
}

// --- Directories ------------------------------------------------------------------------------

std::filesystem::path Win32Window::ConfigDir() const {
    std::filesystem::path base = EnvironmentPath(L"APPDATA");
    if (base.empty()) base = KnownFolder(CSIDL_APPDATA);
    if (base.empty()) return {};
    return base / L"RayoMD";
}

std::filesystem::path Win32Window::ImageCacheDir() const {
    std::filesystem::path base = EnvironmentPath(L"LOCALAPPDATA");
    if (base.empty()) base = KnownFolder(CSIDL_LOCAL_APPDATA);
    if (base.empty()) return {};
    return base / L"RayoMD" / L"cache" / L"images";
}

std::filesystem::path Win32Window::DocumentsDir() const {
    std::filesystem::path documents = KnownFolder(CSIDL_PERSONAL);
    if (!documents.empty()) return documents;
    const std::filesystem::path profile = EnvironmentPath(L"USERPROFILE");
    if (profile.empty()) return {};
    return profile / L"Documents";
}

// --- Framebuffer ------------------------------------------------------------------------------

bool Win32Window::CreateFramebuffer(int width, int height) {
    if (!memoryDc_) memoryDc_ = CreateCompatibleDC(nullptr);
    if (!memoryDc_) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memoryDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        return false;
    }
    GdiFlush();
    auto* pixels = static_cast<uint32_t*>(bits);
    // Keep what was on screen until the app presents the new size; new area in the old
    // top-left colour (the window background).
    const uint32_t fill = pixels_ ? pixels_[0] : 0;
    for (int y = 0; y < height; ++y) {
        uint32_t* row = pixels + static_cast<ptrdiff_t>(y) * width;
        int copied = 0;
        if (pixels_ && y < height_) {
            copied = std::min(width, width_);
            std::memcpy(row, pixels_ + static_cast<ptrdiff_t>(y) * width_, static_cast<size_t>(copied) * 4);
        }
        std::fill(row + copied, row + width, fill);
    }
    HGDIOBJ previous = SelectObject(memoryDc_, bitmap);
    if (bitmap_) {
        DeleteObject(bitmap_);
    } else {
        previousBitmap_ = previous;
    }
    bitmap_ = bitmap;
    pixels_ = pixels;
    width_ = width;
    height_ = height;
    return true;
}

void Win32Window::DestroyFramebuffer() {
    if (memoryDc_ && previousBitmap_) SelectObject(memoryDc_, previousBitmap_);
    if (bitmap_) DeleteObject(bitmap_);
    if (memoryDc_) DeleteDC(memoryDc_);
    memoryDc_ = nullptr;
    bitmap_ = nullptr;
    previousBitmap_ = nullptr;
    pixels_ = nullptr;
    width_ = height_ = 0;
}

void Win32Window::HandleResize(int width, int height) {
    if (width <= 0 || height <= 0) return;  // minimized: keep the framebuffer
    if ((width != width_ || height != height_) && !CreateFramebuffer(width, height)) return;
    if (!client_ || !started_) return;
    if (width_ == reportedWidth_ && height_ == reportedHeight_ && scale_ == reportedScale_) return;
    reportedWidth_ = width_;
    reportedHeight_ = height_;
    reportedScale_ = scale_;
    client_->OnResize(width_, height_, scale_);
    // Draw now: while the user drags the border the system's sizing loop runs, not ours.
    if (framePending_) RunFrame();
}

// --- Input ------------------------------------------------------------------------------------

uint8_t Win32Window::CurrentModifiers() const {
    uint8_t mods = 0;
    if (GetKeyState(VK_CONTROL) < 0) mods |= kModCtrl;
    if (GetKeyState(VK_SHIFT) < 0) mods |= kModShift;
    if (GetKeyState(VK_MENU) < 0) mods |= kModAlt;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= kModSuper;
    return mods;
}

void Win32Window::Deliver(const InputEvent& event) {
    if (client_ && started_) client_->OnInput(event);
}

void Win32Window::SendText(std::string text) {
    InputEvent event;
    event.type = InputEvent::Type::Text;
    event.text = std::move(text);
    event.mods = CurrentModifiers();
    Deliver(event);
}

void Win32Window::SendPreedit(std::wstring_view composition, LONG cursor) {
    InputEvent event;
    event.type = InputEvent::Type::Preedit;
    event.text = Narrow(composition);
    event.preeditCursor = -1;
    if (!event.text.empty() && cursor >= 0 && static_cast<size_t>(cursor) <= composition.size()) {
        event.preeditCursor = static_cast<int>(Narrow(composition.substr(0, static_cast<size_t>(cursor))).size());
    }
    Deliver(event);
}

void Win32Window::UpdateImeWindows() {
    if (!window_ || !textInputEnabled_) return;
    HIMC context = ImmGetContext(window_);
    if (!context) return;
    const LONG left = ToDevice(textInputRect_.x0);
    const LONG top = ToDevice(textInputRect_.y0);
    const LONG right = ToDevice(textInputRect_.x1);
    const LONG bottom = ToDevice(textInputRect_.y1);
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = {left, top};
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = {left, bottom};
    candidate.rcArea = {left, top, right, bottom};
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(window_, context);
}

void Win32Window::ReleaseButtons() {
    for (int bit = 0; bit < 3; ++bit) {
        if ((buttonsDown_ & (1u << bit)) == 0) continue;
        InputEvent event;
        event.type = InputEvent::Type::Button;
        event.x = lastX_;
        event.y = lastY_;
        event.button = static_cast<MouseButton>(bit);
        event.pressed = false;
        event.mods = CurrentModifiers();
        Deliver(event);
    }
    buttonsDown_ = 0;
}

LRESULT CALLBACK Win32Window::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<Win32Window*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->window_ = nullptr;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT Win32Window::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    HWND window = window_;
    switch (message) {
    case WM_CLOSE:
        if (client_ && started_) {
            client_->OnCloseRequest();
        } else {
            Quit();
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        if (dc && memoryDc_) {
            const RECT& r = paint.rcPaint;
            BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, memoryDc_, r.left, r.top, SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) HandleResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const SIZE minimum = WindowSizeForClient(ToDevice(kMinWidth), ToDevice(kMinHeight), dpi_);
        info->ptMinTrackSize = {minimum.cx, minimum.cy};
        return 0;
    }

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam) != 0 ? HIWORD(wParam) : 96;
        scale_ = dpi_ / 96.0;
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        // OnResize even when the client size stayed the same: the scale changed.
        RECT client{};
        GetClientRect(window, &client);
        HandleResize(client.right - client.left, client.bottom - client.top);
        UpdateImeWindows();
        return 0;
    }

    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
        EnterModal();
        return 0;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
        LeaveModal();
        return 0;

    case WM_TIMER:
        if (wParam == kFrameTimer) {
            if (framePending_) {
                RunFrame();
            } else {
                KillTimer(window, kFrameTimer);
                frameTimer_ = false;
            }
            return 0;
        } else {
            const auto found = timers_.find(static_cast<uint64_t>(wParam));
            KillTimer(window, wParam);
            if (found == timers_.end()) return 0;
            std::function<void()> callback = std::move(found->second);
            timers_.erase(found);
            if (callback) callback();
            return 0;
        }

    case kTaskMessage:
        RunTasks();
        return 0;

    case WM_SETTINGCHANGE: {
        // ImmersiveColorSet announces a light/dark switch; the text scale comes with others.
        bool dark = dark_;
        double textScale = textScale_;
        ReadSystemAppearance(dark, textScale);
        if (dark != dark_ || textScale != textScale_) {
            dark_ = dark;
            textScale_ = textScale;
            if (client_ && started_) client_->OnSystemAppearance(dark_, textScale_);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    case WM_SETFOCUS: {
        InputEvent event;
        event.type = InputEvent::Type::FocusIn;
        Deliver(event);
        return 0;
    }
    case WM_KILLFOCUS: {
        highSurrogate_ = 0;
        InputEvent event;
        event.type = InputEvent::Type::FocusOut;
        Deliver(event);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const Key key = wParam == VK_PROCESSKEY ? Key::None : KeyFromVirtualKey(wParam);
        if (key != Key::None) {
            InputEvent event;
            event.type = InputEvent::Type::KeyDown;
            event.key = key;
            event.mods = CurrentModifiers();
            event.repeat = (lParam & (LPARAM{1} << 30)) != 0;
            Deliver(event);
        }
        if (message == WM_SYSKEYDOWN && wParam != VK_F10) {
            return DefWindowProcW(window, message, wParam, lParam);  // Alt+F4, Alt+Space
        }
        return 0;
    }

    case WM_SYSCHAR:
        // Alt+letter is a shortcut for the app; without a menu the system would only beep.
        if (wParam == L' ') return DefWindowProcW(window, message, wParam, lParam);
        return 0;

    case WM_SYSCOMMAND:
        // Alt pressed and released alone would put the window into menu mode.
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0) return 0;
        return DefWindowProcW(window, message, wParam, lParam);

    case WM_CHAR: {
        const auto unit = static_cast<wchar_t>(wParam);
        uint32_t cp = unit;
        if (IS_HIGH_SURROGATE(unit)) {
            highSurrogate_ = unit;
            return 0;
        }
        if (IS_LOW_SURROGATE(unit)) {
            if (!highSurrogate_) return 0;
            cp = 0x10000u + ((static_cast<uint32_t>(highSurrogate_) - 0xD800u) << 10) +
                 (static_cast<uint32_t>(unit) - 0xDC00u);
        }
        highSurrogate_ = 0;
        // Control characters (Ctrl+letter, Enter, Tab, Backspace, Escape) arrive as keys.
        if (cp < 0x20 || cp == 0x7F) return 0;
        std::string text;
        AppendUtf8(text, cp);
        SendText(std::move(text));
        return 0;
    }

    case WM_IME_SETCONTEXT:
        // The app draws the composition at its caret; the IME keeps its candidate window.
        lParam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
        return DefWindowProcW(window, message, wParam, lParam);

    case WM_IME_STARTCOMPOSITION:
        UpdateImeWindows();
        return 0;

    case WM_IME_COMPOSITION: {
        HIMC context = ImmGetContext(window);
        if (!context) return DefWindowProcW(window, message, wParam, lParam);
        if (lParam & GCS_RESULTSTR) {
            const std::wstring result = CompositionString(context, GCS_RESULTSTR);
            SendPreedit({}, -1);
            if (!result.empty()) SendText(Narrow(result));
        }
        if (lParam & GCS_COMPSTR) {
            const std::wstring composition = CompositionString(context, GCS_COMPSTR);
            const LONG cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
            SendPreedit(composition, cursor);
        }
        ImmReleaseContext(window, context);
        UpdateImeWindows();
        return 0;
    }

    case WM_IME_ENDCOMPOSITION:
        SendPreedit({}, -1);
        return 0;

    case WM_MOUSEMOVE: {
        if (!trackingLeave_) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            trackingLeave_ = TrackMouseEvent(&track) != FALSE;
        }
        lastX_ = ToLogical(GET_X_LPARAM(lParam));
        lastY_ = ToLogical(GET_Y_LPARAM(lParam));
        InputEvent event;
        event.type = InputEvent::Type::PointerMove;
        event.x = lastX_;
        event.y = lastY_;
        event.mods = CurrentModifiers();
        Deliver(event);
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        if (buttonsDown_ == 0) {
            InputEvent event;
            event.type = InputEvent::Type::PointerLeave;
            Deliver(event);
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        MouseButton button = MouseButton::Left;
        if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) button = MouseButton::Right;
        if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) button = MouseButton::Middle;
        const bool pressed = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
        const unsigned bit = 1u << static_cast<unsigned>(button);
        lastX_ = ToLogical(GET_X_LPARAM(lParam));
        lastY_ = ToLogical(GET_Y_LPARAM(lParam));
        if (pressed) {
            if (buttonsDown_ == 0) SetCapture(window);
            buttonsDown_ |= bit;
        } else if ((buttonsDown_ & bit) == 0) {
            return 0;  // the press went elsewhere (or was released for us on capture loss)
        }
        InputEvent event;
        event.type = InputEvent::Type::Button;
        event.x = lastX_;
        event.y = lastY_;
        event.button = button;
        event.pressed = pressed;
        event.mods = CurrentModifiers();
        Deliver(event);
        if (!pressed) {
            buttonsDown_ &= ~bit;
            if (buttonsDown_ == 0) {
                ReleaseCapture();
                // Re-arm leave tracking: it fires at once if the drag ended outside the window.
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                trackingLeave_ = TrackMouseEvent(&track) != FALSE;
            }
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window) ReleaseButtons();
        return 0;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};  // screen coordinates
        ScreenToClient(window, &point);
        const float notches = GET_WHEEL_DELTA_WPARAM(wParam) / static_cast<float>(WHEEL_DELTA);
        InputEvent event;
        event.type = InputEvent::Type::Wheel;
        event.x = ToLogical(point.x);
        event.y = ToLogical(point.y);
        // Windows: a positive vertical delta rolls away from the user (content up), a positive
        // horizontal one tilts right (content right).
        if (message == WM_MOUSEWHEEL) {
            event.notchesY = -notches;
        } else {
            event.notchesX = notches;
        }
        event.mods = CurrentModifiers();
        Deliver(event);
        return message == WM_MOUSEHWHEEL ? TRUE : 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            ::SetCursor(CursorHandle(cursor_));
            return TRUE;
        }
        return DefWindowProcW(window, message, wParam, lParam);

    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wParam);
        std::string path;
        if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) > 0) {
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            if (length > 0) {
                std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
                const UINT got = DragQueryFileW(drop, 0, wide.data(), length + 1);
                wide.resize(std::min<size_t>(got, length));
                if (!NarrowPath(wide, &path)) {
                    path.clear();
                    if (client_ && started_) {
                        client_->OnPlatformError("The dropped file's name is not valid Unicode; RayoMD cannot open it");
                    }
                }
            }
        }
        DragFinish(drop);
        if (!path.empty() && client_ && started_) client_->OnDropFile(std::move(path));
        return 0;
    }

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace rgui::win32
