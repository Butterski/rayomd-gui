// SPDX-License-Identifier: Apache-2.0
// The PDF object reader of the preview: the classic cross-reference table (with /Prev chains),
// objects parsed on first use, streams read in place, the page tree, and a key per page that
// changes exactly when what the page shows may change.
//
// One PdfFile is shared, as std::shared_ptr<const PdfFile>, by the preview threads rendering
// its pages at once: every const member function is safe to call from several threads.
// References returned (values, string views) live as long as the PdfFile.
#pragma once

#include "pdf/value.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::pdf {

// A PDF rectangle in user space, normalized so that x0 <= x1 and y0 <= y1.
struct PdfRect {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;

    double Width() const { return x1 - x0; }
    double Height() const { return y1 - y0; }
};

// A stream's data as stored, for the image path, which decodes filters itself.
struct PdfRawStream {
    std::string_view data;
    const PdfValue* filter = nullptr;       // /Filter resolved; nullptr when absent
    const PdfValue* decodeParms = nullptr;  // /DecodeParms resolved; nullptr when absent
};

// A content hash of 128 bits: keyed SipHash-1-3-128, the key drawn once per process. Caches
// that hold what a hash names (page images, decoded images, fonts) take equal hashes for equal
// content: 128 keyed bits make a collision, which would show the wrong image or font, out of
// reach of a crafted document. Not stable across processes.
struct PdfHash {
    uint64_t lo = 0;
    uint64_t hi = 0;

    bool operator==(const PdfHash& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const PdfHash& o) const { return !(*this == o); }
};

struct PdfHashHasher {
    size_t operator()(const PdfHash& h) const { return static_cast<size_t>(h.lo ^ (h.hi >> 7)); }
};

class PdfFile {
public:
    // Reads the cross-reference table from `startxref` and the page tree. Nullptr, with
    // `error` (when not nullptr) set to what failed ("startxref", "xref", "trailer", "root",
    // "pages", "no pages"), when the file has no readable page.
    static std::shared_ptr<const PdfFile> Parse(std::string bytes, std::string* error);

    ~PdfFile();
    PdfFile(const PdfFile&) = delete;
    PdfFile& operator=(const PdfFile&) = delete;

    std::string_view Bytes() const { return bytes_; }
    // The newest trailer dictionary.
    const PdfValue& Trailer() const { return trailer_; }
    // One more than the highest object number the cross-reference table names.
    uint32_t ObjectCount() const { return objectCount_; }

    // Object `num`, parsed on first use; null when it is free, missing or unreadable.
    const PdfValue& Object(uint32_t num) const;
    // `value`, or what its reference (and a reference that object holds, and so on) points to;
    // null for a reference to a missing object, a wrong generation, or a chain of more than 32.
    const PdfValue& Resolve(const PdfValue& value) const;
    // The entry `key` of the dictionary or stream `dict` (itself resolved), resolved; null when
    // absent.
    const PdfValue& Get(const PdfValue& dict, std::string_view key) const;
    // A number, resolved. False for anything else.
    bool Number(const PdfValue& value, double& out) const;
    // An array of four numbers (each resolved), normalized. False for anything else or a
    // non-finite number.
    bool Rect(const PdfValue& value, PdfRect& out) const;

    // A stream's data (`stream` resolved) when it has no filter, in place. False when it is no
    // stream (error "stream") or has a /Filter (error "filter:<first filter's name>").
    bool StreamData(const PdfValue& stream, std::string_view& data, std::string* error) const;
    // A stream's data as stored with its /Filter and /DecodeParms. False when it is no stream.
    bool RawStream(const PdfValue& stream, PdfRawStream& out) const;

    // The hash of a value: its type, contents and stream data, with every reference replaced by
    // the hash of the object it names (each object hashed once per file) and the /Parent entry
    // of dictionaries left out. Object numbers never enter it.
    PdfHash ValueHash(const PdfValue& value) const;

    int PageCount() const { return pageCount_; }
    // The page's dictionary.
    const PdfValue& PageDict(int page) const;
    // The page's /MediaBox, inherited; US Letter when it has none or an empty one.
    PdfRect PageMediaBox(int page) const;
    // The page's /Resources dictionary, inherited and resolved; null when it has none.
    const PdfValue& PageResources(int page) const;
    // The page's content: its /Contents stream in place, or the streams of a /Contents array
    // joined with a newline (kept after the first call). False, with `error` as StreamData sets
    // it (or "contents" for a /Contents that is neither), when a stream cannot be read.
    bool PageContent(int page, std::string_view& content, std::string* error) const;
    // The hash of the page's MediaBox, its content streams as stored and ValueHash of its
    // resources: equal for pages that look the same in two builds, whatever their object numbers.
    PdfHash PageKey(int page) const;

private:
    struct Slot;
    struct Page;
    struct HashContext;
    struct Parsing;
    class Hasher;

    PdfFile();
    bool ReadXref(std::string* error);
    bool ReadPages(std::string* error);
    // Needs parsing_->mutex held; values go to its arena.
    bool ParseObjectAt(uint32_t num, bool resolveLength, PdfValue& out) const;
    bool StreamEnd(size_t start, size_t& end) const;
    bool GenerationMatches(const PdfValue& ref) const;
    PdfHash ObjectHash(uint32_t num, HashContext& context) const;
    void HashInto(const PdfValue& value, Hasher& hash, HashContext& context) const;

    std::string bytes_;
    PdfValue trailer_;
    uint32_t objectCount_ = 0;
    std::unique_ptr<Slot[]> slots_;
    int pageCount_ = 0;
    std::unique_ptr<Page[]> pages_;
    // Where parsed values live (as long as the file), and the lock of whoever parses an object.
    std::unique_ptr<Parsing> parsing_;
    // Where "endstream" occurs, found once on first need (a stream whose /Length is wrong).
    mutable std::once_flag endstreamsOnce_;
    mutable std::vector<size_t> endstreams_;
};

} // namespace rgui::pdf
