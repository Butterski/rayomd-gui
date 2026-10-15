// SPDX-License-Identifier: Apache-2.0
// PDF's lexical conventions (ISO 32000-1 section 7.2-7.3), shared by the object reader and the
// content-stream lexer. Every function stays within the cursor's data and consumes at least one
// byte per token, so a scan over any input ends.
#pragma once

#include "pdf/value.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace rgui::pdf::syntax {

// Arrays and dictionaries nest at most this deep; a deeper value fails.
constexpr int kMaxDepth = 64;

inline bool IsWhite(unsigned char c) {
    return c == 0 || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}
inline bool IsDelimiter(unsigned char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '/' || c == '%';
}
inline bool IsRegular(unsigned char c) { return !IsWhite(c) && !IsDelimiter(c); }
// The first byte of a number: a digit, a sign or a decimal point.
inline bool StartsNumber(unsigned char c) { return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'; }

struct Cursor {
    std::string_view data;
    size_t pos = 0;

    bool AtEnd() const { return pos >= data.size(); }
    // The byte `ahead` past the cursor, 0 past the end.
    unsigned char Peek(size_t ahead = 0) const {
        return pos + ahead < data.size() ? static_cast<unsigned char>(data[pos + ahead]) : 0;
    }
};

// Skips white space and % comments.
void SkipSpace(Cursor& c);
// Skips white space only.
void SkipWhite(Cursor& c);
// A run of regular characters at the cursor, possibly empty.
std::string_view ReadToken(Cursor& c);
// `token` as an Int or a Real: [+-]digits[.digits] or [+-].digits. An integer beyond int64 is a
// Real. False for anything else.
bool ParseNumber(std::string_view token, PdfValue& out);
// A name; the cursor is at '/'.
void ReadName(Cursor& c, std::string& out);
// A literal string with its escapes and balanced parentheses; the cursor is at '('. False when
// it does not end.
bool ReadLiteralString(Cursor& c, std::string& out);
// A hexadecimal string; the cursor is at '<' (not "<<"). False on a byte that is neither a hex
// digit nor white space, or when it does not end.
bool ReadHexString(Cursor& c, std::string& out);

enum class Mode : uint8_t {
    File,     // "n g R" is a reference
    Content,  // no references
};

// The value at the cursor (the caller skipped space before it): a number, true, false, null,
// a name, a string, an array or a dictionary, or in File mode a reference. A bare keyword, a
// stray delimiter or nesting deeper than kMaxDepth fails; the cursor is then left anywhere
// at or after where it started.
bool ParseValue(Cursor& c, Mode mode, int depth, PdfValue& out);

} // namespace rgui::pdf::syntax
