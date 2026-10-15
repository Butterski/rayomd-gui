// SPDX-License-Identifier: Apache-2.0
#include "ui/settingsfile.h"

#include "app/safefile.h"
#include "core/rayomd_pdf_source.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace rgui::ui {

namespace {

bool IsControl(unsigned char c) { return c < 0x20 || c == 0x7F; }

bool ValidKey(std::string_view key) {
    if (key.empty() || key.front() == '#') return false;
    for (unsigned char c : key) {
        if (IsControl(c) || c == ' ' || c == '=') return false;
    }
    return RayoMd::PdfSource::IsValidUtf8(key);
}

bool ValidValue(std::string_view value) {
    for (unsigned char c : value) {
        if (IsControl(c)) return false;
    }
    return RayoMd::PdfSource::IsValidUtf8(value);
}

} // namespace

bool SettingsFile::Load(const std::filesystem::path& path) {
    values_.clear();
    std::string text;
    if (!ReadRegularFile(path, kMaxBytes, text, nullptr)) return false;
    if (!RayoMd::PdfSource::IsValidUtf8(text)) return false;
    std::string_view rest = text;
    while (!rest.empty()) {
        const size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);
        if (!ValidKey(key) || !ValidValue(value)) continue;
        values_[std::string(key)].assign(value);
    }
    return true;
}

bool SettingsFile::Save(const std::filesystem::path& path) const {
    std::string text;
    for (const auto& [key, value] : values_) {
        text += key;
        text += '=';
        text += value;
        text += '\n';
    }
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
    }
    return WriteFileAtomically(path, text, WriteMode::Output, nullptr);
}

bool SettingsFile::Has(std::string_view key) const { return values_.find(key) != values_.end(); }

void SettingsFile::Erase(std::string_view key) {
    const auto it = values_.find(key);
    if (it != values_.end()) values_.erase(it);
}

std::string SettingsFile::GetString(std::string_view key, std::string_view fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? std::string(fallback) : it->second;
}

int SettingsFile::GetInt(std::string_view key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    const std::string& s = it->second;
    int value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    return error == std::errc() && end == s.data() + s.size() ? value : fallback;
}

double SettingsFile::GetDouble(std::string_view key, double fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    const std::string& s = it->second;
    // strtod, not std::from_chars: libstdc++'s floating-point from_chars makes the linker mark
    // libm.so.6 as NEEDED (fesetround), and the Linux executable needs libc only. The settings
    // are written in the C locale, which is the one strtod uses here (setlocale is never called).
    // Only plain decimal numbers are accepted, as from_chars would.
    if (s.empty()) return fallback;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+')) return fallback;
    }
    if (s[0] == '+') return fallback;
    char* end = nullptr;
    const double value = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return fallback;
    // Infinities and NaN are never written; refuse them as hand edits.
    if (!(value == value) || value > 1.7976931348623157e308 || value < -1.7976931348623157e308) return fallback;
    return value;
}

bool SettingsFile::GetBool(std::string_view key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    if (it->second == "true" || it->second == "1") return true;
    if (it->second == "false" || it->second == "0") return false;
    return fallback;
}

bool SettingsFile::SetString(std::string_view key, std::string_view value) {
    if (!ValidKey(key) || !ValidValue(value)) return false;
    values_[std::string(key)].assign(value);
    return true;
}

bool SettingsFile::SetInt(std::string_view key, int value) {
    char buffer[16];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return SetString(key, std::string_view(buffer, static_cast<size_t>(result.ptr - buffer)));
}

bool SettingsFile::SetDouble(std::string_view key, double value) {
    if (!(value == value) || value > 1.7976931348623157e308 || value < -1.7976931348623157e308) return false;
    // The shortest %.*g that reads back as the same value, through snprintf / strtod: libstdc++'s
    // floating-point to_chars would make libm.so.6 NEEDED (see GetDouble).
    char buffer[32];
    int length = 0;
    for (int precision = 1; precision <= 17; ++precision) {
        length = std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        if (length <= 0 || length >= static_cast<int>(sizeof buffer)) return false;
        if (std::strtod(buffer, nullptr) == value) break;
    }
    return SetString(key, std::string_view(buffer, static_cast<size_t>(length)));
}

bool SettingsFile::SetBool(std::string_view key, bool value) { return SetString(key, value ? "true" : "false"); }

} // namespace rgui::ui
