// SPDX-License-Identifier: Apache-2.0
#include "runner.h"

#include "text/unicode.h"

RGUI_TEST("text.cell_width") {
    using rgui::text::CellWidth;
    CHECK_EQ(CellWidth(0x4E2D), 2);  // 中
    CHECK_EQ(CellWidth(0x0301), 0);  // combining acute
    CHECK_EQ(CellWidth('a'), 1);
    CHECK_EQ(CellWidth(0x00AD), 1);  // soft hyphen
    CHECK_EQ(CellWidth(0x200B), 0);  // zero width space (Cf)
    CHECK_EQ(CellWidth(0xFF21), 2);  // fullwidth A
    CHECK_EQ(CellWidth(0x1F600), 2); // emoji, East Asian Wide
}

RGUI_TEST("text.unicode_properties") {
    using namespace rgui::text;
    CHECK(IsWhiteSpace(' '));
    CHECK(IsWhiteSpace(0x3000));
    CHECK(IsWhiteSpace(0x2028));
    CHECK(!IsWhiteSpace('a'));
    CHECK(!IsWhiteSpace(0x200B));
    CHECK_EQ(ToUpper(0x0105), 0x0104u);  // ą -> Ą
    CHECK_EQ(ToLower(0x0141), 0x0142u);  // Ł -> ł
    CHECK_EQ(ToUpper('7'), uint32_t('7'));
    CHECK_EQ(ToLower(0x00DF), 0x00DFu);  // ß has no simple mapping
}
