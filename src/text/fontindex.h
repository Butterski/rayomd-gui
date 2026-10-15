// SPDX-License-Identifier: Apache-2.0
// The system's fonts: every face of every font file in the font directories, scanned once on a
// thread at startup.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rgui::text {

struct FontEntry {
    std::string path;                    // UTF-8
    uint32_t faceIndex = 0;
    std::vector<std::string> families;   // nameID 16, then nameID 1
    std::string style;                   // nameID 17, else 2
    uint16_t weight = 400;
    uint16_t widthClass = 5;
    bool italic = false;
    bool fixedPitch = false;
};

class FontIndex {
public:
    // At most this many font files are read, and directories this deep below a root.
    static constexpr size_t kMaxFiles = 8192;
    static constexpr int kMaxDepth = 5;

    FontIndex();
    ~FontIndex();   // waits for a running scan
    FontIndex(const FontIndex&) = delete;
    FontIndex& operator=(const FontIndex&) = delete;

    // Linux: $XDG_DATA_HOME/fonts (default ~/.local/share/fonts), ~/.fonts,
    // /usr/local/share/fonts, /usr/share/fonts. Windows: %WINDIR%\Fonts,
    // %LOCALAPPDATA%\Microsoft\Windows\Fonts.
    static std::vector<std::string> DefaultDirectories();
    // Replaces the directories to scan (tests); only before Start.
    void SetDirectories(std::vector<std::string> directories);

    // Starts the scan on a thread; later calls do nothing.
    void Start();
    // Waits for the scan, starting it on this thread when Start was never called. After it
    // returns, the index never changes, so any number of threads may read it at once.
    void Wait() const;
    // Whether the scan has finished (never waits).
    bool Scanned() const;

    // Every face found, in directory order, then by path within a directory (waits).
    const std::vector<FontEntry>& Entries() const;
    // The face of `family` (ASCII case-insensitive, against any of its families) whose weight
    // is nearest `weight`; on a tie a face whose italic matches wins, then the width class
    // nearest normal, then the earlier entry. nullptr when the family has no face (waits).
    const FontEntry* Find(std::string_view family, int weight, bool italic) const;
    bool HasFamily(std::string_view family) const;

    // How long the scan took, in milliseconds (waits).
    double ScanMilliseconds() const;

private:
    // Reads every font file into entries_, then marks the index finished.
    void Scan() const;

    mutable std::mutex mutex_;
    mutable std::condition_variable done_;
    mutable bool started_ = false;
    mutable bool finished_ = false;
    std::thread thread_;
    std::vector<std::string> directories_;
    mutable std::vector<FontEntry> entries_;
    mutable double scanMs_ = 0;
};

// ASCII case-insensitive equality, for family names.
bool FamilyEquals(std::string_view a, std::string_view b);

} // namespace rgui::text
