// SPDX-License-Identifier: Apache-2.0
// The desktop portal (xdg-desktop-portal) over our D-Bus client: the Settings interface for the
// colour scheme and text scale, and the FileChooser for the system's open and save dialogs.
//
// Everything runs on the UI thread. The event loop polls Fd() for PollEvents() and calls
// Dispatch() when it is ready and when TimeoutMs() has passed. Callbacks run from Dispatch(),
// or before the request returns when there is no session bus.
//
// Signals are believed only from the portal: the current owner of org.freedesktop.portal.Desktop,
// learned from the bus (GetNameOwner, then NameOwnerChanged) and from the senders of the portal's
// own replies, which the bus sets. Another peer's Response or SettingChanged, broadcast or sent to
// us directly, is ignored. Request tokens are random.
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

    // Starts connecting to the session bus ($DBUS_SESSION_BUS_ADDRESS, else
    // $XDG_RUNTIME_DIR/bus), or to `address`, without waiting: the handshake goes on in
    // Dispatch() and must end within `timeoutMs`. Requests may be made at once. False when there
    // is no bus to connect to: requests then answer as if there were no portal. A file dialog
    // asked for before the handshake ended waits for it (at most the rest of `timeoutMs`).
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
    // The portal's unique name as far as we know it; empty when it is not running.
    const std::string& PortalOwner() const { return portalOwner_; }

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
    static void OnNameOwner(void* user, const rgui_dbus_message* reply);
    static void OnNameOwnerChanged(void* user, const rgui_dbus_message* signal);

    // After a connection started: follows the portal's owner.
    bool Started(int timeoutMs);
    // A method return of a call to the portal: its sender is the portal.
    void LearnOwner(const rgui_dbus_message* reply);
    bool FromPortal(const rgui_dbus_message* signal) const;
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
    uint32_t ownerMatch_ = 0;
    int connectTimeoutMs_ = 0;
    std::string portalOwner_;
    std::vector<std::unique_ptr<Request>> requests_;
    std::vector<std::unique_ptr<SettingsRead>> reads_;
};

} // namespace rgui::dbus
