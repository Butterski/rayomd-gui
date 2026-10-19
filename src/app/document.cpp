// SPDX-License-Identifier: Apache-2.0
#include "app/document.h"

#include "app/safefile.h"
#include "core/rayomd_pdf_source.h"

#include <filesystem>
#include <system_error>
#include <utility>

#include <emmintrin.h>

namespace rgui {
namespace {

namespace fs = std::filesystem;

bool IsPdfPath(const fs::path& path) {
    const std::string ext = path.extension().u8string();
    if (ext.size() != 4 || ext[0] != '.') return false;
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return lower(ext[1]) == 'p' && lower(ext[2]) == 'd' && lower(ext[3]) == 'f';
}

// `text` with LF line ends, and whether it had CRLF ones. Every CR right before a LF goes, so
// the result never holds "\r\n" (a run "\r\r\n" becomes "\n" too). A run of CRs is read once:
// read again from each of its CRs, a file of lone CRs took time growing with its square.
std::string ToLf(const std::string& text, bool& crlf) {
    crlf = false;
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') {
            size_t end = i;
            while (end < text.size() && text[end] == '\r') end++;
            if (end < text.size() && text[end] == '\n') {
                crlf = true;
            } else {
                out.append(end - i, '\r');
            }
            i = end - 1;  // the byte after the run comes next
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string ToCrlf(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (char c : text) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

bool FileLargerThan(const fs::path& path, size_t limit) {
    std::error_code error;
    const uintmax_t size = fs::file_size(path, error);
    return !error && size > limit;
}

} // namespace

void Document::New() {
    text.clear();
    filePath.clear();
    displayName = kUntitledName;
    modified = false;
    crlf = false;
}

bool Document::Open(const std::string& path, std::string& status) {
    status.clear();
    const fs::path file = fs::u8path(path);
    const std::string name = file.filename().u8string();

    if (IsPdfPath(file)) {
        namespace PdfSource = RayoMd::PdfSource;
        std::string bytes;
        if (!ReadRegularFile(file, PdfSource::kMaxPdfBytes, bytes, nullptr)) {
            status = FileLargerThan(file, PdfSource::kMaxPdfBytes)
                         ? PdfSource::StatusMessage(PdfSource::Status::LimitExceeded)
                         : "Cannot read " + name;
            return false;
        }
        PdfSource::Result result = PdfSource::Inspect(bytes, true);
        if (!result.Ok()) {
            status = PdfSource::StatusMessage(result.status);
            return false;
        }
        bool hadCrlf = false;
        text = ToLf(result.source, hadCrlf);
        crlf = hadCrlf;
        filePath.clear();
        displayName = file.stem().u8string() + ".md";
        modified = false;
        status = "Recovered " + std::to_string(result.info.sourceBytes) + " validated bytes (SHA-256 valid) from " +
                 name + ". Ctrl+S saves the Markdown.";
        return true;
    }

    std::string bytes;
    if (!ReadRegularFile(file, kMaxTextBytes, bytes, nullptr)) {
        status = FileLargerThan(file, kMaxTextBytes) ? "Too large to open (over 128 MiB): " + name
                                                     : "Cannot read " + name;
        return false;
    }
    if (!RayoMd::PdfSource::IsValidUtf8(bytes)) {
        status = "Not a UTF-8 text file: " + name;
        return false;
    }
    bool hadCrlf = false;
    text = ToLf(bytes, hadCrlf);
    crlf = hadCrlf;
    filePath = path;
    displayName = name;
    modified = false;
    return true;
}

bool Document::Save(const std::string& path, std::string& status) {
    const fs::path file = fs::u8path(path);
    const std::string name = file.filename().u8string();
    const bool ok = crlf ? WriteFileAtomically(file, ToCrlf(text), WriteMode::Document, nullptr)
                         : WriteFileAtomically(file, text, WriteMode::Document, nullptr);
    if (!ok) {
        status = "Cannot save " + name;
        return false;
    }
    filePath = path;
    displayName = name;
    modified = false;
    status = "Saved " + name;
    return true;
}

DocumentStats Document::ComputeStats(const std::string& text) {
    // 16 bytes at a time (SSE2): a byte's lane is 0xFF when it separates words; a word starts at a
    // byte that does not, after one that does (or at the text's start). Per-lane counts subtract
    // the 0xFF lanes and are summed (_mm_sad_epu8) before a lane can wrap, every 255 blocks.
    DocumentStats stats;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size();
    size_t i = 0;
    const __m128i space = _mm_set1_epi8(' '), tab = _mm_set1_epi8('\t'), lf = _mm_set1_epi8('\n'),
                  cr = _mm_set1_epi8('\r'), zero = _mm_setzero_si128();
    __m128i before = _mm_set1_epi8(-1);   // the previous block's separators: the start counts as one
    __m128i lineSum = zero, wordSum = zero;
    while (n - i >= 16) {
        __m128i lines = zero, words = zero;
        for (int k = 0; k < 255 && n - i >= 16; k++, i += 16) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
            const __m128i newline = _mm_cmpeq_epi8(v, lf);
            const __m128i sep = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, space), _mm_cmpeq_epi8(v, tab)),
                                             _mm_or_si128(newline, _mm_cmpeq_epi8(v, cr)));
            const __m128i previous = _mm_or_si128(_mm_slli_si128(sep, 1), _mm_srli_si128(before, 15));
            lines = _mm_sub_epi8(lines, newline);
            words = _mm_sub_epi8(words, _mm_andnot_si128(sep, previous));
            before = sep;
        }
        lineSum = _mm_add_epi64(lineSum, _mm_sad_epu8(lines, zero));
        wordSum = _mm_add_epi64(wordSum, _mm_sad_epu8(words, zero));
    }
    const auto total = [](__m128i sum) {
        return static_cast<size_t>(_mm_cvtsi128_si64(sum)) +
               static_cast<size_t>(_mm_cvtsi128_si64(_mm_unpackhi_epi64(sum, sum)));
    };
    stats.lines += total(lineSum);
    stats.words += total(wordSum);
    bool inWord = i > 0 && (_mm_movemask_epi8(before) & 0x8000) == 0;
    for (; i < n; i++) {
        const unsigned char c = p[i];
        if (c == '\n') stats.lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            stats.words++;
        }
    }
    return stats;
}

} // namespace rgui
