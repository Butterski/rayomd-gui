// SPDX-License-Identifier: Apache-2.0
#include "text/unicode.h"

#include <cstddef>

namespace rgui::text {
namespace {

struct CodepointRange {
    uint32_t first;
    uint32_t last;
};

struct CaseMapping {
    uint32_t from;
    uint32_t to;
};

#include "text/unicode_tables.inc"

template <size_t N>
bool InRanges(const CodepointRange (&ranges)[N], uint32_t cp) {
    size_t lo = 0;
    size_t hi = N;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].first) hi = mid;
        else if (cp > ranges[mid].last) lo = mid + 1;
        else return true;
    }
    return false;
}

template <size_t N>
uint32_t Map(const CaseMapping (&mappings)[N], uint32_t cp) {
    size_t lo = 0;
    size_t hi = N;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < mappings[mid].from) hi = mid;
        else if (cp > mappings[mid].from) lo = mid + 1;
        else return mappings[mid].to;
    }
    return cp;
}

} // namespace

int CellWidth(uint32_t cp) {
    if (cp < 0x300) return 1;  // nothing below U+0300 is wide or zero width (U+00AD is 1)
    if (InRanges(kZeroWidthRanges, cp)) return 0;
    if (InRanges(kWideRanges, cp)) return 2;
    return 1;
}

bool IsWhiteSpace(uint32_t cp) {
    return InRanges(kWhiteSpaceRanges, cp);
}

uint32_t ToUpper(uint32_t cp) {
    return Map(kToUpper, cp);
}

uint32_t ToLower(uint32_t cp) {
    return Map(kToLower, cp);
}

} // namespace rgui::text
