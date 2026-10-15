// SPDX-License-Identifier: Apache-2.0
// The desktop portal: Settings and FileChooser (see portal.h). References: the XDG Desktop
// Portal documentation of org.freedesktop.portal.Settings, .FileChooser and .Request.
//
// The D-Bus messages RayoMD GUI sends and receives (signatures as in the interfaces' XML):
//   org.freedesktop.DBus             Hello () -> s; AddMatch (s); RemoveMatch (s)   [dbus_wire.c]
//                                    GetNameOwner (s) -> s; signal NameOwnerChanged (sss)
//   org.freedesktop.DBus.Peer        Ping () -> (), answered                        [dbus_wire.c]
//   org.freedesktop.portal.Settings  ReadOne (ss) -> v; Read (ss) -> v (a v in the v);
//                                    signal SettingChanged (ssv)
//   org.freedesktop.portal.FileChooser  OpenFile (ssa{sv}) -> o; SaveFile (ssa{sv}) -> o
//   org.freedesktop.portal.Request   signal Response (ua{sv})

#include "platform/linux/portal.h"

#include "platform/linux/dbus_wire.h"

#include "core/rayomd_pdf_source.h"

#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace rgui::dbus {
namespace {

constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kSettings = "org.freedesktop.portal.Settings";
constexpr const char* kFileChooser = "org.freedesktop.portal.FileChooser";
constexpr const char* kAppearance = "org.freedesktop.appearance";
constexpr const char* kColorScheme = "color-scheme";
constexpr const char* kInterface = "org.gnome.desktop.interface";
constexpr const char* kTextScale = "text-scaling-factor";
constexpr int kCallTimeoutMs = 25000;
constexpr const char* kBusName = "org.freedesktop.DBus";
constexpr const char* kBusPath = "/org/freedesktop/DBus";

// A request token nobody can guess: 64 random bits (getrandom; should the kernel refuse, the
// clock, the pid and a counter, which still never repeat within the process).
std::string RandomToken() {
    static uint64_t counter = 0;
    uint64_t r = 0;
    if (getrandom(&r, sizeof r, GRND_NONBLOCK) != static_cast<ssize_t>(sizeof r)) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        r = (static_cast<uint64_t>(ts.tv_sec) * 1000000000u + static_cast<uint64_t>(ts.tv_nsec)) ^
            (static_cast<uint64_t>(getpid()) << 40) ^ (++counter * 0x9E3779B97F4A7C15ull);
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(r));
    return std::string("rayomd_gui_") + hex;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool IsDbusString(std::string_view s) {
    return s.find('\0') == std::string_view::npos && RayoMd::PdfSource::IsValidUtf8(s);
}

struct Filter {
    const char* name;
    std::vector<const char*> globs;
};

std::vector<Filter> FiltersFor(FileDialog kind) {
    switch (kind) {
    case FileDialog::OpenDocument:
        return {{"Markdown", {"*.md", "*.markdown", "*.txt"}}, {"RayoMD PDF", {"*.pdf"}}};
    case FileDialog::SaveMarkdown:
        return {{"Markdown", {"*.md"}}};
    case FileDialog::ExportPdf:
        return {{"PDF", {"*.pdf"}}};
    case FileDialog::ChooseTheme:
        return {{"Theme files", {"*.theme", "*.txt"}}, {"All files", {"*"}}};
    }
    return {};
}

const char* TitleFor(FileDialog kind) {
    switch (kind) {
    case FileDialog::OpenDocument: return "Open Markdown or RayoMD PDF";
    case FileDialog::SaveMarkdown: return "Save Markdown";
    case FileDialog::ExportPdf: return "Export PDF";
    case FileDialog::ChooseTheme: return "Choose PDF theme file";
    }
    return "";
}

// Starts an a{sv} entry: the key and the variant's signature.
void OpenOption(rgui_dbus_buf* body, const char* key, const char* signature) {
    rgui_dbus_open_struct(body);
    rgui_dbus_put_string(body, key);
    rgui_dbus_put_signature(body, signature);
}

// Moves `it` into the value of a variant, and of a variant inside it (Settings.Read).
bool EnterVariants(rgui_dbus_iter* it, rgui_dbus_iter* inner, rgui_dbus_iter* scratch) {
    if (rgui_dbus_iter_type(it) != 'v' || !rgui_dbus_recurse(it, inner)) return false;
    if (rgui_dbus_iter_type(inner) == 'v') {
        if (!rgui_dbus_recurse(inner, scratch)) return false;
        *inner = *scratch;
    }
    return true;
}

} // namespace

bool FileUriToPath(std::string_view uri, std::string& path) {
    static constexpr std::string_view kScheme = "file://";
    if (uri.size() < kScheme.size()) return false;
    for (size_t i = 0; i < kScheme.size(); ++i) {
        char c = uri[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kScheme[i]) return false;
    }
    std::string_view rest = uri.substr(kScheme.size());
    if (rest.substr(0, 10) == "localhost/") rest.remove_prefix(9);
    if (rest.empty() || rest[0] != '/') return false;
    std::string out;
    out.reserve(rest.size());
    for (size_t i = 0; i < rest.size(); ++i) {
        char c = rest[i];
        if (c == '%') {
            if (i + 2 >= rest.size()) return false;
            int hi = HexValue(rest[i + 1]);
            int lo = HexValue(rest[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = static_cast<char>(hi * 16 + lo);
            i += 2;
        }
        if (c == '\0') return false;
        out.push_back(c);
    }
    path = std::move(out);
    return true;
}

std::string RequestPath(std::string_view uniqueName, std::string_view token) {
    std::string path = "/org/freedesktop/portal/desktop/request/";
    for (char c : uniqueName) {
        if (c == ':') continue;
        path.push_back(c == '.' ? '_' : c);
    }
    path.push_back('/');
    path.append(token);
    return path;
}

struct Portal::Request {
    Portal* portal = nullptr;
    std::string handle;
    uint32_t match = 0;
    uint32_t call = 0;
    std::function<void(FileDialogResult)> callback;
};

struct Portal::KeyRead {
    SettingsRead* group = nullptr;
    const char* nameSpace = nullptr;
    const char* key = nullptr;
    bool usingRead = false;  // the fallback for portals without ReadOne
    bool done = false;
};

struct Portal::SettingsRead {
    Portal* portal = nullptr;
    KeyRead keys[2];
    std::function<void(Appearance)> done;
};

Portal::Portal() = default;
Portal::~Portal() { Disconnect(); }

void Portal::Disconnect() {
    // Closing runs no callbacks; the requests' own are dropped with them.
    rgui_dbus_close(conn_);
    conn_ = nullptr;
    settingsMatch_ = 0;
    ownerMatch_ = 0;
    portalOwner_.clear();
    requests_.clear();
    reads_.clear();
}

bool Portal::Connect(int timeoutMs) {
    Disconnect();
    conn_ = rgui_dbus_open_session_async(timeoutMs);
    return Started(timeoutMs);
}

bool Portal::ConnectTo(const char* address, int timeoutMs) {
    Disconnect();
    conn_ = rgui_dbus_open_async(address, timeoutMs);
    return Started(timeoutMs);
}

bool Portal::Started(int timeoutMs) {
    if (!conn_) return false;
    connectTimeoutMs_ = timeoutMs;
    // The portal's owner: changes from now on, then who owns it now.
    rgui_dbus_match changed{kBusName, "NameOwnerChanged", kBusPath, kBusName, kPortalName};
    ownerMatch_ = rgui_dbus_add_match(conn_, &changed, &Portal::OnNameOwnerChanged, this);
    rgui_dbus_buf body{};
    rgui_dbus_put_string(&body, kPortalName);
    rgui_dbus_message m{};
    m.destination = kBusName;
    m.path = kBusPath;
    m.interface = kBusName;
    m.member = "GetNameOwner";
    m.signature = "s";
    m.body = body.data;
    m.body_size = static_cast<uint32_t>(body.size);
    if (!body.failed) rgui_dbus_call(conn_, &m, kCallTimeoutMs, &Portal::OnNameOwner, this);
    rgui_dbus_buf_free(&body);
    return true;
}

void Portal::OnNameOwner(void* user, const rgui_dbus_message* reply) {
    auto* portal = static_cast<Portal*>(user);
    if (!reply || reply->type != RGUI_DBUS_METHOD_RETURN || std::strcmp(reply->signature, "s") != 0) return;
    rgui_dbus_iter it;
    rgui_dbus_body_iter(reply, &it);
    const char* owner;
    if (rgui_dbus_get_string(&it, &owner, nullptr) && owner[0] == ':') portal->portalOwner_ = owner;
}

void Portal::OnNameOwnerChanged(void* user, const rgui_dbus_message* signal) {
    auto* portal = static_cast<Portal*>(user);
    // Only the bus speaks for names (dbus_wire compares the sender of a unique or bus sender).
    if (!signal->sender || std::strcmp(signal->sender, kBusName) != 0 || std::strcmp(signal->signature, "sss") != 0) {
        return;
    }
    rgui_dbus_iter it;
    rgui_dbus_body_iter(signal, &it);
    const char* name;
    const char* oldOwner;
    const char* newOwner;
    if (!rgui_dbus_get_string(&it, &name, nullptr) || !rgui_dbus_get_string(&it, &oldOwner, nullptr) ||
        !rgui_dbus_get_string(&it, &newOwner, nullptr) || std::strcmp(name, kPortalName) != 0) {
        return;
    }
    portal->portalOwner_ = newOwner[0] == ':' ? newOwner : "";
}

void Portal::LearnOwner(const rgui_dbus_message* reply) {
    if (reply && reply->type == RGUI_DBUS_METHOD_RETURN && reply->sender && reply->sender[0] == ':') {
        portalOwner_ = reply->sender;
    }
}

bool Portal::FromPortal(const rgui_dbus_message* signal) const {
    return signal->sender && !portalOwner_.empty() && portalOwner_ == signal->sender;
}

int Portal::Fd() const { return conn_ ? rgui_dbus_fd(conn_) : -1; }
short Portal::PollEvents() const { return conn_ ? rgui_dbus_poll_events(conn_) : 0; }
int Portal::TimeoutMs() const { return conn_ ? rgui_dbus_timeout_ms(conn_) : -1; }

bool Portal::Dispatch() {
    if (!conn_) return false;
    if (rgui_dbus_dispatch(conn_)) return true;
    // The bus is gone. Calls in flight got their NULL replies; dialogs still waiting for their
    // Response never get one.
    while (!requests_.empty()) Finish(requests_.front().get(), {FileDialogResult::Failed, {}});
    Disconnect();
    return false;
}

// ---- Settings ------------------------------------------------------------------------------

bool Portal::ApplySetting(std::string_view nameSpace, std::string_view key, rgui_dbus_iter* value) {
    rgui_dbus_iter inner;
    rgui_dbus_iter scratch;
    if (nameSpace == kAppearance && key == kColorScheme) {
        uint32_t scheme = 0;
        if (!EnterVariants(value, &inner, &scratch) || rgui_dbus_iter_type(&inner) != 'u' ||
            !rgui_dbus_get_uint32(&inner, &scheme)) {
            return false;
        }
        bool dark = scheme != 2;
        bool changed = dark != appearance_.dark;
        appearance_.dark = dark;
        return changed;
    }
    if (nameSpace == kInterface && key == kTextScale) {
        double scale = 0;
        if (!EnterVariants(value, &inner, &scratch) || rgui_dbus_iter_type(&inner) != 'd' ||
            !rgui_dbus_get_double(&inner, &scale)) {
            return false;
        }
        if (!(scale > 0.0 && scale < 100.0)) return false;  // also rejects NaN
        bool changed = scale != appearance_.textScale;
        appearance_.textScale = scale;
        return changed;
    }
    return false;
}

bool Portal::ReadKey(KeyRead* key) {
    rgui_dbus_buf body{};
    rgui_dbus_put_string(&body, key->nameSpace);
    rgui_dbus_put_string(&body, key->key);
    rgui_dbus_message m{};
    m.destination = kPortalName;
    m.path = kPortalPath;
    m.interface = kSettings;
    m.member = key->usingRead ? "Read" : "ReadOne";
    m.signature = "ss";
    m.body = body.data;
    m.body_size = static_cast<uint32_t>(body.size);
    bool ok = !body.failed && rgui_dbus_call(conn_, &m, kCallTimeoutMs, &Portal::OnSettingReply, key) != 0;
    rgui_dbus_buf_free(&body);
    return ok;
}

void Portal::OnSettingReply(void* user, const rgui_dbus_message* reply) {
    auto* key = static_cast<KeyRead*>(user);
    Portal* portal = key->group->portal;
    if (reply && reply->type == RGUI_DBUS_ERROR && !key->usingRead && reply->error_name &&
        std::strcmp(reply->error_name, "org.freedesktop.DBus.Error.UnknownMethod") == 0) {
        key->usingRead = true;
        if (portal->ReadKey(key)) return;
    } else if (reply && reply->type == RGUI_DBUS_METHOD_RETURN && std::strcmp(reply->signature, "v") == 0) {
        portal->LearnOwner(reply);
        rgui_dbus_iter it;
        rgui_dbus_body_iter(reply, &it);
        portal->ApplySetting(key->nameSpace, key->key, &it);
    }
    portal->KeyDone(key);
}

void Portal::KeyDone(KeyRead* key) {
    key->done = true;
    SettingsRead* group = key->group;
    for (const KeyRead& k : group->keys) {
        if (!k.done) return;
    }
    std::function<void(Appearance)> done = std::move(group->done);
    reads_.erase(std::find_if(reads_.begin(), reads_.end(),
                              [group](const std::unique_ptr<SettingsRead>& r) { return r.get() == group; }));
    if (done) done(appearance_);
}

void Portal::ReadAppearance(std::function<void(Appearance)> done) {
    if (!conn_) {
        if (done) done(appearance_);
        return;
    }
    auto group = std::make_unique<SettingsRead>();
    group->portal = this;
    group->done = std::move(done);
    group->keys[0] = {group.get(), kAppearance, kColorScheme, false, false};
    group->keys[1] = {group.get(), kInterface, kTextScale, false, false};
    SettingsRead* raw = group.get();
    reads_.push_back(std::move(group));
    for (KeyRead& key : raw->keys) {
        if (!ReadKey(&key)) key.done = true;
    }
    // Nothing could be sent: answer now.
    if (raw->keys[0].done && raw->keys[1].done) KeyDone(&raw->keys[1]);
}

void Portal::OnSettingChanged(void* user, const rgui_dbus_message* signal) {
    auto* portal = static_cast<Portal*>(user);
    if (!portal->FromPortal(signal) || std::strcmp(signal->signature, "ssv") != 0) return;
    rgui_dbus_iter it;
    rgui_dbus_body_iter(signal, &it);
    const char* nameSpace;
    const char* key;
    uint32_t nameSpaceLength;
    uint32_t keyLength;
    if (!rgui_dbus_get_string(&it, &nameSpace, &nameSpaceLength) || !rgui_dbus_get_string(&it, &key, &keyLength)) return;
    if (portal->ApplySetting(std::string_view(nameSpace, nameSpaceLength), std::string_view(key, keyLength), &it) &&
        portal->changed_) {
        portal->changed_(portal->appearance_);
    }
}

void Portal::WatchAppearance(std::function<void(Appearance)> changed) {
    changed_ = std::move(changed);
    if (!conn_ || settingsMatch_) return;
    rgui_dbus_match match{kSettings, "SettingChanged", kPortalPath, kPortalName, nullptr};
    settingsMatch_ = rgui_dbus_add_match(conn_, &match, &Portal::OnSettingChanged, this);
}

// ---- FileChooser ---------------------------------------------------------------------------

void Portal::Finish(Request* request, FileDialogResult result) {
    if (conn_) {
        rgui_dbus_remove_match(conn_, request->match);
        if (request->call) rgui_dbus_cancel(conn_, request->call);
    }
    std::function<void(FileDialogResult)> callback = std::move(request->callback);
    auto it = std::find_if(requests_.begin(), requests_.end(),
                           [request](const std::unique_ptr<Request>& r) { return r.get() == request; });
    if (it != requests_.end()) requests_.erase(it);
    if (callback) callback(std::move(result));
}

void Portal::OnFileReply(void* user, const rgui_dbus_message* reply) {
    auto* request = static_cast<Request*>(user);
    Portal* portal = request->portal;
    request->call = 0;
    const char* handle = nullptr;
    rgui_dbus_iter it;
    if (reply && reply->type == RGUI_DBUS_METHOD_RETURN && std::strcmp(reply->signature, "o") == 0) {
        portal->LearnOwner(reply);
        rgui_dbus_body_iter(reply, &it);
        if (!rgui_dbus_get_string(&it, &handle, nullptr)) handle = nullptr;
    }
    if (!handle) {
        portal->Finish(request, {FileDialogResult::Unavailable, {}});
        return;
    }
    if (request->handle != handle) {
        // A portal older than handle_token (before 0.9) picks its own request path.
        rgui_dbus_remove_match(portal->conn_, request->match);
        request->handle = handle;
        rgui_dbus_match match{"org.freedesktop.portal.Request", "Response", request->handle.c_str(), kPortalName,
                              nullptr};
        request->match = rgui_dbus_add_match(portal->conn_, &match, &Portal::OnFileResponse, request);
        if (!request->match) portal->Finish(request, {FileDialogResult::Unavailable, {}});
    }
}

void Portal::OnFileResponse(void* user, const rgui_dbus_message* signal) {
    auto* request = static_cast<Request*>(user);
    Portal* portal = request->portal;
    // Anyone may send us a signal with this path; only the portal's Response counts.
    if (!portal->FromPortal(signal)) return;
    FileDialogResult result{FileDialogResult::Failed, {}};
    rgui_dbus_iter it;
    rgui_dbus_body_iter(signal, &it);
    uint32_t code = 2;
    if (std::strcmp(signal->signature, "ua{sv}") == 0 && rgui_dbus_get_uint32(&it, &code)) {
        if (code == 1) {
            result.kind = FileDialogResult::Cancelled;
        } else if (code == 0) {
            // results: find "uris" (as) and take the first.
            rgui_dbus_iter dict;
            if (rgui_dbus_recurse(&it, &dict)) {
                while (rgui_dbus_iter_type(&dict)) {
                    rgui_dbus_iter entry;
                    const char* key;
                    if (!rgui_dbus_recurse(&dict, &entry) || !rgui_dbus_get_string(&entry, &key, nullptr)) break;
                    rgui_dbus_iter value;
                    if (std::strcmp(key, "uris") == 0 && rgui_dbus_recurse(&entry, &value) &&
                        value.sig_end - value.sig == 2 && std::memcmp(value.sig, "as", 2) == 0) {
                        rgui_dbus_iter uris;
                        const char* uri;
                        uint32_t length;
                        if (rgui_dbus_recurse(&value, &uris) && rgui_dbus_iter_type(&uris) == 's' &&
                            rgui_dbus_get_string(&uris, &uri, &length) &&
                            FileUriToPath(std::string_view(uri, length), result.path)) {
                            result.kind = FileDialogResult::Chosen;
                        }
                        break;
                    }
                    if (!rgui_dbus_leave(&dict, &entry)) break;
                }
            }
        }
    }
    portal->Finish(request, std::move(result));
}

void Portal::ShowFileDialog(FileDialog kind, const std::string& suggestedPath,
                            std::function<void(FileDialogResult)> callback) {
    // The request path holds our unique name: the handshake must be over.
    if (!conn_ || !rgui_dbus_wait_ready(conn_, connectTimeoutMs_)) {
        if (callback) callback({FileDialogResult::Unavailable, {}});
        return;
    }
    auto request = std::make_unique<Request>();
    request->portal = this;
    request->callback = std::move(callback);
    std::string token = RandomToken();
    request->handle = RequestPath(rgui_dbus_unique_name(conn_), token);
    Request* raw = request.get();
    requests_.push_back(std::move(request));

    // Subscribe to the Response before calling, so it cannot be missed.
    rgui_dbus_match match{"org.freedesktop.portal.Request", "Response", raw->handle.c_str(), kPortalName, nullptr};
    raw->match = rgui_dbus_add_match(conn_, &match, &Portal::OnFileResponse, raw);
    if (!raw->match) {
        Finish(raw, {FileDialogResult::Unavailable, {}});
        return;
    }

    const bool save = kind == FileDialog::SaveMarkdown || kind == FileDialog::ExportPdf;
    rgui_dbus_buf body{};
    rgui_dbus_put_string(&body, "");  // no parent window
    rgui_dbus_put_string(&body, TitleFor(kind));
    rgui_dbus_array options = rgui_dbus_open_array(&body, '{');
    OpenOption(&body, "handle_token", "s");
    rgui_dbus_put_string(&body, token.c_str());
    OpenOption(&body, "modal", "b");
    rgui_dbus_put_bool(&body, true);
    OpenOption(&body, "filters", "a(sa(us))");
    rgui_dbus_array filters = rgui_dbus_open_array(&body, '(');
    for (const Filter& filter : FiltersFor(kind)) {
        rgui_dbus_open_struct(&body);
        rgui_dbus_put_string(&body, filter.name);
        rgui_dbus_array globs = rgui_dbus_open_array(&body, '(');
        for (const char* glob : filter.globs) {
            rgui_dbus_open_struct(&body);
            rgui_dbus_put_uint32(&body, 0);  // a glob pattern
            rgui_dbus_put_string(&body, glob);
        }
        rgui_dbus_close_array(&body, globs);
    }
    rgui_dbus_close_array(&body, filters);
    if (save && !suggestedPath.empty()) {
        std::filesystem::path suggested(suggestedPath);
        std::string name = suggested.filename().string();
        if (!name.empty() && IsDbusString(name)) {
            OpenOption(&body, "current_name", "s");
            rgui_dbus_put_string(&body, name.c_str());
        }
        std::string folder = suggested.parent_path().string();
        std::error_code error;
        if (!folder.empty() && folder.find('\0') == std::string::npos &&
            std::filesystem::is_directory(suggested.parent_path(), error)) {
            OpenOption(&body, "current_folder", "ay");
            rgui_dbus_array bytes = rgui_dbus_open_array(&body, 'y');
            for (char c : folder) rgui_dbus_put_byte(&body, static_cast<uint8_t>(c));
            rgui_dbus_put_byte(&body, 0);
            rgui_dbus_close_array(&body, bytes);
        }
    }
    rgui_dbus_close_array(&body, options);

    rgui_dbus_message m{};
    m.destination = kPortalName;
    m.path = kPortalPath;
    m.interface = kFileChooser;
    m.member = save ? "SaveFile" : "OpenFile";
    m.signature = "ssa{sv}";
    m.body = body.data;
    m.body_size = static_cast<uint32_t>(body.size);
    raw->call = body.failed ? 0 : rgui_dbus_call(conn_, &m, kCallTimeoutMs, &Portal::OnFileReply, raw);
    rgui_dbus_buf_free(&body);
    if (!raw->call) Finish(raw, {FileDialogResult::Unavailable, {}});
}

} // namespace rgui::dbus
