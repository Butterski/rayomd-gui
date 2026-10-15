// SPDX-License-Identifier: Apache-2.0
// One PDF value (ISO 32000-1 section 7.3): what the object reader and the content-stream lexer
// produce.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::pdf {

struct PdfDictEntry;

// A string holds its bytes as decoded from the literal or hexadecimal form, a name its bytes
// after '/' with #xx escapes decoded. A stream is its dictionary (`entries`) plus the byte range
// of its data in the file; PdfFile::StreamData and PdfFile::RawStream read it.
struct PdfValue {
    enum class Type : uint8_t { Null, Bool, Int, Real, String, Name, Array, Dict, Ref, Stream };

    Type type = Type::Null;
    bool boolean = false;
    int64_t integer = 0;
    double real = 0;
    uint32_t refNum = 0;
    uint32_t refGen = 0;
    size_t streamOffset = 0;
    size_t streamLength = 0;
    std::string text;                   // String bytes, Name
    std::vector<PdfValue> items;        // Array
    std::vector<PdfDictEntry> entries;  // Dict, Stream

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
    // The name's bytes, or "" for anything but a name.
    std::string_view Name() const { return type == Type::Name ? std::string_view(text) : std::string_view(); }
    bool IsName(std::string_view name) const { return type == Type::Name && text == name; }
    // The string's bytes, or "" for anything but a string.
    std::string_view String() const { return type == Type::String ? std::string_view(text) : std::string_view(); }
    // The array's items; none for anything but an array.
    const std::vector<PdfValue>& Array() const;
    // The entry under `key` of a dictionary or stream, unresolved (the first one when a key
    // repeats); nullptr when absent or when this is no dictionary.
    const PdfValue* Find(std::string_view key) const;
};

struct PdfDictEntry {
    std::string key;
    PdfValue value;
};

} // namespace rgui::pdf
