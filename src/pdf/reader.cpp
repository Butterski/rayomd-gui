// SPDX-License-Identifier: Apache-2.0
#include "pdf/reader.h"

#include "pdf/syntax.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace rgui::pdf {
namespace {

using syntax::Cursor;

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;
// What a hash holds where a reference cannot be followed: a missing object, a cycle, a chain
// too deep.
constexpr uint64_t kMissingObject = 0x6d697373696e6721ull;
constexpr uint64_t kCutObject = 0x6379636c65642121ull;

// The page tree nests at most this deep.
constexpr int kMaxTreeDepth = 64;
// With values nested at most 64 deep within an object, hashing recurses at most 32 x 64 deep.
constexpr size_t kMaxHashStack = 32;
constexpr int kMaxReferenceChain = 32;
constexpr size_t kMaxXrefSections = 64;
constexpr size_t kMaxPages = 1u << 20;

void MixByte(uint64_t& hash, uint8_t byte) {
    hash ^= byte;
    hash *= kFnvPrime;
}

void MixBytes(uint64_t& hash, std::string_view bytes) {
    uint64_t h = hash;
    for (unsigned char byte : bytes) {
        h ^= byte;
        h *= kFnvPrime;
    }
    hash = h;
}

void MixU64(uint64_t& hash, uint64_t value) {
    for (int i = 0; i < 8; i++) MixByte(hash, static_cast<uint8_t>(value >> (8 * i)));
}

void MixDouble(uint64_t& hash, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    MixU64(hash, bits);
}

const PdfValue& Null() {
    static const PdfValue kNull;
    return kNull;
}

bool Finite(double value) {
    return value - value == 0;
}

void SetError(std::string* error, std::string_view text) {
    if (error) error->assign(text.data(), text.size());
}

// A non-negative decimal integer token.
bool ReadUnsigned(std::string_view token, uint64_t& out) {
    PdfValue value;
    if (token.empty() || token[0] == '+' || token[0] == '-') return false;
    if (!syntax::ParseNumber(token, value) || value.type != PdfValue::Type::Int) return false;
    out = static_cast<uint64_t>(value.integer);
    return true;
}

} // namespace

struct PdfFile::Slot {
    uint64_t offset = 0;
    uint32_t gen = 0;
    bool assigned = false;  // set by the newest cross-reference section naming the object
    bool used = false;      // an in-use entry ('n') within the file
    std::atomic<const PdfValue*> value{nullptr};
    std::atomic<uint64_t> hash{0};  // ObjectHash once known; 0 before
};

struct PdfFile::Page {
    const PdfValue* dict = nullptr;
    const PdfValue* resources = nullptr;
    PdfRect mediaBox;
    std::atomic<const std::string*> joined{nullptr};
};

struct PdfFile::HashContext {
    std::vector<uint32_t> stack;
    // Set while hashing an object when a reference below it was cut (a cycle or too deep): its
    // hash then depends on where the walk entered, so it is only kept for this walk.
    bool incomplete = false;
    std::unordered_map<uint32_t, uint64_t> local;
};

PdfFile::PdfFile() = default;

PdfFile::~PdfFile() {
    for (uint32_t i = 0; i < objectCount_; i++) {
        const PdfValue* value = slots_[i].value.load(std::memory_order_relaxed);
        if (value != &Null()) delete value;
    }
    for (int i = 0; i < pageCount_; i++) delete pages_[i].joined.load(std::memory_order_relaxed);
}

std::shared_ptr<const PdfFile> PdfFile::Parse(std::string bytes, std::string* error) {
    std::shared_ptr<PdfFile> file(new PdfFile());
    file->bytes_ = std::move(bytes);
    if (!file->ReadXref(error) || !file->ReadPages(error)) return nullptr;
    return file;
}

bool PdfFile::ReadXref(std::string* error) {
    const std::string_view data = bytes_;
    const size_t marker = data.rfind("startxref");
    if (marker == std::string_view::npos) {
        SetError(error, "startxref");
        return false;
    }
    Cursor start{data, marker + 9};
    syntax::SkipSpace(start);
    uint64_t offset = 0;
    if (!ReadUnsigned(syntax::ReadToken(start), offset) || offset >= data.size()) {
        SetError(error, "startxref");
        return false;
    }

    struct Entry {
        uint64_t num;
        uint64_t offset;
        uint32_t gen;
        bool used;
    };
    std::vector<Entry> entries;
    // Object numbers past this are ignored: an object takes far more than 8 bytes, and the slot
    // table must not grow beyond the file.
    const uint64_t limit = std::min<uint64_t>(1u << 24, data.size() / 8 + 1024);
    std::vector<uint64_t> visited;
    for (size_t section = 0; section < kMaxXrefSections; section++) {
        if (std::find(visited.begin(), visited.end(), offset) != visited.end()) break;
        visited.push_back(offset);
        const bool newest = section == 0;
        Cursor c{data, static_cast<size_t>(offset)};
        syntax::SkipSpace(c);
        if (syntax::ReadToken(c) != "xref") {
            if (!newest) break;
            SetError(error, "xref");
            return false;
        }
        bool ok = true;
        for (;;) {
            syntax::SkipSpace(c);
            const std::string_view token = syntax::ReadToken(c);
            if (token == "trailer") break;
            uint64_t first = 0;
            uint64_t count = 0;
            syntax::SkipSpace(c);
            if (!ReadUnsigned(token, first) || !ReadUnsigned(syntax::ReadToken(c), count)) {
                ok = false;
                break;
            }
            // Every entry takes at least three tokens of the file, so `count` cannot outrun it.
            for (uint64_t k = 0; k < count && ok; k++) {
                syntax::SkipSpace(c);
                const std::string_view entryOffset = syntax::ReadToken(c);
                syntax::SkipSpace(c);
                const std::string_view entryGen = syntax::ReadToken(c);
                syntax::SkipSpace(c);
                const std::string_view entryType = syntax::ReadToken(c);
                uint64_t objectOffset = 0;
                uint64_t gen = 0;
                if (!ReadUnsigned(entryOffset, objectOffset) || !ReadUnsigned(entryGen, gen) || gen > 65535 ||
                    (entryType != "n" && entryType != "f")) {
                    ok = false;
                    break;
                }
                const uint64_t num = first + k;
                if (num < limit && num >= first) {
                    entries.push_back({num, objectOffset, static_cast<uint32_t>(gen),
                                       entryType == "n" && objectOffset < data.size()});
                }
            }
            if (!ok) break;
        }
        PdfValue trailer;
        if (ok) {
            syntax::SkipSpace(c);
            ok = syntax::ParseValue(c, syntax::Mode::File, 0, trailer) && trailer.type == PdfValue::Type::Dict;
        }
        if (!ok) {
            if (newest) {
                SetError(error, "xref");
                return false;
            }
            break;
        }
        const PdfValue* prev = trailer.Find("Prev");
        const bool more = prev && prev->type == PdfValue::Type::Int && prev->integer >= 0 &&
                          static_cast<uint64_t>(prev->integer) < data.size();
        if (more) offset = static_cast<uint64_t>(prev->integer);
        if (newest) trailer_ = std::move(trailer);
        if (!more) break;
    }

    uint64_t highest = 0;
    for (const Entry& entry : entries) {
        if (entry.used) highest = std::max(highest, entry.num);
    }
    objectCount_ = static_cast<uint32_t>(highest + 1);
    slots_.reset(new Slot[objectCount_]);
    // Sections are read newest first: the first entry for an object wins.
    for (const Entry& entry : entries) {
        if (entry.num == 0 || entry.num >= objectCount_) continue;
        Slot& slot = slots_[entry.num];
        if (slot.assigned) continue;
        slot.assigned = true;
        slot.used = entry.used;
        slot.offset = entry.offset;
        slot.gen = entry.gen;
    }
    return true;
}

bool PdfFile::ReadPages(std::string* error) {
    const PdfValue& root = Get(trailer_, "Root");
    if (!root.IsDict()) {
        SetError(error, "root");
        return false;
    }
    const PdfValue* pagesEntry = root.Find("Pages");
    const PdfValue& tree = pagesEntry ? Resolve(*pagesEntry) : Null();
    if (!tree.IsDict()) {
        SetError(error, "pages");
        return false;
    }

    struct Found {
        const PdfValue* dict;
        const PdfValue* mediaBox;
        const PdfValue* resources;
    };
    struct Walker {
        const PdfFile& file;
        std::vector<uint8_t> visited;
        std::vector<Found> found;

        void Walk(const PdfValue& node, int depth, const PdfValue* mediaBox, const PdfValue* resources) {
            if (depth > kMaxTreeDepth || found.size() >= kMaxPages) return;
            if (const PdfValue* own = node.Find("MediaBox")) mediaBox = &file.Resolve(*own);
            if (const PdfValue* own = node.Find("Resources")) resources = &file.Resolve(*own);
            const PdfValue& type = file.Get(node, "Type");
            const PdfValue& kids = file.Get(node, "Kids");
            if (type.IsName("Page") || (!kids.IsArray() && !type.IsName("Pages"))) {
                found.push_back({&node, mediaBox, resources});
                return;
            }
            for (const PdfValue& kid : kids.Array()) {
                if (kid.IsRef()) {
                    if (!file.GenerationMatches(kid) || visited[kid.refNum]) continue;
                    visited[kid.refNum] = 1;
                }
                const PdfValue& child = file.Resolve(kid);
                if (child.IsDict()) Walk(child, depth + 1, mediaBox, resources);
            }
        }
    };
    Walker walker{*this, std::vector<uint8_t>(objectCount_, 0), {}};
    if (pagesEntry->IsRef() && GenerationMatches(*pagesEntry)) walker.visited[pagesEntry->refNum] = 1;
    walker.Walk(tree, 0, nullptr, nullptr);
    if (walker.found.empty()) {
        SetError(error, "no pages");
        return false;
    }

    pageCount_ = static_cast<int>(walker.found.size());
    pages_.reset(new Page[walker.found.size()]);
    for (size_t i = 0; i < walker.found.size(); i++) {
        const Found& found = walker.found[i];
        Page& page = pages_[i];
        page.dict = found.dict;
        page.resources = found.resources && found.resources->IsDict() ? found.resources : nullptr;
        PdfRect box;
        if (found.mediaBox && Rect(*found.mediaBox, box) && box.Width() > 0 && box.Height() > 0) {
            page.mediaBox = box;
        } else {
            page.mediaBox = {0, 0, 612, 792};
        }
    }
    return true;
}

bool PdfFile::GenerationMatches(const PdfValue& ref) const {
    return ref.refNum < objectCount_ && slots_[ref.refNum].used && slots_[ref.refNum].gen == ref.refGen;
}

bool PdfFile::StreamEnd(size_t start, size_t& end) const {
    std::call_once(endstreamsOnce_, [this] {
        for (size_t at = bytes_.find("endstream"); at != std::string::npos; at = bytes_.find("endstream", at + 9)) {
            endstreams_.push_back(at);
        }
    });
    const auto it = std::lower_bound(endstreams_.begin(), endstreams_.end(), start);
    if (it == endstreams_.end()) return false;
    end = *it;
    if (end > start && bytes_[end - 1] == '\n') end--;
    if (end > start && bytes_[end - 1] == '\r') end--;
    return true;
}

bool PdfFile::ParseObjectAt(uint32_t num, bool resolveLength, PdfValue& out) const {
    const Slot& slot = slots_[num];
    Cursor c{bytes_, static_cast<size_t>(slot.offset)};
    syntax::SkipSpace(c);
    uint64_t number = 0;
    uint64_t gen = 0;
    if (!ReadUnsigned(syntax::ReadToken(c), number) || number != num) return false;
    syntax::SkipSpace(c);
    if (!ReadUnsigned(syntax::ReadToken(c), gen) || gen != slot.gen) return false;
    syntax::SkipSpace(c);
    if (syntax::ReadToken(c) != "obj") return false;
    syntax::SkipSpace(c);
    if (!syntax::ParseValue(c, syntax::Mode::File, 0, out)) return false;
    if (out.type != PdfValue::Type::Dict) return true;
    syntax::SkipSpace(c);
    if (syntax::ReadToken(c) != "stream") return true;
    // The data begins after the end of line of "stream": CR LF or LF (a lone CR tolerated).
    if (c.Peek() == '\r') c.pos++;
    if (c.Peek() == '\n') c.pos++;
    const size_t start = c.pos;

    int64_t length = -1;
    if (const PdfValue* entry = out.Find("Length")) {
        if (entry->type == PdfValue::Type::Int) {
            length = entry->integer;
        } else if (entry->IsRef() && resolveLength && entry->refNum != 0 && GenerationMatches(*entry)) {
            // Parsed apart from the object cache, without following a /Length of its own, so
            // that two streams naming each other cannot recurse.
            PdfValue value;
            if (ParseObjectAt(entry->refNum, false, value) && value.type == PdfValue::Type::Int) {
                length = value.integer;
            }
        }
    }
    size_t end = 0;
    bool found = false;
    if (length >= 0 && static_cast<uint64_t>(length) <= bytes_.size() - start) {
        Cursor after{bytes_, start + static_cast<size_t>(length)};
        syntax::SkipWhite(after);
        found = syntax::ReadToken(after) == "endstream";
        end = start + static_cast<size_t>(length);
    }
    if (!found && !StreamEnd(start, end)) return false;
    out.type = PdfValue::Type::Stream;
    out.streamOffset = start;
    out.streamLength = end - start;
    return true;
}

const PdfValue& PdfFile::Object(uint32_t num) const {
    if (num == 0 || num >= objectCount_ || !slots_[num].used) return Null();
    Slot& slot = slots_[num];
    const PdfValue* known = slot.value.load(std::memory_order_acquire);
    if (known) return *known;
    // Threads that ask at once parse it each; the first to finish publishes its copy.
    PdfValue* parsed = new PdfValue();
    const PdfValue* candidate = parsed;
    if (!ParseObjectAt(num, true, *parsed)) {
        delete parsed;
        candidate = &Null();
    }
    const PdfValue* expected = nullptr;
    if (slot.value.compare_exchange_strong(expected, candidate, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return *candidate;
    }
    if (candidate != &Null()) delete candidate;
    return *expected;
}

const PdfValue& PdfFile::Resolve(const PdfValue& value) const {
    const PdfValue* current = &value;
    for (int hops = 0; current->IsRef(); hops++) {
        if (hops >= kMaxReferenceChain || !GenerationMatches(*current)) return Null();
        current = &Object(current->refNum);
    }
    return *current;
}

const PdfValue& PdfFile::Get(const PdfValue& dict, std::string_view key) const {
    const PdfValue* entry = Resolve(dict).Find(key);
    return entry ? Resolve(*entry) : Null();
}

bool PdfFile::Number(const PdfValue& value, double& out) const {
    const PdfValue& resolved = Resolve(value);
    if (!resolved.IsNumber()) return false;
    out = resolved.Number();
    return true;
}

bool PdfFile::Rect(const PdfValue& value, PdfRect& out) const {
    const PdfValue& array = Resolve(value);
    if (!array.IsArray() || array.items.size() != 4) return false;
    double v[4];
    for (int i = 0; i < 4; i++) {
        if (!Number(array.items[i], v[i]) || !Finite(v[i])) return false;
    }
    out = {std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3])};
    return true;
}

bool PdfFile::StreamData(const PdfValue& stream, std::string_view& data, std::string* error) const {
    const PdfValue& resolved = Resolve(stream);
    if (!resolved.IsStream()) {
        SetError(error, "stream");
        return false;
    }
    const PdfValue& filter = Get(resolved, "Filter");
    const PdfValue* first = filter.IsArray() ? (filter.items.empty() ? nullptr : &Resolve(filter.items[0]))
                                             : (filter.IsNull() ? nullptr : &filter);
    if (first) {
        if (error) *error = "filter:" + std::string(first->IsName() ? first->Name() : std::string_view("?"));
        return false;
    }
    data = std::string_view(bytes_).substr(resolved.streamOffset, resolved.streamLength);
    return true;
}

bool PdfFile::RawStream(const PdfValue& stream, PdfRawStream& out) const {
    const PdfValue& resolved = Resolve(stream);
    if (!resolved.IsStream()) return false;
    out.data = std::string_view(bytes_).substr(resolved.streamOffset, resolved.streamLength);
    const PdfValue& filter = Get(resolved, "Filter");
    const PdfValue& parms = Get(resolved, "DecodeParms");
    out.filter = filter.IsNull() ? nullptr : &filter;
    out.decodeParms = parms.IsNull() ? nullptr : &parms;
    return true;
}

void PdfFile::HashInto(const PdfValue& value, uint64_t& hash, HashContext& context) const {
    MixByte(hash, static_cast<uint8_t>(value.type));
    switch (value.type) {
        case PdfValue::Type::Null: break;
        case PdfValue::Type::Bool: MixByte(hash, value.boolean ? 1 : 0); break;
        case PdfValue::Type::Int: MixU64(hash, static_cast<uint64_t>(value.integer)); break;
        case PdfValue::Type::Real: MixDouble(hash, value.real); break;
        case PdfValue::Type::String:
        case PdfValue::Type::Name:
            MixU64(hash, value.text.size());
            MixBytes(hash, value.text);
            break;
        case PdfValue::Type::Array:
            MixU64(hash, value.items.size());
            for (const PdfValue& item : value.items) HashInto(item, hash, context);
            break;
        case PdfValue::Type::Dict:
        case PdfValue::Type::Stream:
            MixU64(hash, value.entries.size());
            for (const PdfDictEntry& entry : value.entries) {
                MixU64(hash, entry.key.size());
                MixBytes(hash, entry.key);
                // /Parent leads up a tree (pages, outlines) to everything else in it.
                if (entry.key == "Parent") continue;
                HashInto(entry.value, hash, context);
            }
            if (value.type == PdfValue::Type::Stream) {
                MixU64(hash, value.streamLength);
                MixBytes(hash, std::string_view(bytes_).substr(value.streamOffset, value.streamLength));
            }
            break;
        case PdfValue::Type::Ref:
            MixU64(hash, GenerationMatches(value) ? ObjectHash(value.refNum, context) : kMissingObject);
            break;
    }
}

uint64_t PdfFile::ObjectHash(uint32_t num, HashContext& context) const {
    Slot& slot = slots_[num];
    const uint64_t known = slot.hash.load(std::memory_order_relaxed);
    if (known != 0) return known;
    const auto local = context.local.find(num);
    if (local != context.local.end()) return local->second;
    if (context.stack.size() >= kMaxHashStack ||
        std::find(context.stack.begin(), context.stack.end(), num) != context.stack.end()) {
        context.incomplete = true;
        return kCutObject;
    }
    const bool outerIncomplete = context.incomplete;
    context.incomplete = false;
    context.stack.push_back(num);
    uint64_t hash = kFnvOffset;
    HashInto(Object(num), hash, context);
    context.stack.pop_back();
    if (hash == 0) hash = 1;
    if (context.incomplete) {
        context.local.emplace(num, hash);
    } else {
        slot.hash.store(hash, std::memory_order_relaxed);
    }
    context.incomplete = context.incomplete || outerIncomplete;
    return hash;
}

uint64_t PdfFile::ValueHash(const PdfValue& value) const {
    HashContext context;
    uint64_t hash = kFnvOffset;
    HashInto(value, hash, context);
    return hash;
}

const PdfValue& PdfFile::PageDict(int page) const {
    return page >= 0 && page < pageCount_ ? *pages_[page].dict : Null();
}

PdfRect PdfFile::PageMediaBox(int page) const {
    return page >= 0 && page < pageCount_ ? pages_[page].mediaBox : PdfRect{0, 0, 612, 792};
}

const PdfValue& PdfFile::PageResources(int page) const {
    return page >= 0 && page < pageCount_ && pages_[page].resources ? *pages_[page].resources : Null();
}

bool PdfFile::PageContent(int page, std::string_view& content, std::string* error) const {
    if (page < 0 || page >= pageCount_) {
        SetError(error, "page");
        return false;
    }
    Page& entry = pages_[page];
    const PdfValue& contents = Get(*entry.dict, "Contents");
    if (contents.IsNull()) {
        content = {};
        return true;
    }
    if (contents.IsStream()) return StreamData(contents, content, error);
    if (!contents.IsArray()) {
        SetError(error, "contents");
        return false;
    }
    if (const std::string* known = entry.joined.load(std::memory_order_acquire)) {
        content = *known;
        return true;
    }
    std::string joined;
    for (size_t i = 0; i < contents.items.size(); i++) {
        std::string_view part;
        if (!StreamData(contents.items[i], part, error)) return false;
        if (i > 0) joined.push_back('\n');
        joined.append(part.data(), part.size());
    }
    const std::string* candidate = new std::string(std::move(joined));
    const std::string* expected = nullptr;
    if (!entry.joined.compare_exchange_strong(expected, candidate, std::memory_order_acq_rel, std::memory_order_acquire)) {
        delete candidate;
        candidate = expected;
    }
    content = *candidate;
    return true;
}

uint64_t PdfFile::PageKey(int page) const {
    if (page < 0 || page >= pageCount_) return 0;
    const Page& entry = pages_[page];
    uint64_t hash = kFnvOffset;
    MixDouble(hash, entry.mediaBox.x0);
    MixDouble(hash, entry.mediaBox.y0);
    MixDouble(hash, entry.mediaBox.x1);
    MixDouble(hash, entry.mediaBox.y1);
    auto mixStream = [&](const PdfValue& stream) {
        if (!stream.IsStream()) {
            MixByte(hash, 0);
            return;
        }
        MixByte(hash, 1);
        MixU64(hash, stream.streamLength);
        MixBytes(hash, std::string_view(bytes_).substr(stream.streamOffset, stream.streamLength));
    };
    const PdfValue& contents = Get(*entry.dict, "Contents");
    if (contents.IsArray()) {
        MixU64(hash, contents.items.size());
        for (const PdfValue& item : contents.items) mixStream(Resolve(item));
    } else {
        mixStream(contents);
    }
    HashContext context;
    if (entry.resources) {
        HashInto(*entry.resources, hash, context);
    } else {
        MixByte(hash, 0);
    }
    return hash;
}

} // namespace rgui::pdf
