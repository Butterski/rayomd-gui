// SPDX-License-Identifier: Apache-2.0
// The desktop portal (xdg-desktop-portal) over our D-Bus client: the Settings interface for the
// colour scheme and text scale, and the FileChooser for the system's open and save dialogs.
//
// Everything runs on the UI thread. The event loop polls Fd() for PollEvents() and calls
// Dispatch() when it is ready and when TimeoutMs() has passed. Callbacks run from Dispatch(),
// or before the request returns when there is no session bus.
#pragma once

#include "app/platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct rgui_dbus_conn;
struct rgui_dbus_message;
struct rgui_dbus_iter;

namespace rgui::dbus {

struct Appearance {
    bool dark = true;        // org.freedesktop.appearance color-scheme: 2 is light, anything else dark
    double textScale = 1.0;  // org.gnome.desktop.interface text-scaling-factor
};

// Turns a portal's file URI into a path: `file://` (or `file://localhost`) and a percent-decoded
// absolute path. False for anything else, %00 or a malformed escape.
bool FileUriToPath(std::string_view uri, std::string& path);

// The FileChooser request path for a sender's unique name and a token:
// /org/freedesktop/portal/desktop/request/<name without ':' and with '.' -> '_'>/<token>.
std::string RequestPath(std::string_view uniqueName, std::string_view token);

class Portal {
public:
    Portal();
    ~Portal();
    Portal(const Portal&) = delete;
    Portal& operator=(const Portal&) = delete;

    // Connects to the session bus ($DBUS_SESSION_BUS_ADDRESS, else $XDG_RUNTIME_DIR/bus), or
    // to `address`, waiting at most `timeoutMs`. False without a bus: requests then answer as
    // if there were no portal.
    bool Connect(int timeoutMs = 2000);
    bool ConnectTo(const char* address, int timeoutMs = 2000);
    bool Connected() const { return conn_ != nullptr; }

    // For the event loop; Fd() is -1 when not connected.
    int Fd() const;
    short PollEvents() const;
    int TimeoutMs() const;
    // False once the connection is gone (the loop then stops polling Fd()); requests still
    // waiting were answered (file dialogs as Failed, settings with their defaults).
    bool Dispatch();

    // Reads the colour scheme and text scale (Settings.ReadOne, else the older Settings.Read)
    // and calls `done` once with what was found; keys that cannot be read keep their values.
    void ReadAppearance(std::function<void(Appearance)> done);
    // Calls `changed` with the updated appearance whenever the portal's SettingChanged signal
    // changes either key.
    void WatchAppearance(std::function<void(Appearance)> changed);
    const Appearance& CurrentAppearance() const { return appearance_; }

    // The portal's OpenFile (OpenDocument, ChooseTheme) or SaveFile (SaveMarkdown, ExportPdf).
    // Unavailable without a bus or when the call fails; Cancelled for Response 1; Failed for
    // any other response or a result without a usable file URI.
    void ShowFileDialog(FileDialog kind, const std::string& suggestedPath,
                        std::function<void(FileDialogResult)> callback);

private:
    struct Request;
    struct KeyRead;
    struct SettingsRead;

    static void OnFileReply(void* user, const rgui_dbus_message* reply);
    static void OnFileResponse(void* user, const rgui_dbus_message* signal);
    static void OnSettingReply(void* user, const rgui_dbus_message* reply);
    static void OnSettingChanged(void* user, const rgui_dbus_message* signal);

    void Finish(Request* request, FileDialogResult result);
    bool ReadKey(KeyRead* key);
    void KeyDone(KeyRead* key);
    // Applies a setting's value (the iterator stands at its variant); true when it changed.
    bool ApplySetting(std::string_view nameSpace, std::string_view key, rgui_dbus_iter* value);
    void Disconnect();

    rgui_dbus_conn* conn_ = nullptr;
    Appearance appearance_;
    std::function<void(Appearance)> changed_;
    uint32_t settingsMatch_ = 0;
    uint32_t nextToken_ = 0;
    std::vector<std::unique_ptr<Request>> requests_;
    std::vector<std::unique_ptr<SettingsRead>> reads_;
};

} // namespace rgui::dbus
