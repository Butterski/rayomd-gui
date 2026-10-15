// SPDX-License-Identifier: Apache-2.0
// The document loader: one thread opens files for the UI thread (Document::Open: reading up to
// 128 MiB of text and its CRLF pass, or a reversible PDF's recovery, up to 256 MiB through
// PdfSource::Inspect) and counts the words and lines, so the window keeps drawing while a file
// opens. Results go back through Platform::PostToUi.
#pragma once

#include "app/document.h"

#include <condition_variable>
#include <cstdint>
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

class DocumentLoader {
public:
    using Callback = std::function<void(DocumentLoaded)>;

    // Starts the thread. `onLoaded` runs on the UI thread, never after the loader is destroyed.
    // `platform` must outlive the loader.
    DocumentLoader(Platform& platform, Callback onLoaded);
    // Drops a pending request and joins the thread (after the file it reads, if any).
    ~DocumentLoader();
    DocumentLoader(const DocumentLoader&) = delete;
    DocumentLoader& operator=(const DocumentLoader&) = delete;

    // Opens `path` (UTF-8) on the thread; replaces a request not yet started. Returns the id
    // its DocumentLoaded carries.
    uint64_t Load(std::string path);

private:
    struct Request {
        uint64_t id = 0;
        std::string path;
    };
    // Shared with the tasks posted to the UI thread, which may run after the loader is gone.
    struct Callbacks {
        bool alive = true;  // UI thread only
        Callback onLoaded;
    };

    void Run();

    Platform& platform_;
    std::shared_ptr<Callbacks> callbacks_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;  // guarded by mutex_
    bool stop_ = false;               // guarded by mutex_
    uint64_t nextId_ = 0;             // UI thread only
    std::thread thread_;
};

} // namespace rgui
