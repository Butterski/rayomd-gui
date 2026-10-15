// SPDX-License-Identifier: Apache-2.0
// The content-stream lexer (ISO 32000-1 section 7.8.2): operators with their operands, for the
// renderer to interpret. Malformed input never stops it: a broken operand is dropped together
// with the operands before it, stray delimiters are skipped, and every step consumes input, so
// a stream of any bytes is read to its end in linear time.
#pragma once

#include "pdf/syntax.h"
#include "pdf/value.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace rgui::pdf {

class ContentLexer {
public:
    // At most this many operands are kept for one operator; more drop all before them.
    static constexpr size_t kMaxOperands = 256;

    explicit ContentLexer(std::string_view data) : cursor_{data, 0} {}

    // The next operator and the operands before it (numbers, names, strings, arrays,
    // dictionaries, booleans, null; no references). False at the end of the data; operands left
    // without an operator there are dropped.
    //
    // An inline image comes as the operator "BI" with one operand, its dictionary (keys as
    // written, abbreviations included), and InlineImageData() holding the bytes between "ID"
    // and "EI"; the lexer continues after "EI". `op` and InlineImageData() point into the data;
    // the operands are copies.
    bool Next(std::string_view& op, std::vector<PdfValue>& operands);

    // The data of the inline image Next just returned, empty after any other operator.
    std::string_view InlineImageData() const { return inlineData_; }
    // Bytes of the data read so far.
    size_t Offset() const { return cursor_.pos; }

private:
    void ReadInlineImage(std::vector<PdfValue>& operands);

    syntax::Cursor cursor_;
    std::string_view inlineData_;
};

} // namespace rgui::pdf
