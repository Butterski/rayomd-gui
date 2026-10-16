// SPDX-License-Identifier: Apache-2.0
// Where the preview shows the Markdown: the engine's map of a preview build
// (PdfOptions::sourceMap, one TinyPdf::SourceBlock per top-level block) as byte ranges of the
// text built and positions in the PDF, and the piecewise-linear mapping between the editor's
// lines and those positions that the main window's source sync uses. Byte offsets bridge the two
// texts, so an edit made since the build shifts the mapping only where it changed the text.
#pragma once

#include "rayomd/tiny_pdf.h"
#include "ui/textbuffer.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace rgui {

// One top-level block of a preview build: its bytes in the text built and where the PDF shows it,
// as page positions (page index + the fraction of that page's height above the point).
struct SourceSpan {
    size_t begin = 0;   // byte offset of its first line
    size_t end = 0;     // byte offset of the line after its last (the text's size at the end)
    double start = 0;   // its top
    double stop = 0;    // its bottom
};

class SourceMap {
public:
    // From the engine's map of `text` (the text the build was given) and the heights in points of
    // the PDF's pages. One pass over the text; blocks the engine gave the same lines merge, and a
    // block that goes back in the text or the pages is dropped.
    static SourceMap FromEngine(const std::vector<TinyPdf::SourceBlock>& blocks, std::string_view text,
                                const std::vector<double>& pageHeightsPt);
    bool Empty() const { return spans_.empty(); }
    const std::vector<SourceSpan>& Spans() const { return spans_; }
    // The page position showing fractional line `line` of `buffer`, whose text may have changed
    // since the build (offsets clamp to it); -1 when empty.
    double PageOfLine(const ui::TextBuffer& buffer, double line) const;
    // The fractional line of `buffer` shown at page position `page`; -1 when empty.
    double LineOfPage(const ui::TextBuffer& buffer, double page) const;

private:
    std::vector<SourceSpan> spans_;
    double pages_ = 0;   // the page position of the PDF's end: its page count
};

} // namespace rgui
