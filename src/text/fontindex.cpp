// SPDX-License-Identifier: Apache-2.0
#include "text/fontindex.h"

#include "text/mapped_file.h"
#include "text/sfnt.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rgui::text {
namespace {

namespace fs = std::filesystem;

char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsFontFile(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = LowerAscii(c);
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

// An environment variable as UTF-8, or "" when unset.
std::string Env(const char* name) {
#ifdef _WIN32
    std::wstring wide;
    for (const char* p = name; *p != 0; p++) wide += static_cast<wchar_t>(*p);
    const wchar_t* const value = _wgetenv(wide.c_str());
    if (value == nullptr || *value == 0) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), length, nullptr, nullptr);
    out.resize(static_cast<size_t>(length) - 1);
    return out;
#else
    const char* const value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

} // namespace

bool FamilyEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

FontIndex::FontIndex() : directories_(DefaultDirectories()) {}

FontIndex::~FontIndex() {
    if (thread_.joinable()) thread_.join();
}

std::vector<std::string> FontIndex::DefaultDirectories() {
    std::vector<std::string> dirs;
#ifdef _WIN32
    const std::string windir = Env("WINDIR");
    if (!windir.empty()) dirs.push_back(windir + "\\Fonts");
    const std::string local = Env("LOCALAPPDATA");
    if (!local.empty()) dirs.push_back(local + "\\Microsoft\\Windows\\Fonts");
#else
    const std::string home = Env("HOME");
    const std::string dataHome = Env("XDG_DATA_HOME");
    if (!dataHome.empty() && dataHome[0] == '/') dirs.push_back(dataHome + "/fonts");
    else if (!home.empty()) dirs.push_back(home + "/.local/share/fonts");
    if (!home.empty()) dirs.push_back(home + "/.fonts");
    dirs.push_back("/usr/local/share/fonts");
    dirs.push_back("/usr/share/fonts");
#endif
    return dirs;
}

void FontIndex::SetDirectories(std::vector<std::string> directories) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) directories_ = std::move(directories);
}

void FontIndex::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    started_ = true;
    thread_ = std::thread([this] { Scan(); });
}

void FontIndex::Wait() const {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) {
        started_ = true;
        lock.unlock();
        Scan();
        return;
    }
    done_.wait(lock, [this] { return finished_; });
}

bool FontIndex::Scanned() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
}

void FontIndex::Scan() const {
    const auto begin = std::chrono::steady_clock::now();
    std::vector<std::string> files;
    std::unordered_set<std::string> seen;
    for (const std::string& root : directories_) {
        if (files.size() >= kMaxFiles) break;
        std::error_code ec;
        fs::recursive_directory_iterator it(fs::u8path(root), fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        std::vector<std::string> local;
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() >= kMaxDepth - 1) it.disable_recursion_pending();
            std::error_code typeError;
            if (!it->is_regular_file(typeError)) continue;
            std::string path = it->path().u8string();
            if (!IsFontFile(path)) continue;
            local.push_back(std::move(path));
            if (files.size() + local.size() >= kMaxFiles) break;
        }
        std::sort(local.begin(), local.end());
        for (std::string& path : local) {
            if (seen.insert(path).second) files.push_back(std::move(path));
        }
    }

    std::vector<FontEntry> entries;
    for (const std::string& path : files) {
        MappedFile file;
        if (!file.Open(path)) continue;
        const uint32_t faces = FaceCount(file.Data(), file.Size());
        for (uint32_t face = 0; face < faces; face++) {
            FaceInfo info;
            if (!ReadFaceInfo(file.Data(), file.Size(), face, info)) continue;
            FontEntry entry;
            entry.path = path;
            entry.faceIndex = face;
            entry.families = std::move(info.families);
            entry.style = std::move(info.style);
            entry.weight = info.weight;
            entry.widthClass = info.widthClass;
            entry.italic = info.italic;
            entry.fixedPitch = info.fixedPitch;
            entries.push_back(std::move(entry));
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

    std::lock_guard<std::mutex> lock(mutex_);
    entries_ = std::move(entries);
    scanMs_ = ms;
    finished_ = true;
    done_.notify_all();
}

const std::vector<FontEntry>& FontIndex::Entries() const {
    Wait();
    return entries_;
}

const FontEntry* FontIndex::Find(std::string_view family, int weight, bool italic) const {
    Wait();
    const FontEntry* best = nullptr;
    int bestWeight = 0, bestItalic = 0, bestWidth = 0;
    for (const FontEntry& entry : entries_) {
        bool match = false;
        for (const std::string& name : entry.families) match = match || FamilyEquals(name, family);
        if (!match) continue;
        const int weightDistance = entry.weight > weight ? entry.weight - weight : weight - entry.weight;
        const int italicDistance = entry.italic == italic ? 0 : 1;
        const int widthDistance = entry.widthClass > 5 ? entry.widthClass - 5 : 5 - entry.widthClass;
        if (best == nullptr || weightDistance < bestWeight ||
            (weightDistance == bestWeight &&
                (italicDistance < bestItalic || (italicDistance == bestItalic && widthDistance < bestWidth)))) {
            best = &entry;
            bestWeight = weightDistance;
            bestItalic = italicDistance;
            bestWidth = widthDistance;
        }
    }
    return best;
}

bool FontIndex::HasFamily(std::string_view family) const {
    return Find(family, 400, false) != nullptr;
}

double FontIndex::ScanMilliseconds() const {
    Wait();
    return scanMs_;
}

} // namespace rgui::text
