// SPDX-License-Identifier: Apache-2.0
// Helpers of the renderer tests: PDFs built by the engine or by hand, and the render environment
// the tests share.
#pragma once

#include "pdf/renderer.h"
#include "rayomd/tiny_pdf.h"
#include "text/fontindex.h"

#include <cstdio>
#include <string>
#include <vector>

namespace rgui::pdftest {

// One font index of the system's fonts for every renderer test, scanned on first use.
inline const text::FontIndex& TestFontIndex() {
    static text::FontIndex index;
    index.Wait();
    return index;
}

// The test environment: TestFontIndex() and caches of its own.
inline const pdf::RenderEnvironment& TestEnvironment() {
    static pdf::ImageCache images;
    static pdf::FontCache fonts;
    static const pdf::RenderEnvironment environment{&TestFontIndex(), &images, &fonts};
    return environment;
}

// The preview's PDF of `markdown`, as the app builds previews: uncompressed. Empty on failure.
inline std::string BuildPreview(const std::string& markdown, TinyPdf::PdfOptions options = {}) {
    options.compress = false;
    std::string pdf;
    if (!TinyPdf::BuildPdf(markdown, options, pdf).Ok()) pdf.clear();
    return pdf;
}

// A stream object's body.
inline std::string Stream(const std::string& data, const std::string& extra = "") {
    return "<< /Length " + std::to_string(data.size()) + extra + " >>\nstream\n" + data + "\nendstream";
}

// Objects 1..n with these bodies, a classic cross-reference table and a trailer with /Root 1 0 R.
inline std::string MakePdf(const std::vector<std::string>& bodies) {
    std::string out = "%PDF-1.7\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < bodies.size(); i++) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(bodies.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t offset : offsets) {
        char entry[32];
        std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", offset);
        out += entry;
    }
    out += "trailer\n<< /Size " + std::to_string(bodies.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return out;
}

// A one-page PDF: the page's MediaBox [0 0 width height], its /Resources dictionary `resources`
// and content `content`; objects 5 on are `extra` (refer to them as "5 0 R", ...).
inline std::string PagePdf(const std::string& content, const std::string& resources = "<< >>", double width = 100,
                           double height = 100, const std::vector<std::string>& extra = {}) {
    char box[96];
    std::snprintf(box, sizeof box, "[0 0 %g %g]", width, height);
    std::vector<std::string> bodies = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox " + std::string(box) + " /Resources " + resources +
            " /Contents 4 0 R >>",
        Stream(content)};
    bodies.insert(bodies.end(), extra.begin(), extra.end());
    return MakePdf(bodies);
}

} // namespace rgui::pdftest
