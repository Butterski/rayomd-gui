// SPDX-License-Identifier: Apache-2.0
// The Wayland window: rgui::Platform on our own wire protocol. One xdg_toplevel drawn from three
// wl_shm buffers in one memfd, input from the first wl_seat (keymaps through rgui::xkb), the
// clipboard and drops through wl_data_device, the IME through text-input-v3, scale through
// fractional-scale + viewporter or integer buffer scales. Everything runs on the event loop of
// loop.h, which must be initialised (rgui_loop_init) before Create.
#pragma once

#include "app/platform.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rgui::wl {

// What main_linux.cpp provides from the other Linux modules (the portal, xdg-open, Omarchy).
// Unset hooks: ShowFileDialog answers Unavailable, OpenExternally does nothing, the Omarchy
// paths are nullopt.
struct WindowHooks {
    std::function<void(FileDialog kind, std::string suggestedPath, std::function<void(FileDialogResult)> done)>
        showFileDialog;
    std::function<void(const std::string& path)> openExternally;
    std::function<std::optional<std::filesystem::path>()> omarchyThemeDir;
    std::function<std::optional<std::filesystem::path>()> omarchyUserShellToml;
};

struct WindowOptions {
    int width = 1280;   // logical pixels, used until the compositor chooses a size
    int height = 820;
    bool maximized = false;
    std::string title = "RayoMD";
};

struct AdvertisedGlobal {
    std::string interface;
    uint32_t version = 0;
    uint32_t bound = 0;  // the version bound, 0 when not used
};

class WaylandWindow : public Platform {
public:
    // Connects to the compositor, binds the globals and creates the toplevel, committed without
    // a buffer: the first configure arrives from the loop, then OnResize and OnFrame. nullptr
    // after printing why (no compositor, or "rayomd-gui: the compositor lacks <interface>"): the
    // caller exits with status 1.
    static std::unique_ptr<WaylandWindow> Create(const WindowOptions& options);

    // Receives every callback; set it before the loop runs.
    virtual void SetClient(PlatformClient* client) = 0;
    virtual void SetHooks(WindowHooks hooks) = 0;
    // True once the connection failed (the loop was told to quit).
    virtual bool ConnectionLost() const = 0;
    // The window's state, for saving it.
    virtual int LogicalWidth() const = 0;
    virtual int LogicalHeight() const = 0;
    virtual bool Maximized() const = 0;
    // What the compositor advertised, in order (diagnostics).
    virtual std::vector<AdvertisedGlobal> Globals() const = 0;
};

// XDG directories (Phase 9 step 9): $XDG_CONFIG_HOME/rayomd-gui, $XDG_CACHE_HOME/rayomd/images,
// XDG_DOCUMENTS_DIR from $XDG_CONFIG_HOME/user-dirs.dirs (else ~/Documents).
std::filesystem::path XdgConfigDir();
std::filesystem::path XdgImageCacheDir();
std::filesystem::path XdgDocumentsDir();

} // namespace rgui::wl
