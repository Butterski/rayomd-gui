// SPDX-License-Identifier: Apache-2.0
#include "app/notes.h"

#include "common/file_io.h"
#include "core/embeds.h"
#include "core/vault.h"

#include <algorithm>
#include <deque>
#include <string_view>
#include <system_error>
#include <utility>

namespace rgui {
namespace {

namespace fs = std::filesystem;
constexpr size_t kNone = std::string_view::npos;

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool LessIgnoringCase(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        const char x = Lower(a[i]), y = Lower(b[i]);
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsBlank(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsBlank(text.back())) text.remove_suffix(1);
    return text;
}

// Calls `each(note)` for the note each "![[inner]]" alone on a line from its first column names
// (the engine's StandaloneEmbed and ParseTarget: the name before `|` and `#`, not an image, PDF,
// audio or video), at most `limit` times. Reads only the lines that hold "![[".
template <typename Each>
void ForEachEmbeddedNote(std::string_view text, size_t limit, Each each) {
    size_t at = 0;
    size_t count = 0;
    while (count < limit && at < text.size()) {
        const size_t found = text.find("![[", at);
        if (found == kNone) return;
        size_t end = text.find('\n', found);
        if (end == kNone) end = text.size();
        at = end;
        if (found != 0 && text[found - 1] != '\n') continue;
        const std::string_view line = text.substr(found, end - found);
        const size_t close = line.find("]]", 3);
        if (close == kNone || close == 3 || !Trim(line.substr(close + 2)).empty()) continue;
        const std::string_view inner = line.substr(3, close - 3);
        if (inner.find('[') != kNone || inner.find("%%") != kNone) continue;
        std::string_view name = inner.substr(0, inner.find('|'));
        if (!name.empty() && name.back() == '\\') name.remove_suffix(1);
        name = Trim(name);
        const std::string_view note = Trim(name.substr(0, name.find('#')));
        if (note.empty() || !TinyPdf::Internal::IsNoteTarget(note)) continue;
        count++;
        each(note);
    }
}

} // namespace

std::vector<std::string> ListNoteNames(const fs::path& folder, size_t maxNames) {
    std::vector<std::string> names;
    if (folder.empty()) return names;
    struct Folder {
        fs::path path;
        int depth;
    };
    std::deque<Folder> folders;
    folders.push_back({folder, 0});
    size_t entries = 0;
    while (!folders.empty() && names.size() < maxNames && entries < kMaxNoteScanEntries) {
        Folder current = std::move(folders.front());
        folders.pop_front();
        std::error_code error;
        fs::directory_iterator it(current.path, fs::directory_options::skip_permission_denied, error);
        for (; !error && it != fs::directory_iterator(); it.increment(error)) {
            if (++entries > kMaxNoteScanEntries || names.size() >= maxNames) break;
            const fs::directory_entry& entry = *it;
            const fs::path name = entry.path().filename();
            const std::string utf8 = name.u8string();
            if (utf8.empty() || utf8[0] == '.') continue;
            std::error_code statusError;
            const fs::file_status status = entry.symlink_status(statusError);
            if (statusError) continue;
            if (fs::is_directory(status)) {
                if (current.depth < kMaxNoteDepth) folders.push_back({entry.path(), current.depth + 1});
                continue;
            }
            const std::string extension = name.extension().u8string();
            if (extension.size() != 3 || extension[0] != '.' || Lower(extension[1]) != 'm' || Lower(extension[2]) != 'd') {
                continue;
            }
            std::string stem = name.stem().u8string();
            if (stem.empty() || stem.find_first_of("[]|#^") != std::string::npos) continue;
            names.push_back(std::move(stem));
        }
    }
    std::sort(names.begin(), names.end(), LessIgnoringCase);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

EmbedWatch::Stamp EmbedWatch::StampOf(const fs::path& path) {
    Stamp stamp;
    stamp.path = path;
    std::error_code sizeError, timeError;
    stamp.size = fs::file_size(path, sizeError);
    stamp.modified = fs::last_write_time(path, timeError);
    stamp.exists = !sizeError && !timeError;
    if (!stamp.exists) {
        stamp.size = 0;
        stamp.modified = fs::file_time_type();
    }
    return stamp;
}

void EmbedWatch::Clear() {
    key_.clear();
    stamps_.clear();
}

bool EmbedWatch::Changed() const {
    for (const Stamp& stamp : stamps_) {
        if (!(StampOf(stamp.path) == stamp)) return true;
    }
    return false;
}

void EmbedWatch::Update(const std::string& text, const TinyPdf::PdfOptions& options) {
    if (options.dialect != TinyPdf::MarkdownDialect::Obsidian || !TinyPdf::Internal::MayEmbedNotes(text, options)) {
        Clear();
        return;
    }
    std::string key = options.vaultRoot;
    key += '\0';
    key += options.sourcePath;
    ForEachEmbeddedNote(text, kMaxWatched, [&key](std::string_view note) {
        key += '\0';
        key.append(note.data(), note.size());
    });
    if (key == key_ && !Changed()) return;
    key_ = std::move(key);
    Resolve(text, options);
}

void EmbedWatch::Resolve(const std::string& text, const TinyPdf::PdfOptions& options) {
    using TinyPdf::Internal::EmbedSet;
    stamps_.clear();
    TinyPdf::Internal::Vault vault(options.vaultRoot, options.sourcePath);
    if (!vault.Valid()) return;
    fs::path folder = RayoMd::File::PathFromUtf8(options.sourcePath).parent_path();
    if (folder.empty()) folder = ".";
    folder = TinyPdf::Internal::ResolvedPath(folder);
    if (folder.empty()) return;
    // Breadth first: the document's embeds, then theirs, as deep and as many bytes as the
    // engine reads (EmbedSet's limits).
    struct Note {
        fs::path folder;
        std::string text;
        int depth;
    };
    std::deque<Note> notes;
    size_t budget = EmbedSet::kMaxBytes;
    auto visit = [&](std::string_view from, const fs::path& in, int depth) {
        if (depth >= EmbedSet::kMaxDepth) return;
        ForEachEmbeddedNote(from, kMaxWatched, [&](std::string_view target) {
            if (stamps_.size() >= kMaxWatched) return;
            const fs::path path = vault.Find(in, target, true);
            if (path.empty()) return;
            for (const Stamp& known : stamps_) {
                if (known.path == path) return;
            }
            // Stamped before it is read: a change after this shows as one.
            stamps_.push_back(StampOf(path));
            Note note;
            RayoMd::File::FileIdentity identity;
            if (budget > 0 && vault.Read(path, budget, note.text, identity)) {
                budget -= std::min(budget, note.text.size());
                note.folder = path.parent_path();
                note.depth = depth + 1;
                notes.push_back(std::move(note));
            }
        });
    };
    visit(text, folder, 0);
    while (!notes.empty() && stamps_.size() < kMaxWatched) {
        Note note = std::move(notes.front());
        notes.pop_front();
        visit(note.text, note.folder, note.depth);
    }
}

} // namespace rgui
