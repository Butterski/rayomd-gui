// SPDX-License-Identifier: Apache-2.0
// The document loader: one thread opens files for the UI thread (Document::Open: reading up to
// 128 MiB of text and its CRLF pass, or a reversible PDF's recovery, up to 256 MiB through
// PdfSource::Inspect) and counts the words and lines, so the window keeps drawing while a file
// opens. It also copies the images dropped on a saved document into the `images` folder beside it
// (CopyImageBeside). Results go back through Platform::PostToUi.
#pragma once

#include "app/document.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rgui {

class Platform;

struct DocumentLoaded {
    uint64_t id = 0;       // the Load call's
    std::string path;      // as requested (UTF-8)
    bool ok = false;
    // On success the opened document (Document::Open's result); else unused.
    Document document;
    // Document::Open's status: what to tell the user (empty after a plain open).
    std::string status;
    DocumentStats stats;
};

struct ImageCopied {
    uint64_t id = 0;            // the CopyImage call's
    std::string documentPath;   // as requested (UTF-8)
    bool ok = false;
    // On success the image's Markdown, `![name](images/name.png)`.
    std::string markdown;
    // `Copied <file> into images/` (`... as <name>` when renamed), or why it was not.
    std::string status;
};

// The largest image CopyImageBeside copies.
constexpr size_t kMaxDroppedImageBytes = size_t(64) << 20;

// Copies the image at `source` into the `images` folder beside `document` (made when missing):
// read with ReadRegularFile (at most kMaxDroppedImageBytes), written with WriteMode::NewFile under
// its name made safe for a Markdown link (ASCII letters, digits, `.`, `-`, `_` and well-formed
// UTF-8 kept, any other byte `-`, the extension in lower case), as `name-2.png`, `name-3.png` and on
// when the name is taken; an existing file is never replaced. An image that is already in that
// folder under that name is linked as it is. Fills `copied` (not its id or documentPath).
void CopyImageBeside(const std::filesystem::path& source, const std::filesystem::path& document, ImageCopied& copied);

class DocumentLoader {
public:
    using Callback = std::function<void(DocumentLoaded)>;
    using ImageCallback = std::function<void(ImageCopied)>;
    // Copies waiting at most; more are refused.
    static constexpr size_t kMaxPendingCopies = 32;

    // Starts the thread. `onLoaded` and `onImage` run on the UI thread, never after the loader is
    // destroyed. `platform` must outlive the loader.
    DocumentLoader(Platform& platform, Callback onLoaded, ImageCallback onImage = nullptr);
    // Drops a pending request and joins the thread (after the file it reads, if any).
    ~DocumentLoader();
    DocumentLoader(const DocumentLoader&) = delete;
    DocumentLoader& operator=(const DocumentLoader&) = delete;

    // Opens `path` (UTF-8) on the thread; replaces a request not yet started. Returns the id
    // its DocumentLoaded carries.
    uint64_t Load(std::string path);
    // Copies `source` beside `documentPath` (both UTF-8) on the thread, after the copies asked for
    // before it; the open waits for them. Returns the id its ImageCopied carries, or 0 when
    // kMaxPendingCopies are waiting already.
    uint64_t CopyImage(std::string source, std::string documentPath);

private:
    struct Request {
        uint64_t id = 0;
        std::string path;
        std::string documentPath;   // a copy's
    };
    // Shared with the tasks posted to the UI thread, which may run after the loader is gone.
    struct Callbacks {
        bool alive = true;  // UI thread only
        Callback onLoaded;
        ImageCallback onImage;
    };

    void Run();

    Platform& platform_;
    std::shared_ptr<Callbacks> callbacks_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;  // guarded by mutex_
    std::deque<Request> copies_;      // guarded by mutex_
    bool stop_ = false;               // guarded by mutex_
    uint64_t nextId_ = 0;             // UI thread only
    std::thread thread_;
};

} // namespace rgui
