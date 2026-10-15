// SPDX-License-Identifier: Apache-2.0
// The settings file: UTF-8 `key=value` lines, `#` starts a comment line. Saving is atomic.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace rgui::ui {

class SettingsFile {
public:
    static constexpr size_t kMaxBytes = 1024 * 1024;

    // Replaces the values with the file's. False (and no values) when the file is missing,
    // not a regular file, unreadable, over kMaxBytes or not UTF-8. Lines without `=`, with an empty or invalid key
    // (whitespace, control characters) or with control characters in the value are ignored;
    // a later key wins. CR before LF is dropped.
    bool Load(const std::filesystem::path& path);

    // Writes the values, sorted by key, to a temporary file renamed over `path`
    // (WriteFileAtomically, WriteMode::Output); creates the parent directories. False when any
    // step fails (no temporary file is left).
    bool Save(const std::filesystem::path& path) const;

    bool Has(std::string_view key) const;
    void Erase(std::string_view key);
    void Clear() { values_.clear(); }

    // The value of `key`, or `fallback` when it is missing or does not parse.
    std::string GetString(std::string_view key, std::string_view fallback = {}) const;
    int GetInt(std::string_view key, int fallback) const;
    double GetDouble(std::string_view key, double fallback) const;
    bool GetBool(std::string_view key, bool fallback) const;

    // False (and nothing stored) for a key that is empty or holds `=`, `#` first, whitespace or
    // control characters, or for a value with control characters or invalid UTF-8.
    bool SetString(std::string_view key, std::string_view value);
    bool SetInt(std::string_view key, int value);
    bool SetDouble(std::string_view key, double value);  // shortest round-trip form
    bool SetBool(std::string_view key, bool value);      // "true" / "false"

    const std::map<std::string, std::string, std::less<>>& Values() const { return values_; }

private:
    std::map<std::string, std::string, std::less<>> values_;
};

} // namespace rgui::ui
