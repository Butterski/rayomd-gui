// SPDX-License-Identifier: Apache-2.0
// The Compose reader: libX11's Compose(5) format into a trie, with libxkbcommon's rules for
// conflicting lines (a longer sequence replaces a shorter one that is its prefix; a sequence that
// is a prefix of an existing one is skipped; a repeated sequence replaces the earlier result).
#include "platform/linux/compose.h"

#include "app/safefile.h"
#include "platform/linux/xkb_keymap.h"

#include <cstdlib>
#include <string_view>

#include <unistd.h>

namespace rgui::xkb {
namespace {

constexpr size_t kMaxFileBytes = 4u << 20;
constexpr size_t kMaxDirBytes = 1u << 20;
constexpr int kMaxIncludeDepth = 4;
constexpr int kMaxFiles = 32;
constexpr size_t kMaxNodes = 1u << 20;
constexpr size_t kMaxResultBytes = 16u << 20;
constexpr size_t kMaxSequence = 16;
constexpr size_t kMaxResult = 256;
constexpr uint32_t kMultiKey = 0xff20;

bool IsModifierKeysym(uint32_t keysym) {
    return (keysym >= 0xffe1 && keysym <= 0xffee) ||  // Shift_L .. Hyper_R
           (keysym >= 0xfe01 && keysym <= 0xfe13) ||  // ISO_Lock .. ISO_Level5_Lock
           keysym == 0xff7e || keysym == 0xff7f;      // Mode_switch, Num_Lock
}

bool IsDeadKeysym(uint32_t keysym) {
    return keysym >= 0xfe50 && keysym <= 0xfe93;
}

// A Compose file (or one it includes, or compose.dir): a regular file of at most `limit` bytes,
// read without blocking on a FIFO or device at the path; `out` empty when it is not.
bool ReadFile(const std::string& path, size_t limit, std::string& out) {
    out.clear();
    return ReadRegularFile(path, limit, out, nullptr);
}

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n;
        uint32_t cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xc2 && c <= 0xdf) {
            n = 1;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 2;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + n >= s.size()) return false;  // the continuation bytes are s[i + 1 .. i + n]
        for (size_t k = 1; k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3f);
        }
        if ((n == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff)))
            return false;
        i += n + 1;
    }
    return true;
}

bool IsUtf8Codeset(std::string_view codeset) {
    std::string c;
    for (char ch : codeset) {
        if (ch == '-' || ch == '_') continue;
        c.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return c == "utf8";
}

// The current locale name, as libX11 looks it up.
std::string CurrentLocale() {
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        std::string v = Env(name);
        if (!v.empty()) return v;
    }
    return "en_US.UTF-8";
}

// The locale's Compose file from <localeDir>/compose.dir. Our text is UTF-8, so a locale is
// looked up by its UTF-8 variant (pl_PL.ISO-8859-2 -> pl_PL.UTF-8); C and POSIX are en_US.UTF-8.
std::string LocaleComposeFile(const std::string& localeDir, const std::string& locale) {
    std::string dir;
    if (!ReadFile(localeDir + "/compose.dir", kMaxDirBytes, dir)) return std::string();
    std::string language = locale.substr(0, locale.find_first_of(".@"));
    std::string candidates[3];
    size_t count = 0;
    const size_t dot = locale.find('.');
    if (dot != std::string::npos) {
        const size_t at = locale.find('@', dot);
        const std::string_view codeset = std::string_view(locale).substr(dot + 1, at == std::string::npos ? std::string::npos : at - dot - 1);
        if (IsUtf8Codeset(codeset)) candidates[count++] = locale;
    }
    if (language == "C" || language == "POSIX" || language.empty()) language = "en_US";
    candidates[count++] = language + ".UTF-8";
    candidates[count++] = "en_US.UTF-8";
    for (size_t c = 0; c < count; ++c) {
        size_t pos = 0;
        while (pos < dir.size()) {
            size_t end = dir.find('\n', pos);
            if (end == std::string::npos) end = dir.size();
            std::string_view line(dir.data() + pos, end - pos);
            pos = end + 1;
            if (line.empty() || line[0] == '#') continue;
            const size_t ws = line.find_first_of(" \t");
            if (ws == std::string_view::npos) continue;
            std::string_view file = line.substr(0, ws);
            if (!file.empty() && file.back() == ':') file.remove_suffix(1);
            std::string_view name = line.substr(ws);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r'))
                name.remove_suffix(1);
            if (name == candidates[c] && !file.empty()) return localeDir + "/" + std::string(file);
        }
    }
    return std::string();
}

class Loader {
public:
    Loader(std::vector<Compose::Node>& nodes, std::string& results, std::string localeDir)
        : nodes_(nodes), results_(results), localeDir_(std::move(localeDir)) {}

    bool LoadFile(const std::string& path, int depth);

private:
    void ParseLine(std::string_view line, int depth);
    void Add(const uint32_t* sequence, size_t length, std::string_view result);
    bool ExpandInclude(std::string_view raw, std::string& path);

    std::vector<Compose::Node>& nodes_;
    std::string& results_;
    std::string localeDir_;
    int files_ = 0;
};

bool Loader::LoadFile(const std::string& path, int depth) {
    if (depth > kMaxIncludeDepth || files_ >= kMaxFiles) return false;
    ++files_;
    std::string text;
    if (!ReadFile(path, kMaxFileBytes, text)) return false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        ParseLine(std::string_view(text.data() + pos, end - pos), depth);
        pos = end + 1;
    }
    return true;
}

bool Loader::ExpandInclude(std::string_view raw, std::string& path) {
    path.clear();
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '%') {
            path.push_back(raw[i]);
            continue;
        }
        if (++i >= raw.size()) return false;
        switch (raw[i]) {
        case '%': path.push_back('%'); break;
        case 'H': {
            const std::string home = Env("HOME");
            if (home.empty()) return false;
            path += home;
            break;
        }
        case 'L': {
            const std::string file = LocaleComposeFile(localeDir_, CurrentLocale());
            if (file.empty()) return false;
            path += file;
            break;
        }
        case 'S': path += localeDir_; break;
        default: return false;
        }
        if (path.size() > 4096) return false;
    }
    return !path.empty();
}

// Reads a "string" with its escapes (\\ \" \n \t \r, octal \123, hex \x41) at `i`.
bool ParseString(std::string_view line, size_t& i, std::string& out) {
    out.clear();
    ++i;  // '"'
    while (i < line.size() && line[i] != '"') {
        if (out.size() > kMaxResult) return false;
        const char c = line[i++];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i >= line.size()) return false;
        const char e = line[i++];
        if (e == 'n') out.push_back('\n');
        else if (e == 't') out.push_back('\t');
        else if (e == 'r') out.push_back('\r');
        else if (e >= '0' && e <= '7') {
            unsigned v = static_cast<unsigned>(e - '0');
            for (int d = 0; d < 2 && i < line.size() && line[i] >= '0' && line[i] <= '7'; ++d)
                v = v * 8 + static_cast<unsigned>(line[i++] - '0');
            if (v == 0 || v > 0xff) return false;
            out.push_back(static_cast<char>(v));
        } else if (e == 'x' || e == 'X') {
            unsigned v = 0;
            int digits = 0;
            while (digits < 2 && i < line.size()) {
                const char h = line[i];
                int d;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                else break;
                v = v * 16 + static_cast<unsigned>(d);
                ++digits;
                ++i;
            }
            if (digits == 0 || v == 0) return false;
            out.push_back(static_cast<char>(v));
        } else {
            out.push_back(e);
        }
    }
    if (i >= line.size()) return false;
    ++i;  // '"'
    return true;
}

void SkipSpace(std::string_view line, size_t& i) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
}

bool IsWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void Loader::ParseLine(std::string_view line, int depth) {
    size_t i = 0;
    SkipSpace(line, i);
    if (i >= line.size() || line[i] == '#') return;
    if (line.compare(i, 7, "include") == 0) {
        i += 7;
        SkipSpace(line, i);
        std::string raw;
        if (i >= line.size() || line[i] != '"' || !ParseString(line, i, raw)) return;
        std::string path;
        if (ExpandInclude(raw, path)) LoadFile(path, depth + 1);
        return;
    }
    // Left side: [!] [modifiers] <keysym> ... ; modifiers are ignored, as libxkbcommon does.
    uint32_t sequence[kMaxSequence];
    size_t length = 0;
    for (;;) {
        SkipSpace(line, i);
        if (i >= line.size()) return;
        const char c = line[i];
        if (c == ':') {
            ++i;
            break;
        }
        if (c == '<') {
            const size_t close = line.find('>', i + 1);
            if (close == std::string_view::npos || close == i + 1) return;
            const uint32_t keysym = KeysymFromName(line.substr(i + 1, close - i - 1));
            if (keysym == 0 || length >= kMaxSequence) return;
            sequence[length++] = keysym;
            i = close + 1;
        } else if (c == '!' || c == '~') {
            ++i;
        } else if (IsWordChar(c)) {
            while (i < line.size() && IsWordChar(line[i])) ++i;  // None, Ctrl, Shift, ...
        } else {
            return;
        }
    }
    if (length == 0) return;
    // Right side: ["string"] [keysym] [# comment]
    SkipSpace(line, i);
    std::string result;
    if (i < line.size() && line[i] == '"') {
        if (!ParseString(line, i, result)) return;
        if (!IsValidUtf8(result)) return;
        SkipSpace(line, i);
    }
    if (i < line.size() && IsWordChar(line[i])) {
        const size_t start = i;
        while (i < line.size() && IsWordChar(line[i])) ++i;
        const uint32_t keysym = KeysymFromName(line.substr(start, i - start));
        if (keysym == 0) return;
        if (result.empty()) {
            const uint32_t cp = KeysymToCodepoint(keysym);
            if (cp == 0) return;  // a result that types nothing
            AppendUtf8(result, cp);
        }
        SkipSpace(line, i);
    }
    if (i < line.size() && line[i] != '#') return;
    if (result.empty()) return;
    Add(sequence, length, result);
}

void Loader::Add(const uint32_t* sequence, size_t length, std::string_view result) {
    uint32_t node = 0;
    for (size_t k = 0; k < length; ++k) {
        uint32_t child = nodes_[node].firstChild;
        while (child != 0 && nodes_[child].keysym != sequence[k]) child = nodes_[child].nextSibling;
        if (child == 0) {
            if (nodes_.size() >= kMaxNodes) return;
            Compose::Node fresh;
            fresh.keysym = sequence[k];
            fresh.nextSibling = nodes_[node].firstChild;
            child = static_cast<uint32_t>(nodes_.size());
            nodes_.push_back(fresh);
            nodes_[node].firstChild = child;
        }
        if (k + 1 < length) {
            if (nodes_[child].leaf) {
                // A shorter sequence that is a prefix of this one: this one replaces it.
                nodes_[child].leaf = false;
                nodes_[child].resultLength = 0;
            }
        } else {
            if (!nodes_[child].leaf && nodes_[child].firstChild != 0) return;  // prefix of another
            if (results_.size() + result.size() > kMaxResultBytes) return;
            nodes_[child].leaf = true;
            nodes_[child].resultOffset = static_cast<uint32_t>(results_.size());
            nodes_[child].resultLength = static_cast<uint32_t>(result.size());
            results_.append(result.data(), result.size());
        }
        node = child;
    }
}

} // namespace

bool Compose::Load(const std::string& path, const std::string& localeDir) {
    loaded_ = true;
    current_ = 0;
    nodes_.assign(1, Node());
    results_.clear();
    Loader loader(nodes_, results_, localeDir);
    const bool ok = loader.LoadFile(path, 0);
    if (!ok) {
        nodes_.assign(1, Node());
        results_.clear();
    }
    return ok;
}

void Compose::LoadDefault() {
    static const char kLocaleDir[] = "/usr/share/X11/locale";
    std::string path = Env("XCOMPOSEFILE");
    if (path.empty()) {
        const std::string home = Env("HOME");
        if (!home.empty() && access((home + "/.XCompose").c_str(), R_OK) == 0) path = home + "/.XCompose";
    }
    if (path.empty()) path = LocaleComposeFile(kLocaleDir, CurrentLocale());
    if (path.empty() || !Load(path, kLocaleDir)) {
        loaded_ = true;
        nodes_.assign(1, Node());
        results_.clear();
    }
}

Compose::Result Compose::Feed(uint32_t keysym, std::string& text) {
    text.clear();
    if (keysym == 0 || IsModifierKeysym(keysym)) return Result::Ignored;
    if (!loaded_ && (keysym == kMultiKey || IsDeadKeysym(keysym))) LoadDefault();
    if (nodes_.empty()) return Result::Ignored;
    uint32_t child = nodes_[current_].firstChild;
    while (child != 0 && nodes_[child].keysym != keysym) child = nodes_[child].nextSibling;
    if (child == 0) {
        if (current_ == 0) return Result::Ignored;
        current_ = 0;
        return Result::Cancelled;
    }
    const Node& node = nodes_[child];
    if (node.leaf) {
        current_ = 0;
        text.assign(results_, node.resultOffset, node.resultLength);
        return Result::Composed;
    }
    current_ = child;
    return Result::Composing;
}

void Compose::Reset() {
    current_ = 0;
}

} // namespace rgui::xkb
