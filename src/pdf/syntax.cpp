// SPDX-License-Identifier: Apache-2.0
#include "pdf/syntax.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace rgui::pdf {

double PdfValue::Number(double fallback) const {
    if (type == Type::Int) return static_cast<double>(integer);
    if (type == Type::Real) return real;
    return fallback;
}

int64_t PdfValue::Int(int64_t fallback) const {
    if (type == Type::Int) return integer;
    // 2^63 is exact in a double; a NaN fails both comparisons.
    if (type == Type::Real && real > -9223372036854775808.0 && real < 9223372036854775808.0) {
        return static_cast<int64_t>(real);
    }
    return fallback;
}

const PdfValue* PdfValue::Find(std::string_view key) const {
    for (const PdfDictEntry& entry : Entries()) {
        if (entry.key == key) return &entry.value;
    }
    return nullptr;
}

namespace {

// Chunks of the largest regular size (kBigChunk with their header) that arenas dropped, for the
// next arena: a preview's PDF of a long document fills a few MB of them per parse, which the
// system would otherwise map afresh every time. At most kKeptChunks; never destroyed (arenas may
// go during the process's exit).
constexpr size_t kBigChunk = size_t(1) << 20;
constexpr size_t kKeptChunks = 16;

struct ChunkPool {
    std::mutex mutex;
    std::vector<void*> chunks;
};

ChunkPool& Chunks() {
    static ChunkPool* const pool = new ChunkPool;
    return *pool;
}

void FreeChunk(void* chunk, size_t size) {
    if (size == kBigChunk) {
        ChunkPool& pool = Chunks();
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (pool.chunks.size() < kKeptChunks) {
            pool.chunks.push_back(chunk);
            return;
        }
    }
    std::free(chunk);
}

void* NewChunk(size_t bytes) {
    if (bytes == kBigChunk) {
        ChunkPool& pool = Chunks();
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (!pool.chunks.empty()) {
            void* chunk = pool.chunks.back();
            pool.chunks.pop_back();
            return chunk;
        }
    }
    return std::malloc(bytes);
}

} // namespace

PdfArena::~PdfArena() {
    while (head_) {
        Chunk* next = head_->next;
        FreeChunk(head_, sizeof(Chunk) + head_->size);
        head_ = next;
    }
}

void* PdfArena::Allocate(size_t bytes) {
    // Far beyond any input: a size that cannot be meant.
    if (bytes > (size_t(1) << 40)) return nullptr;
    bytes = (bytes + 7) & ~size_t(7);
    if (bytes > static_cast<size_t>(end_ - at_)) {
        // Chunks double from 4 KiB to 1 MiB with their header; a bigger block gets a chunk of its own.
        constexpr size_t kLargest = kBigChunk - sizeof(Chunk);
        const size_t grown = std::min<size_t>(std::max<size_t>(head_ ? head_->size * 2 : 0, 4096), kLargest);
        const size_t size = std::max(grown, bytes);
        Chunk* chunk = static_cast<Chunk*>(NewChunk(sizeof(Chunk) + size));
        if (!chunk) return nullptr;
        chunk->next = head_;
        chunk->size = size;
        head_ = chunk;
        at_ = reinterpret_cast<char*>(chunk + 1);
        end_ = at_ + size;
        capacity_ += size;
    }
    void* block = at_;
    at_ += bytes;
    return block;
}

void PdfArena::Reset() {
    if (!head_) return;
    Chunk* rest = head_->next;
    while (rest) {
        Chunk* next = rest->next;
        FreeChunk(rest, sizeof(Chunk) + rest->size);
        rest = next;
    }
    head_->next = nullptr;
    at_ = reinterpret_cast<char*>(head_ + 1);
    end_ = at_ + head_->size;
    capacity_ = head_->size;
}

namespace syntax {
namespace {

int HexDigit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 10^n for n >= 0, exact up to 10^22; larger powers multiply up and saturate to infinity.
double PowerOfTen(size_t n) {
    static const double kExact[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (n <= 22) return kExact[n];
    double value = kExact[22];
    for (n -= 22; n > 0 && value < 1e308; n--) value *= 10.0;
    return n > 0 ? value * 10.0 : value;
}

bool AllDigits(std::string_view token) {
    if (token.empty()) return false;
    for (char ch : token) {
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

} // namespace

void SkipSpace(Cursor& c) {
    while (c.pos < c.data.size()) {
        const unsigned char ch = static_cast<unsigned char>(c.data[c.pos]);
        if (IsWhite(ch)) {
            c.pos++;
        } else if (ch == '%') {
            while (c.pos < c.data.size() && c.data[c.pos] != '\n' && c.data[c.pos] != '\r') c.pos++;
        } else {
            break;
        }
    }
}

void SkipWhite(Cursor& c) {
    while (c.pos < c.data.size() && IsWhite(static_cast<unsigned char>(c.data[c.pos]))) c.pos++;
}

std::string_view ReadToken(Cursor& c) {
    const size_t start = c.pos;
    while (c.pos < c.data.size() && IsRegular(static_cast<unsigned char>(c.data[c.pos]))) c.pos++;
    return c.data.substr(start, c.pos - start);
}

bool ParseNumber(std::string_view token, PdfValue& out) {
    size_t i = 0;
    bool negative = false;
    if (i < token.size() && (token[i] == '+' || token[i] == '-')) negative = token[i++] == '-';
    // Up to 19 significant digits in `mantissa`; integer digits past them raise `scale`,
    // fraction digits past them are dropped.
    uint64_t mantissa = 0;
    size_t significant = 0;
    size_t scale = 0;
    size_t fraction = 0;
    size_t digits = 0;
    bool point = false;
    for (; i < token.size(); i++) {
        const char ch = token[i];
        if (ch == '.') {
            if (point) return false;
            point = true;
            continue;
        }
        if (ch < '0' || ch > '9') return false;
        digits++;
        const unsigned d = static_cast<unsigned>(ch - '0');
        if (significant < 19) {
            if (mantissa != 0 || d != 0) significant++;
            mantissa = mantissa * 10 + d;
            if (point) fraction++;
        } else if (!point) {
            scale++;
        }
    }
    if (digits == 0) return false;
    // 19 digits stay below 2^64; past INT64_MAX the number is a Real.
    if (!point && scale == 0 && mantissa <= static_cast<uint64_t>(INT64_MAX)) {
        out = PdfValue();
        out.type = PdfValue::Type::Int;
        out.integer = negative ? -static_cast<int64_t>(mantissa) : static_cast<int64_t>(mantissa);
        return true;
    }
    double value = static_cast<double>(mantissa);
    if (scale > 0) value *= PowerOfTen(scale);
    if (fraction > 0) value /= PowerOfTen(fraction);
    out = PdfValue();
    out.type = PdfValue::Type::Real;
    out.real = negative ? -value : value;
    return true;
}

namespace {

// `bytes` copied into the arena (a value counts at most UINT32_MAX of them).
bool Store(Builder& b, std::string_view bytes, std::string_view& out) {
    if (bytes.empty()) {
        out = {};
        return true;
    }
    if (bytes.size() > UINT32_MAX) return false;
    char* block = static_cast<char*>(b.arena.Allocate(bytes.size()));
    if (!block) return false;
    std::memcpy(block, bytes.data(), bytes.size());
    out = std::string_view(block, bytes.size());
    return true;
}

} // namespace

bool ReadName(Cursor& c, Builder& b, std::string_view& out) {
    c.pos++;  // '/'
    // A #xx escape is three regular bytes: the name ends where they do.
    const size_t start = c.pos;
    bool escaped = false;
    while (c.pos < c.data.size()) {
        const unsigned char ch = static_cast<unsigned char>(c.data[c.pos]);
        if (!IsRegular(ch)) break;
        escaped |= ch == '#';
        c.pos++;
    }
    const std::string_view run = c.data.substr(start, c.pos - start);
    if (!escaped) return Store(b, run, out);
    std::string& decoded = b.scratch;
    decoded.clear();
    for (size_t i = 0; i < run.size(); i++) {
        const unsigned char ch = static_cast<unsigned char>(run[i]);
        if (ch == '#' && i + 2 < run.size() && HexDigit(static_cast<unsigned char>(run[i + 1])) >= 0 &&
            HexDigit(static_cast<unsigned char>(run[i + 2])) >= 0) {
            decoded.push_back(static_cast<char>(HexDigit(static_cast<unsigned char>(run[i + 1])) * 16 +
                                                HexDigit(static_cast<unsigned char>(run[i + 2]))));
            i += 2;
        } else {
            decoded.push_back(static_cast<char>(ch));
        }
    }
    return Store(b, decoded, out);
}

bool ReadLiteralString(Cursor& c, Builder& b, std::string_view& out) {
    std::string& text = b.scratch;
    text.clear();
    c.pos++;  // '('
    int depth = 1;
    const size_t size = c.data.size();
    while (c.pos < size) {
        // The run up to the next byte that needs a look, at once.
        const size_t runStart = c.pos;
        while (c.pos < size) {
            const char ch = c.data[c.pos];
            if (ch == '\\' || ch == '(' || ch == ')' || ch == '\r') break;
            c.pos++;
        }
        text.append(c.data.data() + runStart, c.pos - runStart);
        if (c.pos >= size) break;
        const char ch = c.data[c.pos++];
        if (ch == '\\') {
            if (c.pos >= size) return false;
            const char e = c.data[c.pos++];
            switch (e) {
                case 'n': text.push_back('\n'); break;
                case 'r': text.push_back('\r'); break;
                case 't': text.push_back('\t'); break;
                case 'b': text.push_back('\b'); break;
                case 'f': text.push_back('\f'); break;
                case '\r':  // a line continuation
                    if (c.pos < size && c.data[c.pos] == '\n') c.pos++;
                    break;
                case '\n': break;
                default:
                    if (e >= '0' && e <= '7') {
                        unsigned value = static_cast<unsigned>(e - '0');
                        for (int k = 0; k < 2 && c.pos < size && c.data[c.pos] >= '0' && c.data[c.pos] <= '7'; k++) {
                            value = value * 8 + static_cast<unsigned>(c.data[c.pos++] - '0');
                        }
                        text.push_back(static_cast<char>(value & 0xFF));
                    } else {
                        text.push_back(e);  // \( \) \\ and an unknown escape: the character itself
                    }
                    break;
            }
        } else if (ch == '(') {
            depth++;
            text.push_back(ch);
        } else if (ch == ')') {
            if (--depth == 0) return Store(b, text, out);
            text.push_back(ch);
        } else {  // '\r': an end of line in a string is a line feed
            text.push_back('\n');
            if (c.pos < size && c.data[c.pos] == '\n') c.pos++;
        }
    }
    return false;
}

bool ReadHexString(Cursor& c, Builder& b, std::string_view& out) {
    std::string& text = b.scratch;
    text.clear();
    c.pos++;  // '<'
    int high = -1;
    while (c.pos < c.data.size()) {
        const unsigned char ch = static_cast<unsigned char>(c.data[c.pos++]);
        if (ch == '>') {
            if (high >= 0) text.push_back(static_cast<char>(high << 4));
            return Store(b, text, out);
        }
        if (IsWhite(ch)) continue;
        const int d = HexDigit(ch);
        if (d < 0) return false;
        if (high < 0) {
            high = d;
        } else {
            text.push_back(static_cast<char>(high << 4 | d));
            high = -1;
        }
    }
    return false;
}

bool ParseValue(Cursor& c, Mode mode, int depth, Builder& b, PdfValue& out) {
    if (c.AtEnd()) return false;
    const unsigned char ch = c.Peek();
    switch (ch) {
        case '/':
        case '(':
        case '<': {
            if (ch == '<' && c.Peek(1) == '<') break;
            std::string_view text;
            const bool read = ch == '/' ? ReadName(c, b, text)
                              : ch == '(' ? ReadLiteralString(c, b, text)
                                          : ReadHexString(c, b, text);
            if (!read) return false;
            out = PdfValue();
            out.type = ch == '/' ? PdfValue::Type::Name : PdfValue::Type::String;
            out.count = static_cast<uint32_t>(text.size());
            out.data = text.data();
            return true;
        }
        case '[':
            break;
        default: {
            if (!IsRegular(ch)) return false;
            const std::string_view token = ReadToken(c);
            if (token == "true" || token == "false") {
                out = PdfValue();
                out.type = PdfValue::Type::Bool;
                out.boolean = token == "true";
                return true;
            }
            if (token == "null") {
                out = PdfValue();
                return true;
            }
            if (!ParseNumber(token, out)) return false;
            // "n g R": a reference, read ahead without consuming anything else.
            if (mode == Mode::File && out.type == PdfValue::Type::Int && AllDigits(token) && out.integer <= UINT32_MAX) {
                Cursor ahead = c;
                SkipSpace(ahead);
                const std::string_view generation = ReadToken(ahead);
                if (AllDigits(generation) && generation.size() <= 5) {
                    uint32_t gen = 0;
                    for (char g : generation) gen = gen * 10 + static_cast<uint32_t>(g - '0');
                    SkipSpace(ahead);
                    if (gen <= 65535 && ReadToken(ahead) == "R") {
                        const uint32_t num = static_cast<uint32_t>(out.integer);
                        out = PdfValue();
                        out.type = PdfValue::Type::Ref;
                        out.refNum = num;
                        out.refGen = static_cast<uint16_t>(gen);
                        c = ahead;
                    }
                }
            }
            return true;
        }
    }
    // An array or a dictionary: read on the builder's stacks above what the values around it
    // hold there, then copied to the arena whole.
    if (depth >= kMaxDepth) return false;
    if (ch == '[') {
        c.pos++;
        const size_t base = b.items.size();
        bool ok = false;
        for (;;) {
            SkipSpace(c);
            if (c.AtEnd()) break;
            if (c.Peek() == ']') {
                c.pos++;
                ok = true;
                break;
            }
            PdfValue item;
            if (!ParseValue(c, mode, depth + 1, b, item)) break;
            b.items.push_back(item);
        }
        const size_t n = b.items.size() - base;
        PdfValue* items = nullptr;
        if (ok && n > 0) {
            items = n <= UINT32_MAX ? static_cast<PdfValue*>(b.arena.Allocate(n * sizeof(PdfValue))) : nullptr;
            if (items) std::memcpy(static_cast<void*>(items), b.items.data() + base, n * sizeof(PdfValue));
            ok = items != nullptr;
        }
        b.items.resize(base);
        if (!ok) return false;
        out = PdfValue();
        out.type = PdfValue::Type::Array;
        out.count = static_cast<uint32_t>(n);
        out.data = items;
        return true;
    }
    c.pos += 2;  // "<<"
    const size_t base = b.entries.size();
    bool ok = false;
    for (;;) {
        SkipSpace(c);
        if (c.AtEnd()) break;
        if (c.Peek() == '>' && c.Peek(1) == '>') {
            c.pos += 2;
            ok = true;
            break;
        }
        if (c.Peek() != '/') break;
        PdfDictEntry entry;
        if (!ReadName(c, b, entry.key)) break;
        SkipSpace(c);
        if (c.Peek() == '>' && c.Peek(1) == '>') continue;  // a key without a value
        if (!ParseValue(c, mode, depth + 1, b, entry.value)) break;
        b.entries.push_back(entry);
    }
    const size_t n = b.entries.size() - base;
    PdfDictEntry* entries = nullptr;
    if (ok && n > 0) {
        entries = n <= UINT32_MAX ? static_cast<PdfDictEntry*>(b.arena.Allocate(n * sizeof(PdfDictEntry))) : nullptr;
        if (entries) std::memcpy(static_cast<void*>(entries), b.entries.data() + base, n * sizeof(PdfDictEntry));
        ok = entries != nullptr;
    }
    b.entries.resize(base);
    if (!ok) return false;
    out = PdfValue();
    out.type = PdfValue::Type::Dict;
    out.count = static_cast<uint32_t>(n);
    out.data = entries;
    return true;
}

} // namespace syntax
} // namespace rgui::pdf
