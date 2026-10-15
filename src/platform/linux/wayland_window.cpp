// SPDX-License-Identifier: Apache-2.0
#include "platform/linux/wayland_window.h"

#include "gfx/canvas.h"
#include "platform/linux/compose.h"
#include "platform/linux/data_transfer.h"
#include "platform/linux/loop.h"
#include "platform/linux/ui_tasks.h"
#include "platform/linux/wl_protocols.h"
#include "platform/linux/wl_wire.h"
#include "platform/linux/xkb_keymap.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <map>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace rgui::wl {

namespace {

constexpr int kMinWidth = 720;
constexpr int kMinHeight = 520;
constexpr int kMaxLogical = 16384;
constexpr uint32_t kMaxKeymapBytes = 8u << 20;
constexpr size_t kMaxDamageRects = 32;
constexpr int kCursorGrid = 24;  // logical pixels of a built-in cursor
constexpr uint32_t kBtnLeft = 0x110;
constexpr uint32_t kBtnRight = 0x111;
constexpr uint32_t kBtnMiddle = 0x112;

rgui_wl_arg U(uint32_t v) {
    rgui_wl_arg a;
    a.u = v;
    return a;
}
rgui_wl_arg I(int32_t v) {
    rgui_wl_arg a;
    a.i = v;
    return a;
}
rgui_wl_arg S(const char* v) {
    rgui_wl_arg a;
    a.s = v;
    return a;
}
rgui_wl_arg O(uint32_t v) {
    rgui_wl_arg a;
    a.o = v;
    return a;
}
rgui_wl_arg H(int v) {
    rgui_wl_arg a;
    a.h = v;
    return a;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return;
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string PathFromUriList(const std::string& list) {
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        std::string line = list.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 7, "file://") != 0) continue;
        size_t pathStart = 7;
        if (pathStart < line.size() && line[pathStart] != '/') {  // file://host/path
            const size_t slash = line.find('/', pathStart);
            if (slash == std::string::npos) continue;
            const std::string host = line.substr(pathStart, slash - pathStart);
            if (host != "localhost") {
                char name[256] = {};
                if (gethostname(name, sizeof name - 1) != 0 || host != name) continue;
            }
            pathStart = slash;
        }
        std::string path;
        for (size_t i = pathStart; i < line.size(); ++i) {
            if (line[i] == '%' && i + 2 < line.size() && HexValue(line[i + 1]) >= 0 && HexValue(line[i + 2]) >= 0) {
                const char c = static_cast<char>(HexValue(line[i + 1]) * 16 + HexValue(line[i + 2]));
                if (c == 0) {
                    path.clear();
                    break;
                }
                path += c;
                i += 2;
            } else {
                path += line[i];
            }
        }
        if (!path.empty()) return path;
    }
    return {};
}

namespace {

// Cuts UTF-8 text to at most `limit` bytes without splitting a character.
std::string TruncateUtf8(std::string_view text, size_t limit) {
    if (text.size() <= limit) return std::string(text);
    size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return std::string(text.substr(0, end));
}

std::filesystem::path HomeDir() {
    const char* home = getenv("HOME");
    return home && *home ? std::filesystem::path(home) : std::filesystem::path("/");
}

std::filesystem::path XdgBase(const char* variable, const char* fallback) {
    const char* value = getenv(variable);
    if (value && value[0] == '/') return value;
    return HomeDir() / fallback;
}

} // namespace

std::filesystem::path XdgConfigDir() { return XdgBase("XDG_CONFIG_HOME", ".config") / "rayomd-gui"; }

std::filesystem::path XdgImageCacheDir() { return XdgBase("XDG_CACHE_HOME", ".cache") / "rayomd" / "images"; }

std::filesystem::path XdgDocumentsDir() {
    const std::filesystem::path file = XdgBase("XDG_CONFIG_HOME", ".config") / "user-dirs.dirs";
    std::string text;
    if (FILE* f = fopen(file.c_str(), "rb")) {
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0 && text.size() < 65536) text.append(buffer, n);
        fclose(f);
    }
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + at, end - at);
        at = end + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        constexpr std::string_view kKey = "XDG_DOCUMENTS_DIR=";
        if (line.substr(0, kKey.size()) != kKey) continue;
        line.remove_prefix(kKey.size());
        if (line.size() < 2 || line.front() != '"') continue;
        const size_t close = line.find('"', 1);
        if (close == std::string_view::npos) continue;
        std::string_view value = line.substr(1, close - 1);
        if (value.substr(0, 5) == "$HOME") {
            value.remove_prefix(5);
            while (!value.empty() && value.front() == '/') value.remove_prefix(1);
            if (value.empty()) return HomeDir();
            return HomeDir() / std::string(value);
        }
        if (!value.empty() && value.front() == '/') return std::string(value);
    }
    return HomeDir() / "Documents";
}

namespace {

class WaylandWindowImpl;

using Handler = void (WaylandWindowImpl::*)(uint32_t id, uint16_t opcode, const rgui_wl_arg* args);

class WaylandWindowImpl final : public WaylandWindow {
public:
    ~WaylandWindowImpl() override;
    bool Init(const WindowOptions& options);

    // WaylandWindow
    void SetClient(PlatformClient* client) override { client_ = client; }
    void SetHooks(WindowHooks hooks) override { hooks_ = std::move(hooks); }
    bool ConnectionLost() const override { return lost_; }
    int LogicalWidth() const override { return logicalW_; }
    int LogicalHeight() const override { return logicalH_; }
    bool Maximized() const override { return maximized_; }
    std::vector<AdvertisedGlobal> Globals() const override { return advertised_; }

    // Platform
    void SetTitle(std::string_view title) override;
    void SetCursor(Cursor cursor) override;
    void SetTextInputRect(gfx::RectI rect, bool enabled) override;
    void RequestFrame() override;
    void Present(const gfx::Surface& frame, const std::vector<gfx::RectI>& damage) override;
    bool NeedsClientDecorations() const override { return !decoration_ || clientSideDecorations_; }
    bool IsMaximized() const override { return maximized_; }
    void StartMove() override;
    void StartResize(Edge edge) override;
    void ToggleMaximize() override;
    void Minimize() override;
    void SetClipboardText(std::string text) override;
    void RequestClipboardText(std::function<void(std::string)> callback) override;
    void ShowFileDialog(FileDialog kind, std::string suggestedPath,
                        std::function<void(FileDialogResult)> callback) override;
    void OpenExternally(const std::string& path) override;
    void PostToUi(std::function<void()> task) override { tasks_.Post(std::move(task)); }
    uint64_t StartTimer(int ms, std::function<void()> callback) override;
    void CancelTimer(uint64_t id) override;
    void Quit() override { rgui_loop_quit(); }
    std::filesystem::path ConfigDir() const override { return XdgConfigDir(); }
    std::filesystem::path ImageCacheDir() const override { return XdgImageCacheDir(); }
    std::filesystem::path DocumentsDir() const override { return XdgDocumentsDir(); }
    std::optional<std::filesystem::path> OmarchyThemeDir() const override {
        return hooks_.omarchyThemeDir ? hooks_.omarchyThemeDir() : std::nullopt;
    }
    std::optional<std::filesystem::path> OmarchyUserShellToml() const override {
        return hooks_.omarchyUserShellToml ? hooks_.omarchyUserShellToml() : std::nullopt;
    }

private:
    template <Handler H>
    static void Trampoline(void* user, uint32_t id, uint16_t opcode, const rgui_wl_arg* args) {
        (static_cast<WaylandWindowImpl*>(user)->*H)(id, opcode, args);
    }

    // Requests.
    bool Send(uint32_t id, uint16_t opcode, std::initializer_list<rgui_wl_arg> args = {});
    // A new object made by `opcode` of `parent` (its 'n' first, then `args`), with the parent's
    // version. 0 on failure.
    uint32_t Create(uint32_t parent, uint16_t opcode, const rgui_wl_interface* iface, rgui_wl_dispatcher dispatcher,
                    std::initializer_list<rgui_wl_arg> args = {});
    uint32_t Bind(uint32_t name, const rgui_wl_interface* iface, uint32_t version, rgui_wl_dispatcher dispatcher);
    void Fail(const char* what);

    // The connection and the loop.
    static void OnSocket(int fd, uint32_t revents, void* user);
    static void OnPrepare(void* user);
    void Flush();

    // Events.
    void OnRegistry(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnSeat(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnOutput(uint32_t id, uint16_t opcode, const rgui_wl_arg* args);
    void OnSurface(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnWmBase(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnXdgSurface(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnToplevel(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnDecoration(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnFractionalScale(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnFrameDone(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnBuffer(uint32_t id, uint16_t opcode, const rgui_wl_arg* args);
    void OnPointer(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnKeyboard(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnTextInput(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnDataDevice(uint32_t, uint16_t opcode, const rgui_wl_arg* args);
    void OnDataOffer(uint32_t id, uint16_t opcode, const rgui_wl_arg* args);
    void OnDataSource(uint32_t id, uint16_t opcode, const rgui_wl_arg* args);

    // Geometry, buffers and frames.
    void UpdateGeometry();
    bool EnsureBuffers(int width, int height);
    void DestroyBuffers();
    int FreeBuffer() const;
    int WaitForFreeBuffer();
    void ScheduleFrame();
    static void OnFrameTimer(uint64_t id, void* user);

    // Input.
    void Emit(const InputEvent& event) {
        if (client_) client_->OnInput(event);
    }
    uint8_t Mods() const { return keymap_ ? keymap_->Modifiers() : 0; }
    void ApplyCursor();
    bool EnsureCursorImages(int scale);
    void KeyPressed(uint32_t key, bool repeat);
    void StopRepeat();
    static void OnRepeatTimer(uint64_t id, void* user);
    void UpdateTextInput();

    // Clipboard and drops.
    void DestroyOffer(uint32_t id);
    void DropStaleOffers();
    // Asks `offer` for `mime` through a pipe; a paste replaces the paste still in flight.
    void Receive(uint32_t offer, const char* mime, bool paste, std::function<void(std::string)> done);

    static void OnUserTimer(uint64_t id, void* user);

    rgui_wl_connection* conn_ = nullptr;
    PlatformClient* client_ = nullptr;
    WindowHooks hooks_;
    UiTaskQueue tasks_;
    bool lost_ = false;
    bool watchingWrite_ = false;

    // Globals.
    std::vector<AdvertisedGlobal> advertised_;
    uint32_t registry_ = 0;
    uint32_t compositor_ = 0;
    uint32_t shm_ = 0;
    uint32_t seat_ = 0;
    uint32_t dataDeviceManager_ = 0;
    uint32_t wmBase_ = 0;
    uint32_t decorationManager_ = 0;
    uint32_t cursorShapeManager_ = 0;
    uint32_t viewporter_ = 0;
    uint32_t fractionalManager_ = 0;
    uint32_t textInputManager_ = 0;
    std::string tooOld_;  // a required global advertised below its minimum version
    struct Output {
        uint32_t id = 0;
        uint32_t name = 0;
        int scale = 1;
        int pendingScale = 1;
    };
    std::vector<Output> outputs_;

    // The window.
    uint32_t surface_ = 0;
    uint32_t xdgSurface_ = 0;
    uint32_t toplevel_ = 0;
    uint32_t decoration_ = 0;
    uint32_t viewport_ = 0;
    uint32_t fractional_ = 0;
    bool clientSideDecorations_ = false;
    int logicalW_ = 1280;
    int logicalH_ = 820;
    bool maximized_ = false;
    int pendingW_ = 0;
    int pendingH_ = 0;
    bool pendingMaximized_ = false;
    bool configured_ = false;
    bool mapped_ = false;
    std::vector<uint32_t> enteredOutputs_;
    int preferredBufferScale_ = 0;   // wl_surface.preferred_buffer_scale, 0 until sent
    uint32_t preferredScale120_ = 0; // wp_fractional_scale_v1.preferred_scale, 0 until sent
    int deviceW_ = 0;
    int deviceH_ = 0;
    double scale_ = 0;
    int bufferScale_ = 1;            // set_buffer_scale in effect
    int appliedBufferScale_ = 1;     // what the compositor has
    int appliedDestW_ = 0;
    int appliedDestH_ = 0;

    // Buffers.
    struct Buffer {
        uint32_t id = 0;
        bool busy = false;
        uint32_t* pixels = nullptr;
        std::vector<gfx::RectI> damage;  // changed since this buffer was last written
    };
    Buffer buffers_[3];
    std::vector<uint32_t> retiredBuffers_;  // busy buffers of an old size, destroyed when released
    void* map_ = nullptr;
    size_t mapSize_ = 0;
    int bufferW_ = 0;
    int bufferH_ = 0;
    bool fullDamage_ = false;

    // Frames.
    bool frameRequested_ = false;
    uint32_t frameCallback_ = 0;
    uint64_t frameTimer_ = 0;
    uint64_t lastFrameMs_ = 0;
    bool presentedSinceFrame_ = true;  // the last OnFrame presented: the frame callback paces

    // Pointer.
    uint32_t pointer_ = 0;
    uint32_t cursorShapeDevice_ = 0;
    uint32_t pointerSerial_ = 0;  // of the last wl_pointer.enter
    bool pointerInside_ = false;
    float pointerX_ = 0;
    float pointerY_ = 0;
    Cursor cursor_ = Cursor::Arrow;
    double axisValue_[2] = {0, 0};
    bool axisSeen_[2] = {false, false};
    int32_t axisValue120_[2] = {0, 0};
    bool axisValue120Seen_[2] = {false, false};
    uint32_t axisSource_ = UINT32_MAX;
    // Built-in cursors (no cursor-shape): arrow and I-beam in one memfd.
    uint32_t cursorSurface_ = 0;
    uint32_t cursorBuffers_[2] = {0, 0};
    int cursorScale_ = 0;
    int shownCursor_ = -1;  // which built-in cursor the cursor surface holds

    // Keyboard.
    uint32_t keyboard_ = 0;
    std::unique_ptr<xkb::Keymap> keymap_;
    xkb::Compose compose_;
    int32_t repeatRate_ = 25;
    int32_t repeatDelay_ = 600;
    uint32_t repeatKey_ = 0;
    uint64_t repeatTimer_ = 0;
    uint32_t inputSerial_ = 0;          // of the last key or button press
    uint32_t keyboardEnterSerial_ = 0;  // set_selection's serial before any press

    // Text input.
    uint32_t textInput_ = 0;
    bool textInputEntered_ = false;
    bool textInputEnabled_ = false;
    bool appWantsTextInput_ = false;
    gfx::RectI textInputRect_;
    std::string pendingPreedit_;
    int pendingPreeditCursor_ = -1;
    std::string pendingCommit_;
    std::string preedit_;

    // Clipboard and drops.
    uint32_t dataDevice_ = 0;
    struct Offer {
        std::vector<std::string> mimes;
        uint32_t sourceActions = 0;
        uint32_t action = 0;
        bool transferring = false;
    };
    std::map<uint32_t, Offer> offers_;
    uint32_t selectionOffer_ = 0;
    uint32_t dndOffer_ = 0;
    uint32_t selectionSource_ = 0;
    std::shared_ptr<const std::string> selectionText_;
    Transfers transfers_;

    // Timers.
    std::unordered_map<uint64_t, std::function<void()>> timers_;
};

// --- requests ---------------------------------------------------------------------------------

void WaylandWindowImpl::Fail(const char* what) {
    if (lost_) return;
    lost_ = true;
    fprintf(stderr, "rayomd-gui: %s: %s\n", what, conn_ ? rgui_wl_connection_error(conn_) : "");
    if (conn_) rgui_loop_remove_fd(rgui_wl_connection_fd(conn_));
    rgui_loop_quit();
}

bool WaylandWindowImpl::Send(uint32_t id, uint16_t opcode, std::initializer_list<rgui_wl_arg> args) {
    if (lost_ || !id) return false;
    if (rgui_wl_send(conn_, id, opcode, args.begin()) != 0) {
        fprintf(stderr, "rayomd-gui: Wayland request failed: %s\n", rgui_wl_connection_error(conn_));
        return false;
    }
    return true;
}

uint32_t WaylandWindowImpl::Create(uint32_t parent, uint16_t opcode, const rgui_wl_interface* iface,
                                   rgui_wl_dispatcher dispatcher, std::initializer_list<rgui_wl_arg> args) {
    if (lost_ || !parent) return 0;
    const uint32_t version = rgui_wl_object_version(conn_, parent);
    const uint32_t id = rgui_wl_new_object(conn_, iface, version, dispatcher, this);
    if (!id) return 0;
    rgui_wl_arg all[RGUI_WL_MAX_ARGS];
    all[0].n = id;
    size_t n = 1;
    for (const rgui_wl_arg& a : args) {
        if (n < RGUI_WL_MAX_ARGS) all[n++] = a;
    }
    if (rgui_wl_send(conn_, parent, opcode, all) != 0) {
        fprintf(stderr, "rayomd-gui: Wayland request failed: %s\n", rgui_wl_connection_error(conn_));
        return 0;
    }
    return id;
}

uint32_t WaylandWindowImpl::Bind(uint32_t name, const rgui_wl_interface* iface, uint32_t version,
                                 rgui_wl_dispatcher dispatcher) {
    const uint32_t id = rgui_wl_new_object(conn_, iface, version, dispatcher, this);
    if (!id) return 0;
    if (!Send(registry_, WL_REGISTRY_R_BIND, {U(name), S(iface->name), U(version), U(id)})) return 0;
    return id;
}

// --- setup ------------------------------------------------------------------------------------

bool WaylandWindowImpl::Init(const WindowOptions& options) {
    logicalW_ = std::clamp(options.width, kMinWidth, kMaxLogical);
    logicalH_ = std::clamp(options.height, kMinHeight, kMaxLogical);
    char error[512];
    const int fd = rgui_wl_connect_socket(error, sizeof error);
    if (fd < 0) {
        fprintf(stderr, "rayomd-gui: no Wayland compositor: %s\n", error);
        return false;
    }
    conn_ = rgui_wl_connection_create(fd, &rgui_wl_display_interface);
    if (!conn_) {
        close(fd);
        fprintf(stderr, "rayomd-gui: out of memory\n");
        return false;
    }
    registry_ = Create(RGUI_WL_DISPLAY_ID, WL_DISPLAY_R_GET_REGISTRY, &rgui_wl_registry_interface,
                       &Trampoline<&WaylandWindowImpl::OnRegistry>);
    if (!registry_ || rgui_wl_roundtrip(conn_, &rgui_wl_callback_interface, 5000) != 0) {
        fprintf(stderr, "rayomd-gui: the Wayland compositor does not answer: %s\n", rgui_wl_connection_error(conn_));
        return false;
    }
    const struct {
        uint32_t id;
        rgui_wl_global global;
    } required[] = {{compositor_, RGUI_WL_GLOBAL_COMPOSITOR},
                    {shm_, RGUI_WL_GLOBAL_SHM},
                    {seat_, RGUI_WL_GLOBAL_SEAT},
                    {wmBase_, RGUI_WL_GLOBAL_XDG_WM_BASE}};
    for (const auto& r : required) {
        if (r.id) continue;
        const rgui_wl_global_spec& spec = rgui_wl_global_specs[r.global];
        if (spec.min_version > 1) {
            fprintf(stderr, "rayomd-gui: the compositor lacks %s (version %u or newer)\n", spec.iface->name,
                    spec.min_version);
        } else {
            fprintf(stderr, "rayomd-gui: the compositor lacks %s\n", spec.iface->name);
        }
        return false;
    }
    if (dataDeviceManager_) {
        dataDevice_ = Create(dataDeviceManager_, WL_DATA_DEVICE_MANAGER_R_GET_DATA_DEVICE, &rgui_wl_data_device_interface,
                             &Trampoline<&WaylandWindowImpl::OnDataDevice>, {O(seat_)});
    }
    if (textInputManager_) {
        textInput_ = Create(textInputManager_, ZWP_TEXT_INPUT_MANAGER_V3_R_GET_TEXT_INPUT,
                            &rgui_zwp_text_input_v3_interface, &Trampoline<&WaylandWindowImpl::OnTextInput>,
                            {O(seat_)});
    }
    // Seat capabilities, output scales.
    if (rgui_wl_roundtrip(conn_, &rgui_wl_callback_interface, 5000) != 0) {
        fprintf(stderr, "rayomd-gui: the Wayland compositor does not answer: %s\n", rgui_wl_connection_error(conn_));
        return false;
    }

    surface_ = Create(compositor_, WL_COMPOSITOR_R_CREATE_SURFACE, &rgui_wl_surface_interface,
                      &Trampoline<&WaylandWindowImpl::OnSurface>);
    if (fractionalManager_ && viewporter_) {
        viewport_ = Create(viewporter_, WP_VIEWPORTER_R_GET_VIEWPORT, &rgui_wp_viewport_interface, nullptr,
                           {O(surface_)});
        fractional_ = Create(fractionalManager_, WP_FRACTIONAL_SCALE_MANAGER_V1_R_GET_FRACTIONAL_SCALE,
                             &rgui_wp_fractional_scale_v1_interface,
                             &Trampoline<&WaylandWindowImpl::OnFractionalScale>, {O(surface_)});
    }
    xdgSurface_ = Create(wmBase_, XDG_WM_BASE_R_GET_XDG_SURFACE, &rgui_xdg_surface_interface,
                         &Trampoline<&WaylandWindowImpl::OnXdgSurface>, {O(surface_)});
    toplevel_ = Create(xdgSurface_, XDG_SURFACE_R_GET_TOPLEVEL, &rgui_xdg_toplevel_interface,
                       &Trampoline<&WaylandWindowImpl::OnToplevel>);
    Send(toplevel_, XDG_TOPLEVEL_R_SET_APP_ID, {S("rayomd-gui")});
    SetTitle(options.title);
    Send(toplevel_, XDG_TOPLEVEL_R_SET_MIN_SIZE, {I(kMinWidth), I(kMinHeight)});
    if (decorationManager_) {
        decoration_ = Create(decorationManager_, ZXDG_DECORATION_MANAGER_V1_R_GET_TOPLEVEL_DECORATION,
                             &rgui_zxdg_toplevel_decoration_v1_interface,
                             &Trampoline<&WaylandWindowImpl::OnDecoration>, {O(toplevel_)});
        Send(decoration_, ZXDG_TOPLEVEL_DECORATION_V1_R_SET_MODE, {U(ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)});
    }
    if (options.maximized) Send(toplevel_, XDG_TOPLEVEL_R_SET_MAXIMIZED);
    Send(surface_, WL_SURFACE_R_COMMIT);
    if (!surface_ || !xdgSurface_ || !toplevel_ || rgui_wl_flush_blocking(conn_, 5000) != 0) {
        fprintf(stderr, "rayomd-gui: cannot create the window: %s\n", rgui_wl_connection_error(conn_));
        return false;
    }
    if (rgui_loop_add_fd(rgui_wl_connection_fd(conn_), POLLIN, OnSocket, this) != 0 ||
        rgui_loop_add_prepare(OnPrepare, this) != 0) {
        fprintf(stderr, "rayomd-gui: the event loop is not initialised\n");
        return false;
    }
    return true;
}

WaylandWindowImpl::~WaylandWindowImpl() {
    rgui_loop_remove_prepare(OnPrepare, this);
    if (conn_) rgui_loop_remove_fd(rgui_wl_connection_fd(conn_));
    if (frameTimer_) rgui_loop_cancel_timer(frameTimer_);
    if (repeatTimer_) rgui_loop_cancel_timer(repeatTimer_);
    for (const auto& t : timers_) rgui_loop_cancel_timer(t.first);
    if (map_) munmap(map_, mapSize_);
    if (conn_) {
        rgui_wl_flush(conn_);
        rgui_wl_connection_destroy(conn_);  // closing the socket destroys every object
    }
}

// --- the connection ---------------------------------------------------------------------------

void WaylandWindowImpl::OnSocket(int, uint32_t revents, void* user) {
    auto* self = static_cast<WaylandWindowImpl*>(user);
    if (self->lost_) return;
    if (revents & POLLOUT) self->Flush();
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
        const int r = rgui_wl_read(self->conn_);
        if (r < 0) {
            self->Fail("lost the Wayland connection");
            return;
        }
        if (rgui_wl_dispatch(self->conn_) < 0) self->Fail("bad message from the compositor");
    }
}

void WaylandWindowImpl::OnPrepare(void* user) { static_cast<WaylandWindowImpl*>(user)->Flush(); }

void WaylandWindowImpl::Flush() {
    if (lost_) return;
    const int r = rgui_wl_flush(conn_);
    if (r < 0) {
        Fail("lost the Wayland connection");
        return;
    }
    const bool wantWrite = r == 1;
    if (wantWrite != watchingWrite_) {
        rgui_loop_set_fd_events(rgui_wl_connection_fd(conn_), wantWrite ? (POLLIN | POLLOUT) : POLLIN);
        watchingWrite_ = wantWrite;
    }
}

// --- globals ----------------------------------------------------------------------------------

void WaylandWindowImpl::OnRegistry(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode == WL_REGISTRY_E_GLOBAL_REMOVE) {
        const uint32_t name = args[0].u;
        for (size_t i = 0; i < outputs_.size(); ++i) {
            if (outputs_[i].name != name) continue;
            const uint32_t id = outputs_[i].id;
            rgui_wl_set_dispatcher(conn_, id, nullptr, nullptr);
            outputs_.erase(outputs_.begin() + static_cast<ptrdiff_t>(i));
            enteredOutputs_.erase(std::remove(enteredOutputs_.begin(), enteredOutputs_.end(), id),
                                  enteredOutputs_.end());
            UpdateGeometry();
            break;
        }
        return;
    }
    if (opcode != WL_REGISTRY_E_GLOBAL) return;
    const uint32_t name = args[0].u;
    const char* interface = args[1].s;
    const uint32_t advertised = args[2].u;
    AdvertisedGlobal info{interface ? interface : "", advertised, 0};
    const int global = rgui_wl_global_from_name(interface);
    if (global >= 0) {
        const auto g = static_cast<rgui_wl_global>(global);
        const uint32_t version = rgui_wl_global_bind_version(g, advertised);
        const rgui_wl_interface* iface = rgui_wl_global_specs[g].iface;
        uint32_t* slot = nullptr;
        rgui_wl_dispatcher dispatcher = nullptr;
        switch (g) {
        case RGUI_WL_GLOBAL_COMPOSITOR: slot = &compositor_; break;
        case RGUI_WL_GLOBAL_SHM: slot = &shm_; break;
        case RGUI_WL_GLOBAL_SEAT:
            slot = &seat_;
            dispatcher = &Trampoline<&WaylandWindowImpl::OnSeat>;
            break;
        case RGUI_WL_GLOBAL_DATA_DEVICE_MANAGER: slot = &dataDeviceManager_; break;
        case RGUI_WL_GLOBAL_XDG_WM_BASE:
            slot = &wmBase_;
            dispatcher = &Trampoline<&WaylandWindowImpl::OnWmBase>;
            break;
        case RGUI_WL_GLOBAL_DECORATION_MANAGER: slot = &decorationManager_; break;
        case RGUI_WL_GLOBAL_CURSOR_SHAPE_MANAGER: slot = &cursorShapeManager_; break;
        case RGUI_WL_GLOBAL_VIEWPORTER: slot = &viewporter_; break;
        case RGUI_WL_GLOBAL_FRACTIONAL_SCALE_MANAGER: slot = &fractionalManager_; break;
        case RGUI_WL_GLOBAL_TEXT_INPUT_MANAGER: slot = &textInputManager_; break;
        case RGUI_WL_GLOBAL_OUTPUT:
        case RGUI_WL_GLOBAL_COUNT: break;
        }
        if (version && g == RGUI_WL_GLOBAL_OUTPUT) {
            Output output;
            output.id = Bind(name, iface, version, &Trampoline<&WaylandWindowImpl::OnOutput>);
            output.name = name;
            if (output.id) {
                outputs_.push_back(output);
                info.bound = version;
            }
        } else if (version && slot && *slot == 0) {
            // Bound once, during startup (a later global of the same kind is not used).
            *slot = Bind(name, iface, version, dispatcher);
            if (*slot) info.bound = version;
        }
    }
    advertised_.push_back(std::move(info));
}

void WaylandWindowImpl::OnWmBase(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode == XDG_WM_BASE_E_PING) Send(wmBase_, XDG_WM_BASE_R_PONG, {U(args[0].u)});
}

void WaylandWindowImpl::OnSeat(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode != WL_SEAT_E_CAPABILITIES) return;
    const uint32_t caps = args[0].u;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer_) {
        pointer_ = Create(seat_, WL_SEAT_R_GET_POINTER, &rgui_wl_pointer_interface,
                          &Trampoline<&WaylandWindowImpl::OnPointer>);
        if (pointer_ && cursorShapeManager_) {
            cursorShapeDevice_ = Create(cursorShapeManager_, WP_CURSOR_SHAPE_MANAGER_V1_R_GET_POINTER,
                                        &rgui_wp_cursor_shape_device_v1_interface, nullptr, {O(pointer_)});
        }
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer_) {
        // The pointer went away: its object is forgotten (no release in seat version < 3 terms).
        rgui_wl_set_dispatcher(conn_, pointer_, nullptr, nullptr);
        pointer_ = 0;
        cursorShapeDevice_ = 0;
        pointerInside_ = false;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard_) {
        keyboard_ = Create(seat_, WL_SEAT_R_GET_KEYBOARD, &rgui_wl_keyboard_interface,
                           &Trampoline<&WaylandWindowImpl::OnKeyboard>);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard_) {
        rgui_wl_set_dispatcher(conn_, keyboard_, nullptr, nullptr);
        keyboard_ = 0;
        StopRepeat();
    }
}

void WaylandWindowImpl::OnOutput(uint32_t id, uint16_t opcode, const rgui_wl_arg* args) {
    for (Output& output : outputs_) {
        if (output.id != id) continue;
        if (opcode == WL_OUTPUT_E_SCALE) {
            output.pendingScale = std::clamp(args[0].i, 1, 8);
        } else if (opcode == WL_OUTPUT_E_DONE) {
            if (output.scale != output.pendingScale) {
                output.scale = output.pendingScale;
                UpdateGeometry();
            }
        }
        return;
    }
}

// --- the surface ------------------------------------------------------------------------------

void WaylandWindowImpl::OnSurface(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    switch (opcode) {
    case WL_SURFACE_E_ENTER:
        if (std::find(enteredOutputs_.begin(), enteredOutputs_.end(), args[0].o) == enteredOutputs_.end()) {
            enteredOutputs_.push_back(args[0].o);
        }
        UpdateGeometry();
        break;
    case WL_SURFACE_E_LEAVE:
        enteredOutputs_.erase(std::remove(enteredOutputs_.begin(), enteredOutputs_.end(), args[0].o),
                              enteredOutputs_.end());
        UpdateGeometry();
        break;
    case WL_SURFACE_E_PREFERRED_BUFFER_SCALE:
        preferredBufferScale_ = std::clamp(args[0].i, 1, 8);
        UpdateGeometry();
        break;
    default: break;
    }
}

void WaylandWindowImpl::OnFractionalScale(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode != WP_FRACTIONAL_SCALE_V1_E_PREFERRED_SCALE) return;
    preferredScale120_ = std::clamp<uint32_t>(args[0].u, 30, 120 * 8);
    UpdateGeometry();
}

void WaylandWindowImpl::OnToplevel(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode == XDG_TOPLEVEL_E_CLOSE) {
        if (client_) client_->OnCloseRequest();
        return;
    }
    if (opcode != XDG_TOPLEVEL_E_CONFIGURE) return;
    pendingW_ = args[0].i;
    pendingH_ = args[1].i;
    pendingMaximized_ = false;
    const uint32_t count = args[2].a.size / 4;
    const auto* states = static_cast<const uint8_t*>(args[2].a.data);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t state;
        std::memcpy(&state, states + 4 * i, 4);
        if (state == XDG_TOPLEVEL_STATE_MAXIMIZED) pendingMaximized_ = true;
    }
}

void WaylandWindowImpl::OnXdgSurface(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode != XDG_SURFACE_E_CONFIGURE) return;
    Send(xdgSurface_, XDG_SURFACE_R_ACK_CONFIGURE, {U(args[0].u)});
    if (pendingW_ > 0 && pendingH_ > 0) {  // zero: the window keeps its own size
        logicalW_ = std::min(pendingW_, kMaxLogical);
        logicalH_ = std::min(pendingH_, kMaxLogical);
    }
    maximized_ = pendingMaximized_;
    const bool first = !configured_;
    configured_ = true;
    const int oldW = deviceW_;
    const int oldH = deviceH_;
    const double oldScale = scale_;
    UpdateGeometry();
    const bool resized = first || oldW != deviceW_ || oldH != deviceH_ || oldScale != scale_;
    if (!resized && mapped_) Send(surface_, WL_SURFACE_R_COMMIT);  // applies the ack, same buffer
    if (resized) RequestFrame();
}

void WaylandWindowImpl::OnDecoration(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode != ZXDG_TOPLEVEL_DECORATION_V1_E_CONFIGURE) return;
    const bool csd = args[0].u == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    if (csd != clientSideDecorations_) {
        clientSideDecorations_ = csd;
        RequestFrame();
    }
}

void WaylandWindowImpl::UpdateGeometry() {
    if (!configured_) return;
    int integerScale = preferredBufferScale_;
    if (integerScale == 0) {
        // The largest scale of the outputs the surface is on; before it is on any (the first
        // frame), of every output, so a single HiDPI screen does not start blurry.
        integerScale = 1;
        for (const Output& output : outputs_) {
            const bool entered =
                std::find(enteredOutputs_.begin(), enteredOutputs_.end(), output.id) != enteredOutputs_.end();
            if (entered || enteredOutputs_.empty()) integerScale = std::max(integerScale, output.scale);
        }
    }
    int w, h, bufferScale;
    double scale;
    if (viewport_) {
        // Fractional: the buffer is round(logical * scale / 120) and the viewport maps it back.
        const uint32_t scale120 = FitScale120(
            logicalW_, logicalH_, preferredScale120_ ? preferredScale120_ : static_cast<uint32_t>(integerScale) * 120,
            false);
        w = static_cast<int>((static_cast<uint64_t>(logicalW_) * scale120 + 60) / 120);
        h = static_cast<int>((static_cast<uint64_t>(logicalH_) * scale120 + 60) / 120);
        scale = scale120 / 120.0;
        bufferScale = 1;
    } else {
        integerScale = static_cast<int>(FitScale120(logicalW_, logicalH_, static_cast<uint32_t>(integerScale) * 120, true) / 120);
        w = logicalW_ * integerScale;
        h = logicalH_ * integerScale;
        scale = integerScale;
        bufferScale = integerScale;
    }
    w = std::max(w, 1);
    h = std::max(h, 1);
    if (w == deviceW_ && h == deviceH_ && scale == scale_ && bufferScale == bufferScale_) return;
    deviceW_ = w;
    deviceH_ = h;
    scale_ = scale;
    bufferScale_ = bufferScale;
    if (client_) client_->OnResize(deviceW_, deviceH_, scale_);
    if (cursorScale_ && !cursorShapeDevice_) ApplyCursor();
    RequestFrame();
}

// --- buffers and frames -----------------------------------------------------------------------

bool WaylandWindowImpl::EnsureBuffers(int width, int height) {
    if (buffers_[0].id && bufferW_ == width && bufferH_ == height) return true;
    DestroyBuffers();
    const size_t stride = static_cast<size_t>(width) * 4;
    const size_t one = stride * static_cast<size_t>(height);
    const size_t total = one * 3;
    if (width <= 0 || height <= 0 || total > 0x7fffffffu) return false;
    const int fd = memfd_create("rayomd-gui", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, static_cast<off_t>(total)) != 0) {
        close(fd);
        return false;
    }
    void* map = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }
    const uint32_t pool = Create(shm_, WL_SHM_R_CREATE_POOL, &rgui_wl_shm_pool_interface, nullptr,
                                 {H(fd), I(static_cast<int32_t>(total))});
    close(fd);  // the request carries a duplicate
    if (!pool) {
        munmap(map, total);
        return false;
    }
    map_ = map;
    mapSize_ = total;
    for (int i = 0; i < 3; ++i) {
        Buffer& b = buffers_[i];
        b.id = Create(pool, WL_SHM_POOL_R_CREATE_BUFFER, &rgui_wl_buffer_interface,
                      &Trampoline<&WaylandWindowImpl::OnBuffer>,
                      {I(static_cast<int32_t>(one * i)), I(width), I(height), I(static_cast<int32_t>(stride)),
                       U(WL_SHM_FORMAT_XRGB8888)});
        b.busy = false;
        b.pixels = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(map) + one * i);
        b.damage.assign(1, gfx::RectI{0, 0, width, height});
    }
    Send(pool, WL_SHM_POOL_R_DESTROY);  // the buffers keep the memory
    bufferW_ = width;
    bufferH_ = height;
    fullDamage_ = true;
    return buffers_[0].id && buffers_[1].id && buffers_[2].id;
}

void WaylandWindowImpl::DestroyBuffers() {
    for (Buffer& b : buffers_) {
        if (!b.id) continue;
        if (b.busy) {
            retiredBuffers_.push_back(b.id);  // the compositor still reads it
        } else {
            Send(b.id, WL_BUFFER_R_DESTROY);
        }
        b = Buffer{};
    }
    if (map_) munmap(map_, mapSize_);  // the compositor has its own mapping
    map_ = nullptr;
    mapSize_ = 0;
    bufferW_ = bufferH_ = 0;
}

void WaylandWindowImpl::OnBuffer(uint32_t id, uint16_t opcode, const rgui_wl_arg*) {
    if (opcode != WL_BUFFER_E_RELEASE) return;
    for (Buffer& b : buffers_) {
        if (b.id == id) {
            b.busy = false;
            return;
        }
    }
    auto it = std::find(retiredBuffers_.begin(), retiredBuffers_.end(), id);
    if (it != retiredBuffers_.end()) {
        retiredBuffers_.erase(it);
        Send(id, WL_BUFFER_R_DESTROY);
    }
}

int WaylandWindowImpl::FreeBuffer() const {
    for (int i = 0; i < 3; ++i) {
        if (buffers_[i].id && !buffers_[i].busy) return i;
    }
    return -1;
}

int WaylandWindowImpl::WaitForFreeBuffer() {
    const uint64_t deadline = rgui_loop_now_ms() + 1000;
    for (;;) {
        const int free = FreeBuffer();
        if (free >= 0 || lost_) return free;
        const uint64_t now = rgui_loop_now_ms();
        if (now >= deadline) return -1;
        if (rgui_wl_flush_blocking(conn_, 1000) != 0) {
            Fail("lost the Wayland connection");
            return -1;
        }
        struct pollfd p = {rgui_wl_connection_fd(conn_), POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(deadline - now)) <= 0) continue;
        if (rgui_wl_read(conn_) < 0) {
            Fail("lost the Wayland connection");
            return -1;
        }
        if (rgui_wl_dispatch(conn_) < 0) {
            Fail("bad message from the compositor");
            return -1;
        }
    }
}

void WaylandWindowImpl::Present(const gfx::Surface& frame, const std::vector<gfx::RectI>& damage) {
    if (lost_ || !configured_ || !frame.pixels || frame.width != deviceW_ || frame.height != deviceH_ ||
        frame.stride < frame.width) {
        return;
    }
    if (!EnsureBuffers(deviceW_, deviceH_)) return;
    int index = FreeBuffer();
    if (index < 0) index = WaitForFreeBuffer();
    if (index < 0 || !buffers_[index].id) return;
    const gfx::RectI bounds{0, 0, deviceW_, deviceH_};

    // This frame's damage, clipped, at most kMaxDamageRects (else their bounding box).
    std::vector<gfx::RectI> frameDamage;
    if (fullDamage_) {
        frameDamage.push_back(bounds);
    } else {
        for (const gfx::RectI& r : damage) {
            const gfx::RectI c = r.Intersect(bounds);
            if (!c.Empty()) frameDamage.push_back(c);
        }
        if (frameDamage.size() > kMaxDamageRects) {
            gfx::RectI all;
            for (const gfx::RectI& r : frameDamage) all = all.Union(r);
            frameDamage.assign(1, all);
        }
    }
    for (Buffer& b : buffers_) {
        b.damage.insert(b.damage.end(), frameDamage.begin(), frameDamage.end());
        if (b.damage.size() > kMaxDamageRects) {
            gfx::RectI all;
            for (const gfx::RectI& r : b.damage) all = all.Union(r);
            b.damage.assign(1, all);
        }
    }
    Buffer& b = buffers_[index];
    for (const gfx::RectI& r : b.damage) {
        const gfx::RectI c = r.Intersect(bounds);
        if (c.Empty()) continue;
        const size_t bytes = static_cast<size_t>(c.Width()) * 4;
        for (int y = c.y0; y < c.y1; ++y) {
            std::memcpy(b.pixels + static_cast<size_t>(y) * deviceW_ + c.x0, frame.Row(y) + c.x0, bytes);
        }
    }
    b.damage.clear();

    if (bufferScale_ != appliedBufferScale_) {
        Send(surface_, WL_SURFACE_R_SET_BUFFER_SCALE, {I(bufferScale_)});
        appliedBufferScale_ = bufferScale_;
    }
    if (viewport_ && (appliedDestW_ != logicalW_ || appliedDestH_ != logicalH_)) {
        Send(viewport_, WP_VIEWPORT_R_SET_DESTINATION, {I(logicalW_), I(logicalH_)});
        appliedDestW_ = logicalW_;
        appliedDestH_ = logicalH_;
    }
    Send(surface_, WL_SURFACE_R_ATTACH, {O(b.id), I(0), I(0)});
    for (const gfx::RectI& r : frameDamage) {
        Send(surface_, WL_SURFACE_R_DAMAGE_BUFFER, {I(r.x0), I(r.y0), I(r.Width()), I(r.Height())});
    }
    if (!frameCallback_) {
        frameCallback_ = Create(surface_, WL_SURFACE_R_FRAME, &rgui_wl_callback_interface,
                                &Trampoline<&WaylandWindowImpl::OnFrameDone>);
    }
    Send(surface_, WL_SURFACE_R_COMMIT);
    b.busy = true;
    mapped_ = true;
    fullDamage_ = false;
    presentedSinceFrame_ = true;
}

void WaylandWindowImpl::OnFrameDone(uint32_t, uint16_t, const rgui_wl_arg*) {
    frameCallback_ = 0;
    ScheduleFrame();
}

void WaylandWindowImpl::RequestFrame() {
    frameRequested_ = true;
    ScheduleFrame();
}

// OnFrame runs from a timer: right after the frame callback of the last commit, else at most
// every 16 ms, so a client that asks for frames without presenting does not spin.
void WaylandWindowImpl::ScheduleFrame() {
    if (!frameRequested_ || !configured_ || frameCallback_ || frameTimer_ || lost_) return;
    const uint64_t since = rgui_loop_now_ms() - lastFrameMs_;
    const uint32_t delay = presentedSinceFrame_ || since >= 16 ? 0 : static_cast<uint32_t>(16 - since);
    frameTimer_ = rgui_loop_add_timer(delay, 0, OnFrameTimer, this);
}

void WaylandWindowImpl::OnFrameTimer(uint64_t, void* user) {
    auto* self = static_cast<WaylandWindowImpl*>(user);
    self->frameTimer_ = 0;
    if (!self->frameRequested_ || self->frameCallback_ || self->lost_) return;
    self->frameRequested_ = false;
    self->lastFrameMs_ = rgui_loop_now_ms();
    self->presentedSinceFrame_ = false;
    if (self->client_) self->client_->OnFrame();
}

// --- window management ------------------------------------------------------------------------

void WaylandWindowImpl::SetTitle(std::string_view title) {
    const std::string text = TruncateUtf8(title, 1024);
    Send(toplevel_, XDG_TOPLEVEL_R_SET_TITLE, {S(text.c_str())});
}

void WaylandWindowImpl::StartMove() { Send(toplevel_, XDG_TOPLEVEL_R_MOVE, {O(seat_), U(inputSerial_)}); }

void WaylandWindowImpl::StartResize(Edge edge) {
    uint32_t value = 0;
    switch (edge) {
    case Edge::Top: value = 1; break;
    case Edge::Bottom: value = 2; break;
    case Edge::Left: value = 4; break;
    case Edge::TopLeft: value = 5; break;
    case Edge::BottomLeft: value = 6; break;
    case Edge::Right: value = 8; break;
    case Edge::TopRight: value = 9; break;
    case Edge::BottomRight: value = 10; break;
    }
    Send(toplevel_, XDG_TOPLEVEL_R_RESIZE, {O(seat_), U(inputSerial_), U(value)});
}

void WaylandWindowImpl::ToggleMaximize() {
    Send(toplevel_, maximized_ ? XDG_TOPLEVEL_R_UNSET_MAXIMIZED : XDG_TOPLEVEL_R_SET_MAXIMIZED);
}

void WaylandWindowImpl::Minimize() { Send(toplevel_, XDG_TOPLEVEL_R_SET_MINIMIZED); }

// --- pointer ----------------------------------------------------------------------------------

void WaylandWindowImpl::OnPointer(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    switch (opcode) {
    case WL_POINTER_E_ENTER: {
        pointerSerial_ = args[0].u;
        pointerInside_ = args[1].o == surface_;
        pointerX_ = static_cast<float>(rgui_wl_fixed_to_double(args[2].f));
        pointerY_ = static_cast<float>(rgui_wl_fixed_to_double(args[3].f));
        shownCursor_ = -1;
        ApplyCursor();
        InputEvent e;
        e.type = InputEvent::Type::PointerMove;
        e.x = pointerX_;
        e.y = pointerY_;
        e.mods = Mods();
        Emit(e);
        break;
    }
    case WL_POINTER_E_LEAVE: {
        pointerInside_ = false;
        InputEvent e;
        e.type = InputEvent::Type::PointerLeave;
        Emit(e);
        break;
    }
    case WL_POINTER_E_MOTION: {
        pointerX_ = static_cast<float>(rgui_wl_fixed_to_double(args[1].f));
        pointerY_ = static_cast<float>(rgui_wl_fixed_to_double(args[2].f));
        InputEvent e;
        e.type = InputEvent::Type::PointerMove;
        e.x = pointerX_;
        e.y = pointerY_;
        e.mods = Mods();
        Emit(e);
        break;
    }
    case WL_POINTER_E_BUTTON: {
        const uint32_t button = args[2].u;
        const bool pressed = args[3].u == WL_POINTER_BUTTON_STATE_PRESSED;
        if (pressed) inputSerial_ = args[0].u;
        InputEvent e;
        e.type = InputEvent::Type::Button;
        if (button == kBtnLeft) e.button = MouseButton::Left;
        else if (button == kBtnRight) e.button = MouseButton::Right;
        else if (button == kBtnMiddle) e.button = MouseButton::Middle;
        else break;
        e.pressed = pressed;
        e.x = pointerX_;
        e.y = pointerY_;
        e.mods = Mods();
        Emit(e);
        break;
    }
    case WL_POINTER_E_AXIS: {
        const uint32_t axis = args[1].u;
        if (axis > 1) break;
        axisValue_[axis] += rgui_wl_fixed_to_double(args[2].f);
        axisSeen_[axis] = true;
        break;
    }
    case WL_POINTER_E_AXIS_SOURCE: axisSource_ = args[0].u; break;
    case WL_POINTER_E_AXIS_VALUE120: {
        const uint32_t axis = args[0].u;
        if (axis > 1) break;
        axisValue120_[axis] += args[1].i;
        axisValue120Seen_[axis] = true;
        break;
    }
    case WL_POINTER_E_FRAME: {
        InputEvent e;
        e.type = InputEvent::Type::Wheel;
        e.x = pointerX_;
        e.y = pointerY_;
        e.mods = Mods();
        bool any = false;
        const bool discrete = axisSource_ == WL_POINTER_AXIS_SOURCE_WHEEL ||
                              axisSource_ == WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
        for (int axis = 0; axis < 2; ++axis) {
            float notches = 0;
            float pixels = 0;
            if (axisValue120Seen_[axis]) {
                notches = static_cast<float>(axisValue120_[axis]) / 120.0f;
            } else if (axisSeen_[axis] && discrete) {
                notches = static_cast<float>(axisValue_[axis] / 15.0);  // a step of 15 axis units
            } else if (axisSeen_[axis]) {
                pixels = static_cast<float>(axisValue_[axis]);  // finger, continuous: logical pixels
            }
            if (notches != 0 || pixels != 0) any = true;
            if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
                e.notchesY = notches;
                e.pixelsY = pixels;
            } else {
                e.notchesX = notches;
                e.pixelsX = pixels;
            }
            axisValue_[axis] = 0;
            axisSeen_[axis] = false;
            axisValue120_[axis] = 0;
            axisValue120Seen_[axis] = false;
        }
        axisSource_ = UINT32_MAX;
        if (any) Emit(e);
        break;
    }
    default: break;
    }
}

void WaylandWindowImpl::SetCursor(Cursor cursor) {
    if (cursor == cursor_) return;
    cursor_ = cursor;
    ApplyCursor();
}

void WaylandWindowImpl::ApplyCursor() {
    if (!pointer_ || !pointerInside_) return;
    if (cursorShapeDevice_) {
        uint32_t shape = WP_CURSOR_SHAPE_DEFAULT;
        switch (cursor_) {
        case Cursor::Arrow: shape = WP_CURSOR_SHAPE_DEFAULT; break;
        case Cursor::IBeam: shape = WP_CURSOR_SHAPE_TEXT; break;
        case Cursor::Hand: shape = WP_CURSOR_SHAPE_POINTER; break;
        case Cursor::ResizeH: shape = WP_CURSOR_SHAPE_EW_RESIZE; break;
        case Cursor::ResizeV: shape = WP_CURSOR_SHAPE_NS_RESIZE; break;
        case Cursor::ResizeNWSE: shape = WP_CURSOR_SHAPE_NWSE_RESIZE; break;
        case Cursor::ResizeNESW: shape = WP_CURSOR_SHAPE_NESW_RESIZE; break;
        }
        Send(cursorShapeDevice_, WP_CURSOR_SHAPE_DEVICE_V1_R_SET_SHAPE, {U(pointerSerial_), U(shape)});
        return;
    }
    // Built-in cursors at the window's scale, rounded up to a whole buffer scale.
    int scale = 1;
    while (scale < 8 && scale < scale_) ++scale;
    if (!EnsureCursorImages(scale)) return;
    const int which = cursor_ == Cursor::IBeam ? 1 : 0;
    if (which != shownCursor_) {
        Send(cursorSurface_, WL_SURFACE_R_ATTACH, {O(cursorBuffers_[which]), I(0), I(0)});
        Send(cursorSurface_, WL_SURFACE_R_DAMAGE_BUFFER, {I(0), I(0), I(kCursorGrid * scale), I(kCursorGrid * scale)});
        Send(cursorSurface_, WL_SURFACE_R_COMMIT);
        shownCursor_ = which;
    }
    const int hotX = which ? 8 : 2;
    const int hotY = which ? 12 : 2;
    Send(pointer_, WL_POINTER_R_SET_CURSOR, {U(pointerSerial_), O(cursorSurface_), I(hotX), I(hotY)});
}

bool WaylandWindowImpl::EnsureCursorImages(int scale) {
    if (cursorScale_ == scale && cursorBuffers_[0]) return true;
    if (!cursorSurface_) {
        cursorSurface_ = Create(compositor_, WL_COMPOSITOR_R_CREATE_SURFACE, &rgui_wl_surface_interface, nullptr);
        if (!cursorSurface_) return false;
    }
    for (uint32_t& id : cursorBuffers_) {
        if (id) Send(id, WL_BUFFER_R_DESTROY);
        id = 0;
    }
    const int side = kCursorGrid * scale;
    const size_t one = static_cast<size_t>(side) * side * 4;
    const int fd = memfd_create("rayomd-gui-cursor", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, static_cast<off_t>(one * 2)) != 0) {
        close(fd);
        return false;
    }
    void* map = mmap(nullptr, one * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }
    std::memset(map, 0, one * 2);
    const uint32_t white = 0xFFFFFFFFu;
    const uint32_t black = 0xFF000000u;
    const gfx::Matrix m = gfx::Matrix::Scale(scale, scale);
    {
        // The arrow: tip at (2, 2), black with a white outline.
        gfx::Surface s{static_cast<uint32_t*>(map), side, side, side};
        gfx::Canvas canvas(s);
        gfx::Path arrow;
        arrow.MoveTo(2, 2);
        arrow.LineTo(2, 19);
        arrow.LineTo(6.5f, 15);
        arrow.LineTo(9.5f, 21.5f);
        arrow.LineTo(12, 20.5f);
        arrow.LineTo(9, 14);
        arrow.LineTo(15, 14);
        arrow.Close();
        gfx::StrokeStyle outline;
        outline.width = 2;
        outline.join = gfx::LineJoin::Round;
        canvas.StrokePath(arrow, m, outline, gfx::Paint::Solid(white));
        canvas.FillPath(arrow, m, gfx::FillRule::NonZero, gfx::Paint::Solid(black));
    }
    {
        // The I-beam: a bar from y 4 to 20 at x 8 with serifs, black over a white halo.
        gfx::Surface s{reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(map) + one), side, side, side};
        gfx::Canvas canvas(s);
        gfx::Path beam;
        beam.MoveTo(5, 4);
        beam.LineTo(11, 4);
        beam.MoveTo(8, 4);
        beam.LineTo(8, 20);
        beam.MoveTo(5, 20);
        beam.LineTo(11, 20);
        gfx::StrokeStyle halo;
        halo.width = 3.5f;
        halo.cap = gfx::LineCap::Square;
        canvas.StrokePath(beam, m, halo, gfx::Paint::Solid(white));
        gfx::StrokeStyle bar;
        bar.width = 1.5f;
        bar.cap = gfx::LineCap::Square;
        canvas.StrokePath(beam, m, bar, gfx::Paint::Solid(black));
    }
    munmap(map, one * 2);
    const uint32_t pool = Create(shm_, WL_SHM_R_CREATE_POOL, &rgui_wl_shm_pool_interface, nullptr,
                                 {H(fd), I(static_cast<int32_t>(one * 2))});
    close(fd);
    if (!pool) return false;
    for (int i = 0; i < 2; ++i) {
        cursorBuffers_[i] = Create(pool, WL_SHM_POOL_R_CREATE_BUFFER, &rgui_wl_buffer_interface, nullptr,
                                   {I(static_cast<int32_t>(one * i)), I(side), I(side), I(side * 4), U(0) /* ARGB8888 */});
    }
    Send(pool, WL_SHM_POOL_R_DESTROY);
    Send(cursorSurface_, WL_SURFACE_R_SET_BUFFER_SCALE, {I(scale)});
    cursorScale_ = scale;
    shownCursor_ = -1;
    return cursorBuffers_[0] && cursorBuffers_[1];
}

// --- keyboard ---------------------------------------------------------------------------------

void WaylandWindowImpl::OnKeyboard(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    switch (opcode) {
    case WL_KEYBOARD_E_KEYMAP: {
        const int fd = args[1].h;
        const uint32_t size = args[2].u;
        std::string text;
        if (args[0].u == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0 && size <= kMaxKeymapBytes) {
            if (!ReadKeymapFd(fd, size, &text)) {
                fprintf(stderr, "rayomd-gui: the compositor's keymap is not a file of %u bytes\n", size);
            } else {
                std::string error;
                std::unique_ptr<xkb::Keymap> keymap = xkb::Keymap::Parse(text, &error);
                if (keymap) {
                    StopRepeat();
                    compose_.Reset();
                    keymap_ = std::move(keymap);
                } else {
                    fprintf(stderr, "rayomd-gui: cannot read the keymap: %s\n", error.c_str());
                }
            }
        }
        close(fd);
        break;
    }
    case WL_KEYBOARD_E_ENTER: {
        keyboardEnterSerial_ = args[0].u;
        InputEvent e;
        e.type = InputEvent::Type::FocusIn;
        Emit(e);
        break;
    }
    case WL_KEYBOARD_E_LEAVE: {
        StopRepeat();
        compose_.Reset();
        InputEvent e;
        e.type = InputEvent::Type::FocusOut;
        Emit(e);
        break;
    }
    case WL_KEYBOARD_E_KEY: {
        const uint32_t key = args[2].u;
        if (args[3].u == WL_KEYBOARD_KEY_STATE_PRESSED) {
            inputSerial_ = args[0].u;
            KeyPressed(key, false);
        } else if (key == repeatKey_) {
            StopRepeat();
        }
        break;
    }
    case WL_KEYBOARD_E_MODIFIERS:
        if (keymap_) keymap_->UpdateModifiers(args[1].u, args[2].u, args[3].u, args[4].u);
        break;
    case WL_KEYBOARD_E_REPEAT_INFO:
        repeatRate_ = std::clamp(args[0].i, 0, 1000);
        repeatDelay_ = std::clamp(args[1].i, 0, 10000);
        break;
    default: break;
    }
}

void WaylandWindowImpl::KeyPressed(uint32_t key, bool repeat) {
    if (!keymap_) return;
    const uint8_t mods = keymap_->Modifiers();
    const Key shortcut = keymap_->ShortcutKey(key);
    if (shortcut != Key::None) {
        InputEvent e;
        e.type = InputEvent::Type::KeyDown;
        e.key = shortcut;
        e.mods = mods;
        e.repeat = repeat;
        Emit(e);
    }
    // Typed text: the keysym's character, or what a Compose sequence ends with; never while a
    // shortcut modifier is held. A repeat types the key's character again, outside Compose.
    const uint32_t keysym = keymap_->KeySym(key);
    std::string text;
    bool typeKeysym = repeat;
    if (!repeat) {
        switch (compose_.Feed(keysym, text)) {
        case xkb::Compose::Result::Ignored: typeKeysym = true; break;
        case xkb::Compose::Result::Composed: break;
        case xkb::Compose::Result::Composing:
        case xkb::Compose::Result::Cancelled: text.clear(); break;
        }
    }
    if (typeKeysym) {
        text.clear();
        const uint32_t cp = xkb::KeysymToCodepoint(keysym);
        if (cp >= 0x20 && cp != 0x7F) AppendUtf8(text, cp);
    }
    if (!text.empty() && !(mods & (kModCtrl | kModAlt | kModSuper))) {
        InputEvent e;
        e.type = InputEvent::Type::Text;
        e.text = std::move(text);
        e.mods = mods;
        Emit(e);
    }
    if (!repeat && keymap_ && keymap_->Repeats(key) && repeatRate_ > 0) {
        StopRepeat();
        repeatKey_ = key;
        const uint32_t interval = static_cast<uint32_t>(std::max(1, 1000 / repeatRate_));
        repeatTimer_ = rgui_loop_add_timer(static_cast<uint32_t>(repeatDelay_), interval, OnRepeatTimer, this);
    }
}

void WaylandWindowImpl::StopRepeat() {
    if (repeatTimer_) rgui_loop_cancel_timer(repeatTimer_);
    repeatTimer_ = 0;
    repeatKey_ = 0;
}

void WaylandWindowImpl::OnRepeatTimer(uint64_t id, void* user) {
    auto* self = static_cast<WaylandWindowImpl*>(user);
    if (id != self->repeatTimer_ || !self->repeatKey_) return;
    self->KeyPressed(self->repeatKey_, true);
}

// --- text input -------------------------------------------------------------------------------

void WaylandWindowImpl::SetTextInputRect(gfx::RectI rect, bool enabled) {
    const bool rectChanged = rect != textInputRect_;
    textInputRect_ = rect;
    appWantsTextInput_ = enabled;
    const bool wasEnabled = textInputEnabled_;
    UpdateTextInput();
    if (textInputEnabled_ && wasEnabled && rectChanged) {
        Send(textInput_, ZWP_TEXT_INPUT_V3_R_SET_CURSOR_RECTANGLE,
             {I(rect.x0), I(rect.y0), I(rect.Width()), I(rect.Height())});
        Send(textInput_, ZWP_TEXT_INPUT_V3_R_COMMIT);
    }
}

void WaylandWindowImpl::UpdateTextInput() {
    if (!textInput_) return;
    const bool want = textInputEntered_ && appWantsTextInput_;
    if (want && !textInputEnabled_) {
        const gfx::RectI& r = textInputRect_;
        Send(textInput_, ZWP_TEXT_INPUT_V3_R_ENABLE);
        Send(textInput_, ZWP_TEXT_INPUT_V3_R_SET_CONTENT_TYPE,
             {U(ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE), U(ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL)});
        Send(textInput_, ZWP_TEXT_INPUT_V3_R_SET_CURSOR_RECTANGLE, {I(r.x0), I(r.y0), I(r.Width()), I(r.Height())});
        Send(textInput_, ZWP_TEXT_INPUT_V3_R_COMMIT);
        textInputEnabled_ = true;
    } else if (!want && textInputEnabled_) {
        Send(textInput_, ZWP_TEXT_INPUT_V3_R_DISABLE);
        Send(textInput_, ZWP_TEXT_INPUT_V3_R_COMMIT);
        textInputEnabled_ = false;
        if (!preedit_.empty()) {
            preedit_.clear();
            InputEvent e;
            e.type = InputEvent::Type::Preedit;
            Emit(e);
        }
    }
}

void WaylandWindowImpl::OnTextInput(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    switch (opcode) {
    case ZWP_TEXT_INPUT_V3_E_ENTER:
        if (args[0].o == surface_) {
            textInputEntered_ = true;
            UpdateTextInput();
        }
        break;
    case ZWP_TEXT_INPUT_V3_E_LEAVE:
        textInputEntered_ = false;
        UpdateTextInput();
        break;
    case ZWP_TEXT_INPUT_V3_E_PREEDIT_STRING:
        pendingPreedit_ = args[0].s ? args[0].s : "";
        pendingPreeditCursor_ = args[1].i;
        if (pendingPreeditCursor_ < 0 || static_cast<size_t>(pendingPreeditCursor_) > pendingPreedit_.size()) {
            pendingPreeditCursor_ = -1;
        }
        break;
    case ZWP_TEXT_INPUT_V3_E_COMMIT_STRING: pendingCommit_ = args[0].s ? args[0].s : ""; break;
    case ZWP_TEXT_INPUT_V3_E_DONE: {
        // The order of text-input-v3: the commit string goes in, then the preedit replaces the old
        // one (a batch without preedit_string clears it).
        if (!pendingCommit_.empty()) {
            InputEvent e;
            e.type = InputEvent::Type::Text;
            e.text = std::move(pendingCommit_);
            Emit(e);
        }
        if (pendingPreedit_ != preedit_ || !pendingPreedit_.empty()) {
            preedit_ = pendingPreedit_;
            InputEvent e;
            e.type = InputEvent::Type::Preedit;
            e.text = preedit_;
            e.preeditCursor = pendingPreeditCursor_;
            Emit(e);
        }
        pendingCommit_.clear();
        pendingPreedit_.clear();
        pendingPreeditCursor_ = -1;
        break;
    }
    default: break;
    }
}

// --- clipboard and drops ----------------------------------------------------------------------

void WaylandWindowImpl::DestroyOffer(uint32_t id) {
    if (!id) return;
    auto it = offers_.find(id);
    if (it == offers_.end()) return;
    offers_.erase(it);
    Send(id, WL_DATA_OFFER_R_DESTROY);
}

// Offers that are neither the selection, the drag in progress nor being read.
void WaylandWindowImpl::DropStaleOffers() {
    std::vector<uint32_t> stale;
    for (const auto& o : offers_) {
        if (o.first != selectionOffer_ && o.first != dndOffer_ && !o.second.transferring) stale.push_back(o.first);
    }
    for (uint32_t id : stale) DestroyOffer(id);
}

void WaylandWindowImpl::OnDataDevice(uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    switch (opcode) {
    case WL_DATA_DEVICE_E_DATA_OFFER:
        offers_[args[0].n] = Offer{};
        rgui_wl_set_dispatcher(conn_, args[0].n, &Trampoline<&WaylandWindowImpl::OnDataOffer>, this);
        break;
    case WL_DATA_DEVICE_E_SELECTION:
        selectionOffer_ = offers_.count(args[0].o) ? args[0].o : 0;
        DropStaleOffers();
        break;
    case WL_DATA_DEVICE_E_ENTER: {
        const uint32_t serial = args[0].u;
        dndOffer_ = offers_.count(args[4].o) ? args[4].o : 0;
        DropStaleOffers();
        if (!dndOffer_) break;
        const Offer& offer = offers_[dndOffer_];
        const bool uris = std::find(offer.mimes.begin(), offer.mimes.end(), "text/uri-list") != offer.mimes.end();
        Send(dndOffer_, WL_DATA_OFFER_R_ACCEPT, {U(serial), S(uris ? "text/uri-list" : nullptr)});
        if (uris && rgui_wl_object_version(conn_, dndOffer_) >= 3) {
            Send(dndOffer_, WL_DATA_OFFER_R_SET_ACTIONS,
                 {U(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY), U(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)});
        }
        break;
    }
    case WL_DATA_DEVICE_E_LEAVE: {
        const uint32_t offer = dndOffer_;
        dndOffer_ = 0;
        auto it = offers_.find(offer);
        if (it != offers_.end() && !it->second.transferring) DestroyOffer(offer);
        break;
    }
    case WL_DATA_DEVICE_E_DROP: {
        const uint32_t id = dndOffer_;
        dndOffer_ = 0;
        auto it = offers_.find(id);
        if (it == offers_.end()) break;
        const bool uris =
            std::find(it->second.mimes.begin(), it->second.mimes.end(), "text/uri-list") != it->second.mimes.end();
        if (!uris) {
            DestroyOffer(id);
            break;
        }
        it->second.transferring = true;
        Receive(id, "text/uri-list", false, [this, id](std::string list) {
            auto offer = offers_.find(id);
            if (offer != offers_.end()) {
                if (rgui_wl_object_version(conn_, id) >= 3 && offer->second.action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
                    Send(id, WL_DATA_OFFER_R_FINISH);
                }
                DestroyOffer(id);
            }
            std::string path = PathFromUriList(list);
            if (!path.empty() && client_) client_->OnDropFile(std::move(path));
        });
        break;
    }
    default: break;
    }
}

void WaylandWindowImpl::OnDataOffer(uint32_t id, uint16_t opcode, const rgui_wl_arg* args) {
    auto it = offers_.find(id);
    if (it == offers_.end()) return;
    Offer& offer = it->second;
    switch (opcode) {
    case WL_DATA_OFFER_E_OFFER:
        if (offer.mimes.size() < 64 && std::strlen(args[0].s) < 256) offer.mimes.emplace_back(args[0].s);
        break;
    case WL_DATA_OFFER_E_SOURCE_ACTIONS: offer.sourceActions = args[0].u; break;
    case WL_DATA_OFFER_E_ACTION: offer.action = args[0].u; break;
    default: break;
    }
}

void WaylandWindowImpl::Receive(uint32_t offer, const char* mime, bool paste,
                                std::function<void(std::string)> done) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        tasks_.Post([done = std::move(done)] { done(std::string()); });
        return;
    }
    Send(offer, WL_DATA_OFFER_R_RECEIVE, {S(mime), H(fds[1])});
    close(fds[1]);  // the request carries a duplicate; the source closes the last write end
    Flush();
    if (paste) {
        transfers_.ReceivePaste(fds[0], std::move(done));
    } else {
        transfers_.Receive(fds[0], std::move(done));
    }
}

void WaylandWindowImpl::OnDataSource(uint32_t id, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode == WL_DATA_SOURCE_E_SEND) {
        const int fd = args[1].h;
        if (id != selectionSource_ || !selectionText_) {
            close(fd);
            return;
        }
        transfers_.Send(fd, selectionText_);  // closes it when over the limits
    } else if (opcode == WL_DATA_SOURCE_E_CANCELLED) {
        if (id == selectionSource_) {
            selectionSource_ = 0;
            selectionText_.reset();
        }
        Send(id, WL_DATA_SOURCE_R_DESTROY);
    }
}

void WaylandWindowImpl::SetClipboardText(std::string text) {
    if (!dataDevice_) return;
    if (selectionSource_) Send(selectionSource_, WL_DATA_SOURCE_R_DESTROY);
    selectionSource_ = Create(dataDeviceManager_, WL_DATA_DEVICE_MANAGER_R_CREATE_DATA_SOURCE,
                              &rgui_wl_data_source_interface, &Trampoline<&WaylandWindowImpl::OnDataSource>);
    if (!selectionSource_) return;
    selectionText_ = std::make_shared<const std::string>(std::move(text));
    for (const char* mime : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"}) {
        Send(selectionSource_, WL_DATA_SOURCE_R_OFFER, {S(mime)});
    }
    // The serial of the last key or button press; before any, the keyboard focus's own.
    const uint32_t serial = inputSerial_ ? inputSerial_ : keyboardEnterSerial_;
    Send(dataDevice_, WL_DATA_DEVICE_R_SET_SELECTION, {O(selectionSource_), U(serial)});
}

void WaylandWindowImpl::RequestClipboardText(std::function<void(std::string)> callback) {
    if (!callback) return;
    auto it = offers_.find(selectionOffer_);
    if (selectionOffer_ && it != offers_.end()) {
        for (const char* mime : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"}) {
            if (std::find(it->second.mimes.begin(), it->second.mimes.end(), mime) != it->second.mimes.end()) {
                Receive(selectionOffer_, mime, true, std::move(callback));
                return;
            }
        }
    }
    tasks_.Post([callback = std::move(callback)] { callback(std::string()); });
}

// --- the rest of Platform ---------------------------------------------------------------------

void WaylandWindowImpl::ShowFileDialog(FileDialog kind, std::string suggestedPath,
                                       std::function<void(FileDialogResult)> callback) {
    if (hooks_.showFileDialog) {
        hooks_.showFileDialog(kind, std::move(suggestedPath), std::move(callback));
        return;
    }
    tasks_.Post([callback = std::move(callback)] {
        FileDialogResult result;
        result.kind = FileDialogResult::Unavailable;
        if (callback) callback(result);
    });
}

void WaylandWindowImpl::OpenExternally(const std::string& path) {
    if (hooks_.openExternally) hooks_.openExternally(path);
}

uint64_t WaylandWindowImpl::StartTimer(int ms, std::function<void()> callback) {
    const uint64_t id = rgui_loop_add_timer(static_cast<uint32_t>(std::max(ms, 0)), 0, OnUserTimer, this);
    if (id) timers_[id] = std::move(callback);
    return id;
}

void WaylandWindowImpl::CancelTimer(uint64_t id) {
    if (timers_.erase(id)) rgui_loop_cancel_timer(id);
}

void WaylandWindowImpl::OnUserTimer(uint64_t id, void* user) {
    auto* self = static_cast<WaylandWindowImpl*>(user);
    auto it = self->timers_.find(id);
    if (it == self->timers_.end()) return;
    std::function<void()> callback = std::move(it->second);
    self->timers_.erase(it);
    if (callback) callback();
}

} // namespace

uint32_t FitScale120(int logicalWidth, int logicalHeight, uint32_t scale120, bool wholeScale) {
    const uint64_t lw = static_cast<uint64_t>(std::max(logicalWidth, 1));
    const uint64_t lh = static_cast<uint64_t>(std::max(logicalHeight, 1));
    const uint32_t step = wholeScale ? 120 : 1;
    auto fits = [&](uint32_t s) {
        const uint64_t w = (lw * s + 60) / 120;
        const uint64_t h = (lh * s + 60) / 120;
        return w <= static_cast<uint64_t>(kMaxDeviceSide) && h <= static_cast<uint64_t>(kMaxDeviceSide) &&
               w * h <= static_cast<uint64_t>(kMaxDevicePixels);
    };
    while (scale120 > 120 && !fits(scale120)) scale120 = scale120 - step < 120 ? 120 : scale120 - step;
    return scale120;
}

bool ReadKeymapFd(int fd, uint32_t size, std::string* text) {
    struct stat st;
    if (size == 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) < size) {
        return false;
    }
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) return false;
    std::string_view view(static_cast<const char*>(map), size);
    while (!view.empty() && view.back() == '\0') view.remove_suffix(1);
    text->assign(view.data(), view.size());
    munmap(map, size);
    return true;
}

std::unique_ptr<WaylandWindow> WaylandWindow::Create(const WindowOptions& options) {
    auto window = std::make_unique<WaylandWindowImpl>();
    if (!window->Init(options)) return nullptr;
    return window;
}

} // namespace rgui::wl
