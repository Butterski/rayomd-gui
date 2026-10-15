// SPDX-License-Identifier: Apache-2.0
// The open document: its Markdown (LF line ends only), where it lives, and how it is saved.
#pragma once

#include <cstddef>
#include <string>

namespace rgui {

struct DocumentStats {
    size_t lines = 1;  // 1 + the number of '\n'
    size_t words = 0;  // runs of bytes other than space, tab, '\n' and '\r'
};

class Document {
public:
    static constexpr size_t kMaxTextBytes = 128u * 1024u * 1024u;
    static constexpr const char* kUntitledName = "untitled.md";

    std::string text;         // UTF-8, '\n' line ends only
    std::string filePath;     // UTF-8; empty for a new document or one recovered from a PDF
    std::string displayName = kUntitledName;  // the file name the title shows
    bool modified = false;    // edited since it was opened or saved
    bool crlf = false;        // the file had CRLF line ends; Save writes them back

    // An empty, unnamed document.
    void New();

    // Opens `path` (UTF-8). A `.pdf` (any case) is a reversible RayoMD PDF: its embedded source
    // becomes the text, with no file path (saving asks for one) and the name `<pdf base>.md`.
    // Any other file is UTF-8 text, at most kMaxTextBytes; CRLF line ends are read as LF and
    // remembered. Either way `status` is what to tell the user (empty after a plain open); on
    // failure the document is kept as it was and false is returned.
    bool Open(const std::string& path, std::string& status);

    // Writes the text to `path` (UTF-8), with CRLF line ends when `crlf`, through `<path>.tmp`
    // renamed over it. On success the document lives at `path` and is unmodified; `status` is
    // `Saved <name>`, else `Cannot save <name>` and the document is unchanged.
    bool Save(const std::string& path, std::string& status);

    DocumentStats Stats() const { return ComputeStats(text); }
    static DocumentStats ComputeStats(const std::string& text);
};

} // namespace rgui
