// SPDX-License-Identifier: Apache-2.0
// Unicode properties from unicode_tables.inc (Unicode 16.0.0).
#pragma once

#include <cstdint>

namespace rgui::text {

// Editor cells: 2 for East Asian Wide and Fullwidth, 0 for Mn, Me and Cf (U+00AD is 1), else 1.
int CellWidth(uint32_t cp);
bool IsWhiteSpace(uint32_t cp);
// Simple case mappings; the code point itself when it has none.
uint32_t ToUpper(uint32_t cp);
uint32_t ToLower(uint32_t cp);

} // namespace rgui::text
