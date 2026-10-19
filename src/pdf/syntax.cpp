// SPDX-License-Identifier: Apache-2.0
#include "pdf/syntax.h"

#include <cstdint>

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

const std::vector<PdfValue>& PdfValue::Array() const {
    static const std::vector<PdfValue> kEmpty;
    return type == Type::Array ? items : kEmpty;
}

const PdfValue* PdfValue::Find(std::string_view key) const {
    if (!IsDict()) return nullptr;
    for (const PdfDictEntry& entry : entries) {
        if (entry.key == key) return &entry.value;
    }
    return nullptr;
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
        out.Reset(PdfValue::Type::Int);
        out.integer = negative ? -static_cast<int64_t>(mantissa) : static_cast<int64_t>(mantissa);
        return true;
    }
    double value = static_cast<double>(mantissa);
    if (scale > 0) value *= PowerOfTen(scale);
    if (fraction > 0) value /= PowerOfTen(fraction);
    out.Reset(PdfValue::Type::Real);
    out.real = negative ? -value : value;
    return true;
}

void ReadName(Cursor& c, std::string& out) {
    out.clear();
    c.pos++;  // '/'
    while (c.pos < c.data.size()) {
        const unsigned char ch = static_cast<unsigned char>(c.data[c.pos]);
        if (!IsRegular(ch)) break;
        if (ch == '#' && HexDigit(c.Peek(1)) >= 0 && HexDigit(c.Peek(2)) >= 0) {
            out.push_back(static_cast<char>(HexDigit(c.Peek(1)) * 16 + HexDigit(c.Peek(2))));
            c.pos += 3;
        } else {
            out.push_back(static_cast<char>(ch));
            c.pos++;
        }
    }
}

bool ReadLiteralString(Cursor& c, std::string& out) {
    out.clear();
    c.pos++;  // '('
    int depth = 1;
    const size_t size = c.data.size();
    while (c.pos < size) {
        const char ch = c.data[c.pos++];
        if (ch == '\\') {
            if (c.pos >= size) return false;
            const char e = c.data[c.pos++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
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
                        out.push_back(static_cast<char>(value & 0xFF));
                    } else {
                        out.push_back(e);  // \( \) \\ and an unknown escape: the character itself
                    }
                    break;
            }
        } else if (ch == '(') {
            depth++;
            out.push_back(ch);
        } else if (ch == ')') {
            if (--depth == 0) return true;
            out.push_back(ch);
        } else if (ch == '\r') {  // an end of line in a string is a line feed
            out.push_back('\n');
            if (c.pos < size && c.data[c.pos] == '\n') c.pos++;
        } else {
            out.push_back(ch);
        }
    }
    return false;
}

bool ReadHexString(Cursor& c, std::string& out) {
    out.clear();
    c.pos++;  // '<'
    int high = -1;
    while (c.pos < c.data.size()) {
        const unsigned char ch = static_cast<unsigned char>(c.data[c.pos++]);
        if (ch == '>') {
            if (high >= 0) out.push_back(static_cast<char>(high << 4));
            return true;
        }
        if (IsWhite(ch)) continue;
        const int d = HexDigit(ch);
        if (d < 0) return false;
        if (high < 0) {
            high = d;
        } else {
            out.push_back(static_cast<char>(high << 4 | d));
            high = -1;
        }
    }
    return false;
}

bool ParseValue(Cursor& c, Mode mode, int depth, PdfValue& out) {
    if (c.AtEnd()) return false;
    const unsigned char ch = c.Peek();
    switch (ch) {
        case '/':
            out.Reset(PdfValue::Type::Name);
            ReadName(c, out.text);
            return true;
        case '(':
            out.Reset(PdfValue::Type::String);
            return ReadLiteralString(c, out.text);
        case '<':
            if (c.Peek(1) != '<') {
                out.Reset(PdfValue::Type::String);
                return ReadHexString(c, out.text);
            } else {
                if (depth >= kMaxDepth) return false;
                c.pos += 2;
                // Filled in place; on failure `out` is left half filled, which callers drop.
                out.Reset(PdfValue::Type::Dict);
                for (;;) {
                    SkipSpace(c);
                    if (c.AtEnd()) return false;
                    if (c.Peek() == '>' && c.Peek(1) == '>') {
                        c.pos += 2;
                        break;
                    }
                    if (c.Peek() != '/') return false;
                    PdfDictEntry& entry = out.entries.emplace_back();
                    ReadName(c, entry.key);
                    SkipSpace(c);
                    if (c.Peek() == '>' && c.Peek(1) == '>') {  // a key without a value
                        out.entries.pop_back();
                        continue;
                    }
                    if (!ParseValue(c, mode, depth + 1, entry.value)) return false;
                }
                return true;
            }
        case '[': {
            if (depth >= kMaxDepth) return false;
            c.pos++;
            out.Reset(PdfValue::Type::Array);
            for (;;) {
                SkipSpace(c);
                if (c.AtEnd()) return false;
                if (c.Peek() == ']') {
                    c.pos++;
                    break;
                }
                if (!ParseValue(c, mode, depth + 1, out.items.emplace_back())) return false;
            }
            return true;
        }
        default:
            break;
    }
    if (!IsRegular(ch)) return false;
    const std::string_view token = ReadToken(c);
    if (token == "true" || token == "false") {
        out.Reset(PdfValue::Type::Bool);
        out.boolean = token == "true";
        return true;
    }
    if (token == "null") {
        out.Reset(PdfValue::Type::Null);
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
                out.Reset(PdfValue::Type::Ref);
                out.refNum = num;
                out.refGen = gen;
                c = ahead;
            }
        }
    }
    return true;
}

} // namespace syntax
} // namespace rgui::pdf
