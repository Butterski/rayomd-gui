// SPDX-License-Identifier: Apache-2.0
#include "pdf/reader.h"

#include "pdf/syntax.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace rgui::pdf {
namespace {

using syntax::Cursor;

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

inline uint64_t Rotl(uint64_t v, int n) {
    return (v << n) | (v >> (64 - n));
}

uint64_t SplitMix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The key of every hash in this process, drawn once: the kernel's random bytes on Linux, mixed
// with the clocks and where the process was loaded (ASLR) everywhere. Documents are made before
// the process exists and never see a hash, so they cannot be made to collide.
struct HashKey {
    uint64_t k0;
    uint64_t k1;
};

const HashKey& ProcessKey() {
    static const HashKey key = [] {
        uint64_t seed[2] = {0, 0};
#if defined(__linux__)
        if (getrandom(seed, sizeof seed, 0) != static_cast<ssize_t>(sizeof seed)) seed[0] = seed[1] = 0;
#endif
        static int anchor = 0;
        uint64_t state = seed[0] ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&anchor));
        state ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull;
        state ^= Rotl(static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()), 29);
        state ^= Rotl(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&state)), 47);
        HashKey k;
        k.k0 = SplitMix(state);
        k.k1 = SplitMix(state) ^ seed[1];
        return k;
    }();
    return key;
}

} // namespace

// SipHash-1-3 with 128-bit output (Aumasson and Bernstein), keyed with ProcessKey(): fed bytes
// and words as a stream, finished into a PdfHash.
class PdfFile::Hasher {
public:
    Hasher() {
        const HashKey& k = ProcessKey();
        v0_ = k.k0 ^ 0x736f6d6570736575ull;
        v1_ = k.k1 ^ 0x646f72616e646f6dull ^ 0xee;
        v2_ = k.k0 ^ 0x6c7967656e657261ull;
        v3_ = k.k1 ^ 0x7465646279746573ull;
    }

    void Byte(uint8_t b) {
        tail_ |= static_cast<uint64_t>(b) << (8 * tailBytes_);
        length_++;
        if (++tailBytes_ == 8) {
            Word(tail_);
            tail_ = 0;
            tailBytes_ = 0;
        }
    }

    void Bytes(std::string_view bytes) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
        size_t n = bytes.size();
        while (n > 0 && tailBytes_ != 0) {
            Byte(*p++);
            n--;
        }
        length_ += n & ~size_t(7);
        for (; n >= 8; n -= 8, p += 8) {
            uint64_t w = 0;
            for (int i = 0; i < 8; i++) w |= static_cast<uint64_t>(p[i]) << (8 * i);
            Word(w);
        }
        while (n-- > 0) Byte(*p++);
    }

    void U64(uint64_t value) {
        if (tailBytes_ == 0) {
            Word(value);
            length_ += 8;
            return;
        }
        for (int i = 0; i < 8; i++) Byte(static_cast<uint8_t>(value >> (8 * i)));
    }

    void Double(double value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        U64(bits);
    }

    void Hash(const PdfHash& h) {
        U64(h.lo);
        U64(h.hi);
    }

    PdfHash Finish() const {
        uint64_t v0 = v0_, v1 = v1_, v2 = v2_, v3 = v3_;
        const uint64_t b = (static_cast<uint64_t>(length_) << 56) | tail_;
        v3 ^= b;
        Round(v0, v1, v2, v3);
        v0 ^= b;
        v2 ^= 0xee;
        for (int i = 0; i < 3; i++) Round(v0, v1, v2, v3);
        PdfHash out;
        out.lo = v0 ^ v1 ^ v2 ^ v3;
        v1 ^= 0xdd;
        for (int i = 0; i < 3; i++) Round(v0, v1, v2, v3);
        out.hi = v0 ^ v1 ^ v2 ^ v3;
        return out;
    }

private:
    static void Round(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
        v0 += v1;
        v1 = Rotl(v1, 13);
        v1 ^= v0;
        v0 = Rotl(v0, 32);
        v2 += v3;
        v3 = Rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = Rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = Rotl(v1, 17);
        v1 ^= v2;
        v2 = Rotl(v2, 32);
    }

    void Word(uint64_t m) {
        v3_ ^= m;
        Round(v0_, v1_, v2_, v3_);
        v0_ ^= m;
    }

    uint64_t v0_, v1_, v2_, v3_;
    uint64_t tail_ = 0;
    int tailBytes_ = 0;
    uint64_t length_ = 0;
};

namespace {

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
    // ObjectHash once known (`hashed` set after both halves).
    std::atomic<uint64_t> hashLo{0};
    std::atomic<uint64_t> hashHi{0};
    std::atomic<bool> hashed{false};
};

struct PdfFile::Page {
    const PdfValue* dict = nullptr;
    const PdfValue* resources = nullptr;
    PdfRect mediaBox;
    std::atomic<const std::string*> joined{nullptr};
};

struct PdfFile::HashContext {
    std::vector<uint32_t> stack;
    // Set while hashing an object when a reference below it was cut (a cycle or too deep), or
    // when it met an object hashed so earlier in this walk: its hash then depends on where the
    // walk entered, so it is only kept for this walk.
    bool incomplete = false;
    std::unordered_map<uint32_t, PdfHash> local;
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

void PdfFile::HashInto(const PdfValue& value, Hasher& hash, HashContext& context) const {
    hash.Byte(static_cast<uint8_t>(value.type));
    switch (value.type) {
        case PdfValue::Type::Null: break;
        case PdfValue::Type::Bool: hash.Byte(value.boolean ? 1 : 0); break;
        case PdfValue::Type::Int: hash.U64(static_cast<uint64_t>(value.integer)); break;
        case PdfValue::Type::Real: hash.Double(value.real); break;
        case PdfValue::Type::String:
        case PdfValue::Type::Name:
            hash.U64(value.text.size());
            hash.Bytes(value.text);
            break;
        case PdfValue::Type::Array:
            hash.U64(value.items.size());
            for (const PdfValue& item : value.items) HashInto(item, hash, context);
            break;
        case PdfValue::Type::Dict:
        case PdfValue::Type::Stream:
            hash.U64(value.entries.size());
            for (const PdfDictEntry& entry : value.entries) {
                hash.U64(entry.key.size());
                hash.Bytes(entry.key);
                // /Parent leads up a tree (pages, outlines) to everything else in it.
                if (entry.key == "Parent") continue;
                HashInto(entry.value, hash, context);
            }
            if (value.type == PdfValue::Type::Stream) {
                hash.U64(value.streamLength);
                hash.Bytes(std::string_view(bytes_).substr(value.streamOffset, value.streamLength));
            }
            break;
        case PdfValue::Type::Ref:
            hash.Hash(GenerationMatches(value) ? ObjectHash(value.refNum, context)
                                               : PdfHash{kMissingObject, kMissingObject});
            break;
    }
}

PdfHash PdfFile::ObjectHash(uint32_t num, HashContext& context) const {
    Slot& slot = slots_[num];
    if (slot.hashed.load(std::memory_order_acquire)) {
        return PdfHash{slot.hashLo.load(std::memory_order_relaxed), slot.hashHi.load(std::memory_order_relaxed)};
    }
    const auto local = context.local.find(num);
    if (local != context.local.end()) {
        // A hash cut somewhere in this walk: whatever holds it depends on the walk too.
        context.incomplete = true;
        return local->second;
    }
    if (context.stack.size() >= kMaxHashStack ||
        std::find(context.stack.begin(), context.stack.end(), num) != context.stack.end()) {
        context.incomplete = true;
        return PdfHash{kCutObject, kCutObject};
    }
    const bool outerIncomplete = context.incomplete;
    context.incomplete = false;
    context.stack.push_back(num);
    Hasher hasher;
    HashInto(Object(num), hasher, context);
    context.stack.pop_back();
    const PdfHash hash = hasher.Finish();
    if (context.incomplete) {
        context.local.emplace(num, hash);
    } else {
        // Racing threads store the same halves.
        slot.hashLo.store(hash.lo, std::memory_order_relaxed);
        slot.hashHi.store(hash.hi, std::memory_order_relaxed);
        slot.hashed.store(true, std::memory_order_release);
    }
    context.incomplete = context.incomplete || outerIncomplete;
    return hash;
}

PdfHash PdfFile::ValueHash(const PdfValue& value) const {
    HashContext context;
    Hasher hash;
    HashInto(value, hash, context);
    return hash.Finish();
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

PdfHash PdfFile::PageKey(int page) const {
    if (page < 0 || page >= pageCount_) return PdfHash{};
    const Page& entry = pages_[page];
    Hasher hash;
    hash.Double(entry.mediaBox.x0);
    hash.Double(entry.mediaBox.y0);
    hash.Double(entry.mediaBox.x1);
    hash.Double(entry.mediaBox.y1);
    auto mixStream = [&](const PdfValue& stream) {
        if (!stream.IsStream()) {
            hash.Byte(0);
            return;
        }
        hash.Byte(1);
        hash.U64(stream.streamLength);
        hash.Bytes(std::string_view(bytes_).substr(stream.streamOffset, stream.streamLength));
    };
    const PdfValue& contents = Get(*entry.dict, "Contents");
    if (contents.IsArray()) {
        hash.U64(contents.items.size());
        for (const PdfValue& item : contents.items) mixStream(Resolve(item));
    } else {
        mixStream(contents);
    }
    HashContext context;
    if (entry.resources) {
        HashInto(*entry.resources, hash, context);
    } else {
        hash.Byte(0);
    }
    return hash.Finish();
}

} // namespace rgui::pdf
