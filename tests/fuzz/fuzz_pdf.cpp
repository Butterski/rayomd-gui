// SPDX-License-Identifier: Apache-2.0
// The preview's PDF path: PdfFile::Parse, the content lexer on the raw bytes, and RenderPage of
// the first pages at a small width (the budget: at most 2 pages of 48 px; fonts and images are
// decoded on every render, no system font index, so runs are independent of each other).
#include "harness.h"

#include "gfx/surface.h"
#include "pdf/content.h"
#include "pdf/reader.h"
#include "pdf/renderer.h"

#include <string>
#include <string_view>
#include <vector>

namespace rgui::fuzz {

int FuzzPdf(const uint8_t* data, size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);

    pdf::ContentLexer lexer(bytes);
    std::string_view op;
    std::vector<pdf::PdfValue> operands;
    size_t last = 0;
    while (lexer.Next(op, operands)) {
        if (lexer.Offset() <= last && !op.empty()) Fail("ContentLexer did not consume input");
        last = lexer.Offset();
    }

    std::string error;
    const auto file = pdf::PdfFile::Parse(std::string(bytes), &error);
    if (!file) return 0;
    const pdf::RenderEnvironment environment;  // no font index, no caches
    const int pages = file->PageCount() < 2 ? file->PageCount() : 2;
    for (int page = 0; page < pages; ++page) {
        (void)file->PageKey(page);
        gfx::Image image;
        pdf::RenderStats stats;
        pdf::RenderPage(*file, page, 48, image, &stats, environment);
    }
    return 0;
}

} // namespace rgui::fuzz
