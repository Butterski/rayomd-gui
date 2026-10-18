// SPDX-License-Identifier: Apache-2.0
#include "app/documentloader.h"

#include "app/notes.h"
#include "app/platform.h"
#include "app/safefile.h"
#include "common/text_utils.h"

#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

namespace rgui {

namespace {

namespace fs = std::filesystem;

// Names tried for a copy: `name.png`, then `name-2.png` to `name-999.png`.
constexpr int kMaxNameTries = 999;
// The safe name's stem keeps at most this many bytes (cut at a code point).
constexpr size_t kMaxStemBytes = 100;

bool SafeAscii(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == '_';
}

// `name` with what a Markdown link takes as it is: ASCII letters, digits, `.`, `-`, `_` and
// well-formed UTF-8 sequences; any other byte becomes `-` (a Latin-1 `\xE9` too, which the
// editor would show as U+FFFD, so the link would not name the file).
std::string SafeName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size();) {
        const unsigned char u = static_cast<unsigned char>(name[i]);
        uint32_t cp = 0;
        size_t length = 1;
        if (u >= 0x80 && RayoMd::Text::DecodeUtf8(name, i, cp, length)) {
            out.append(name.data() + i, length);
            i += length;
            continue;
        }
        out += SafeAscii(name[i]) ? name[i] : '-';
        i++;
    }
    return out;
}

std::string SafeStem(std::string_view name) {
    std::string stem = SafeName(name);
    if (stem.size() > kMaxStemBytes) {
        size_t keep = kMaxStemBytes;
        while (keep > 0 && (static_cast<unsigned char>(stem[keep]) & 0xC0) == 0x80) keep--;
        stem.resize(keep);
    }
    // No hidden file.
    if (!stem.empty() && stem[0] == '.') stem[0] = '-';
    return stem.empty() ? std::string("image") : stem;
}

} // namespace

void CopyImageBeside(const fs::path& source, const fs::path& document, ImageCopied& copied) {
    copied.ok = false;
    const std::string shown = source.filename().u8string();
    std::string bytes;
    std::string error;
    if (!ReadRegularFile(source, kMaxDroppedImageBytes, bytes, &error)) {
        copied.status = "Cannot copy " + shown + ": " + error;
        return;
    }
    std::string extension = SafeName(source.extension().u8string());
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const std::string stem = SafeStem(source.stem().u8string());
    const fs::path folder = document.parent_path() / "images";
    std::error_code ec;
    fs::create_directory(folder, ec);
    if (!fs::is_directory(folder, ec)) {
        copied.status = "Cannot copy " + shown + ": no images folder beside the document";
        return;
    }
    auto done = [&](const std::string& name, bool copiedBytes) {
        copied.ok = true;
        copied.markdown = "![" + stem + "](images/" + name + ")";
        if (!copiedBytes) copied.status = "Linked images/" + name;
        else if (name == shown) copied.status = "Copied " + shown + " into images/";
        else copied.status = "Copied " + shown + " into images/ as " + name;
    };
    // Dropped from that folder under a name that needs no change: linked as it is.
    if (stem + extension == shown && fs::equivalent(source, folder / fs::u8path(shown), ec)) {
        done(shown, false);
        return;
    }
    for (int n = 1; n <= kMaxNameTries; n++) {
        const std::string name = n == 1 ? stem + extension : stem + "-" + std::to_string(n) + extension;
        const fs::path target = folder / fs::u8path(name);
        if (fs::symlink_status(target, ec).type() != fs::file_type::not_found) continue;
        if (WriteFileAtomically(target, bytes, WriteMode::NewFile, &error)) {
            done(name, true);
            return;
        }
        // Taken meanwhile: the next name.
        if (error != kFileExistsError) {
            copied.status = "Cannot copy " + shown + ": " + error;
            return;
        }
    }
    copied.status = "Cannot copy " + shown + ": no free name in images/";
}

DocumentLoader::DocumentLoader(Platform& platform, Callback onLoaded, ImageCallback onImage, NotesCallback onNotes)
    : platform_(platform), callbacks_(std::make_shared<Callbacks>()) {
    callbacks_->onLoaded = std::move(onLoaded);
    callbacks_->onImage = std::move(onImage);
    callbacks_->onNotes = std::move(onNotes);
    thread_ = std::thread([this] { Run(); });
}

DocumentLoader::~DocumentLoader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        pending_.reset();
        copies_.clear();
        notes_.reset();
    }
    wake_.notify_all();
    thread_.join();
    callbacks_->alive = false;
}

uint64_t DocumentLoader::Load(std::string path) {
    Request request;
    request.id = ++nextId_;
    request.path = std::move(path);
    const uint64_t id = request.id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(request);
    }
    wake_.notify_one();
    return id;
}

uint64_t DocumentLoader::CopyImage(std::string source, std::string documentPath) {
    Request request;
    request.path = std::move(source);
    request.documentPath = std::move(documentPath);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (copies_.size() >= kMaxPendingCopies) return 0;
        request.id = ++nextId_;
        copies_.push_back(std::move(request));
    }
    wake_.notify_one();
    return nextId_;
}

void DocumentLoader::ListNotes(std::string folder) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notes_ = std::move(folder);
    }
    wake_.notify_one();
}

void DocumentLoader::Run() {
    for (;;) {
        Request request;
        bool copy = false;
        std::optional<std::string> notes;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || pending_.has_value() || !copies_.empty() || notes_.has_value(); });
            if (stop_) return;
            // Copies first: they are small, and their links go into the document they were
            // dropped on. Note names last: an open is waited for.
            if (!copies_.empty()) {
                request = std::move(copies_.front());
                copies_.pop_front();
                copy = true;
            } else if (pending_) {
                request = std::move(*pending_);
                pending_.reset();
            } else {
                notes = std::move(notes_);
                notes_.reset();
            }
        }
        if (notes) {
            NotesListed listed;
            listed.names = ListNoteNames(fs::u8path(*notes));
            listed.folder = std::move(*notes);
            platform_.PostToUi([callbacks = callbacks_, listed = std::move(listed)]() mutable {
                if (callbacks->alive && callbacks->onNotes) callbacks->onNotes(std::move(listed));
            });
            continue;
        }
        if (copy) {
            ImageCopied copied;
            copied.id = request.id;
            CopyImageBeside(fs::u8path(request.path), fs::u8path(request.documentPath), copied);
            copied.documentPath = std::move(request.documentPath);
            platform_.PostToUi([callbacks = callbacks_, copied = std::move(copied)]() mutable {
                if (callbacks->alive && callbacks->onImage) callbacks->onImage(std::move(copied));
            });
            continue;
        }
        DocumentLoaded loaded;
        loaded.id = request.id;
        loaded.ok = loaded.document.Open(request.path, loaded.status);
        if (loaded.ok) loaded.stats = loaded.document.Stats();
        loaded.path = std::move(request.path);
        platform_.PostToUi([callbacks = callbacks_, loaded = std::move(loaded)]() mutable {
            if (callbacks->alive && callbacks->onLoaded) callbacks->onLoaded(std::move(loaded));
        });
    }
}

} // namespace rgui
