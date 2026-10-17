// SPDX-License-Identifier: Apache-2.0
#pragma once

// Whole-file reads and writes for the files the app reads and writes on its own: documents,
// exports, settings, Omarchy theme files and PDF theme files. They may live in shared or
// group-writable folders, so:
// - a write goes to a temporary file with a random name next to the destination, created
//   exclusively (never through a planted symlink or an existing file), then renamed over the
//   destination; a failure leaves the destination as it was and no temporary file behind;
// - a read opens without blocking and takes regular files only (no FIFO, device or socket can
//   hang the UI), at most a given size.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace rgui {

enum class WriteMode {
    // A file the user edits (Markdown documents): the data reaches the disk before the rename
    // (fsync of the file, then of its directory, FlushFileBuffers on Windows); an existing
    // destination keeps its permission bits; a destination that is a symlink stays one, and the
    // file it points to is replaced.
    Document,
    // A file the app produces (exports, settings): no flush; an existing regular file's
    // permission bits are kept, a new file gets 0666 less the umask; a symlink at the destination
    // is replaced, never followed.
    Output,
    // A file that must not exist yet (an image copied beside a document): no flush, 0666 less the
    // umask, and the temporary file takes the name only while nothing is there (RENAME_NOREPLACE,
    // or a hard link where the file system has no such rename; MoveFileExW without
    // MOVEFILE_REPLACE_EXISTING), so an existing file or link is never replaced; that failure's
    // reason is kFileExistsError.
    NewFile,
};

// The reason WriteFileAtomically gives when a NewFile destination exists.
inline constexpr const char kFileExistsError[] = "the file exists";

// Replaces `path` with `bytes` as described above. A destination that exists and is not a
// regular file (a directory, FIFO, device or socket; for Output also no symlink to one) is
// refused. False on any failure, with a short reason in `*error` when `error` is set.
bool WriteFileAtomically(const std::filesystem::path& path, std::string_view bytes, WriteMode mode,
                         std::string* error);

// The whole regular file at `path` (symlinks followed), at most `limit` bytes. Never blocks on a
// FIFO or device: those are refused, as are larger files. False on any failure, with a short
// reason in `*error` when `error` is set.
bool ReadRegularFile(const std::filesystem::path& path, size_t limit, std::string& out, std::string* error);

} // namespace rgui
