// SPDX-License-Identifier: Apache-2.0
// The notes around a document, for Obsidian's dialect: the names `[[` and `![[` complete
// (ListNoteNames, run on the document loader's thread), and the notes a preview embeds, whose
// files the build worker watches so an edit made elsewhere shows in the preview (EmbedWatch).
// Both work off the UI thread and are bounded.
#pragma once

#include "common/file_io.h"
#include "rayomd/tiny_pdf.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rgui {

// ListNoteNames' bounds: names kept, folder levels below the one listed, and directory entries
// looked at in all.
constexpr size_t kMaxNoteNames = 5000;
constexpr int kMaxNoteDepth = 8;
constexpr size_t kMaxNoteScanEntries = 50000;

// The names of the notes under `folder`: the stems (UTF-8) of its `.md` files (any case),
// breadth first, at most kMaxNoteDepth folders down and `maxNames` names (kMaxNoteNames; lower in
// tests, as thousands of files are slow to make on Windows) from at most kMaxNoteScanEntries
// entries. Hidden files and folders (a name beginning with `.`: `.obsidian`, `.trash`, `.git`)
// are passed over and links to folders not followed. A name a wiki link cannot hold (`[`, `]`,
// `|`, `#`, `^`) is left out. Sorted ASCII case-insensitively, each once.
std::vector<std::string> ListNoteNames(const std::filesystem::path& folder, size_t maxNames = kMaxNoteNames);

// The notes a document embeds (Obsidian's `![[note]]` alone on a line), as the engine resolves
// them (TinyPdf::Internal::Vault), nested embeds too, and the stamp of each file (one stat:
// RayoMd::File::StampOf). Lines inside fences count too: a note watched for nothing costs a stat.
// Worker thread only.
class EmbedWatch {
public:
    // Notes watched at most; past them the rest go unwatched.
    static constexpr size_t kMaxWatched = 128;

    // Before a preview build of `text` with `options`: what it embeds. The notes are resolved
    // again (and read for their own embeds, within the engine's EmbedSet limits) only when the
    // embed lines, the vault or the document's path changed, or a watched file did; else this
    // costs a stat a note. Nothing for a document that embeds no note (MayEmbedNotes).
    void Update(const std::string& text, const TinyPdf::PdfOptions& options);
    // Whether a watched note's file changed (RayoMd::File::Stamp: size, times, identity; or came
    // or went) since Update.
    bool Changed() const;
    bool Empty() const { return watched_.empty(); }
    size_t Count() const { return watched_.size(); }
    void Clear();

private:
    struct Watched {
        std::filesystem::path path;
        RayoMd::File::Stamp stamp;
    };
    void Resolve(const std::string& text, const TinyPdf::PdfOptions& options);

    std::string key_;   // the vault, the document's path and its embed targets
    std::vector<Watched> watched_;
};

} // namespace rgui
