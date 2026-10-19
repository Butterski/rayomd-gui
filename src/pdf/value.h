// SPDX-License-Identifier: Apache-2.0
// One PDF value (ISO 32000-1 section 7.3): what the object reader and the content-stream lexer
// produce. A value is 32 bytes and trivially copyable: strings, names, arrays and dictionaries
// view bytes, items and entries in a PdfArena that whoever parsed them owns (a PdfFile's objects
// live as long as the file, the content lexer's operands until its next operator).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rgui::pdf {

struct PdfDictEntry;

// `size` items held elsewhere.
template <class T>
class PdfSpan {
public:
    PdfSpan() = default;
    PdfSpan(const T* data, size_t size) : data_(data), size_(size) {}
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const T& operator[](size_t i) const { return data_[i]; }

private:
    const T* data_ = nullptr;
    size_t size_ = 0;
};

// A string holds its bytes as decoded from the literal or hexadecimal form, a name its bytes
// after '/' with #xx escapes decoded. A stream is its dictionary plus the byte range of its data
// in the file; PdfFile::StreamData and PdfFile::RawStream read it.
struct PdfValue {
    enum class Type : uint8_t { Null, Bool, Int, Real, String, Name, Array, Dict, Ref, Stream };

    Type type = Type::Null;
    bool boolean = false;
    uint16_t refGen = 0;
    union {
        uint32_t refNum = 0;
        uint32_t count;  // bytes of a String or Name, items of an Array, entries of a Dict or Stream
    };
    union {
        int64_t integer = 0;
        double real;
        const void* data;  // the bytes, items or entries `count` counts
    };
    size_t streamOffset = 0;
    size_t streamLength = 0;

    bool IsNull() const { return type == Type::Null; }
    bool IsNumber() const { return type == Type::Int || type == Type::Real; }
    bool IsName() const { return type == Type::Name; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    // A dictionary or a stream's dictionary.
    bool IsDict() const { return type == Type::Dict || type == Type::Stream; }
    bool IsStream() const { return type == Type::Stream; }
    bool IsRef() const { return type == Type::Ref; }

    // An Int or Real as a double; `fallback` for anything else.
    double Number(double fallback = 0) const;
    // An Int, or a Real truncated toward zero when it fits; `fallback` for anything else.
    int64_t Int(int64_t fallback = 0) const;
    bool Bool(bool fallback = false) const { return type == Type::Bool ? boolean : fallback; }
    // The bytes of a string or a name; "" for anything else.
    std::string_view Text() const {
        return type == Type::String || type == Type::Name
                   ? std::string_view(static_cast<const char*>(data), count)
                   : std::string_view();
    }
    // The name's bytes, or "" for anything but a name.
    std::string_view Name() const { return type == Type::Name ? Text() : std::string_view(); }
    bool IsName(std::string_view name) const { return type == Type::Name && Text() == name; }
    // The string's bytes, or "" for anything but a string.
    std::string_view String() const { return type == Type::String ? Text() : std::string_view(); }
    // The array's items; none for anything but an array.
    PdfSpan<PdfValue> Array() const {
        return type == Type::Array ? PdfSpan<PdfValue>(static_cast<const PdfValue*>(data), count) : PdfSpan<PdfValue>();
    }
    // The entries of a dictionary or stream in their order; none for anything else.
    inline PdfSpan<PdfDictEntry> Entries() const;
    // The entry under `key` of a dictionary or stream, unresolved (the first one when a key
    // repeats); nullptr when absent or when this is no dictionary.
    const PdfValue* Find(std::string_view key) const;
};

struct PdfDictEntry {
    std::string_view key;
    PdfValue value;
};

inline PdfSpan<PdfDictEntry> PdfValue::Entries() const {
    return IsDict() ? PdfSpan<PdfDictEntry>(static_cast<const PdfDictEntry*>(data), count) : PdfSpan<PdfDictEntry>();
}

// Storage of parsed values, freed all at once: 8-byte aligned blocks bumped out of chunks.
class PdfArena {
public:
    PdfArena() = default;
    ~PdfArena();
    PdfArena(const PdfArena&) = delete;
    PdfArena& operator=(const PdfArena&) = delete;

    // `bytes` of storage that stays put until Reset or the arena goes; nullptr when it cannot be
    // had.
    void* Allocate(size_t bytes);
    // Forgets every block; the newest chunk is kept for what comes next.
    void Reset();
    // Bytes of the chunks held.
    size_t Capacity() const { return capacity_; }

private:
    struct Chunk {
        Chunk* next;
        size_t size;
    };
    Chunk* head_ = nullptr;
    char* at_ = nullptr;
    char* end_ = nullptr;
    size_t capacity_ = 0;
};

} // namespace rgui::pdf
