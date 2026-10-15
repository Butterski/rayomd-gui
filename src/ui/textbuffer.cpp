// SPDX-License-Identifier: Apache-2.0
#include "ui/textbuffer.h"

#include "common/text_utils.h"

#include <algorithm>
#include <cstring>

namespace rgui::ui {

namespace {

constexpr size_t kMinGap = 4096;

// The length of the well-formed UTF-8 sequence at p[0] (at most `avail` bytes), 0 if none.
size_t SequenceLength(const unsigned char* p, size_t avail, uint32_t& cp) {
    if (p[0] < 0x80) {
        cp = p[0];
        return 1;
    }
    size_t len = 0;
    if (!RayoMd::Text::DecodeUtf8(std::string_view(reinterpret_cast<const char*>(p), avail), 0, cp, len)) return 0;
    return len;
}

} // namespace

void TextBuffer::SetText(std::string text) {
    const size_t size = text.size();
    // Exact sizes: a document of N bytes and L lines holds N + kMinGap bytes and L starts, not
    // the doubled capacity of growing vectors.
    std::vector<char> data(size + kMinGap);
    if (size > 0) std::memcpy(data.data(), text.data(), size);
    std::string().swap(text);
    data_.swap(data);
    std::vector<char>().swap(data);
    gapStart_ = size;
    gapEnd_ = data_.size();
    const char* p = data_.data();
    const size_t lines = 1 + static_cast<size_t>(std::count(p, p + size, '\n'));
    std::vector<uint32_t> starts;
    starts.reserve(lines);
    starts.push_back(0);
    for (const char* q = p; (q = static_cast<const char*>(std::memchr(q, '\n', size - (q - p)))) != nullptr; q++) {
        starts.push_back(static_cast<uint32_t>(q - p + 1));
    }
    lineStarts_.swap(starts);
    shiftFrom_ = lineStarts_.size();
    shift_ = 0;
}

void TextBuffer::Reserve(size_t extra) {
    if (gapEnd_ - gapStart_ >= extra) return;
    const size_t size = Size();
    const size_t after = data_.size() - gapEnd_;
    const size_t capacity = std::max(size + extra + kMinGap, data_.size() * 2);
    std::vector<char> grown(capacity);
    if (gapStart_ > 0) std::memcpy(grown.data(), data_.data(), gapStart_);
    if (after > 0) std::memcpy(grown.data() + capacity - after, data_.data() + gapEnd_, after);
    data_.swap(grown);
    gapEnd_ = capacity - after;
}

void TextBuffer::MoveGap(size_t pos) {
    if (pos < gapStart_) {
        const size_t n = gapStart_ - pos;
        std::memmove(data_.data() + gapEnd_ - n, data_.data() + pos, n);
        gapStart_ -= n;
        gapEnd_ -= n;
    } else if (pos > gapStart_) {
        const size_t n = pos - gapStart_;
        std::memmove(data_.data() + gapStart_, data_.data() + gapEnd_, n);
        gapStart_ += n;
        gapEnd_ += n;
    }
}

void TextBuffer::MoveShift(size_t line) {
    if (shift_ != 0) {
        for (size_t i = shiftFrom_; i < line; i++) lineStarts_[i] += shift_;
        for (size_t i = line; i < shiftFrom_; i++) lineStarts_[i] -= shift_;
    }
    shiftFrom_ = line;
}

size_t TextBuffer::FirstLineAfter(size_t pos) const {
    size_t lo = 0, hi = lineStarts_.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (StartOf(mid) <= pos) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void TextBuffer::Insert(size_t pos, std::string_view bytes) {
    if (bytes.empty()) return;
    pos = std::min(pos, Size());
    Reserve(bytes.size());
    MoveGap(pos);
    std::memcpy(data_.data() + gapStart_, bytes.data(), bytes.size());
    gapStart_ += bytes.size();

    // The starts after the line of `pos` move by the inserted size; the new ones go after it,
    // stored without the shift they are read with.
    const size_t line = LineOf(pos);
    MoveShift(line + 1);
    shift_ += static_cast<uint32_t>(bytes.size());
    std::vector<uint32_t> added;
    for (size_t i = 0; i < bytes.size(); i++) {
        if (bytes[i] == '\n') added.push_back(static_cast<uint32_t>(pos + i + 1) - shift_);
    }
    if (!added.empty()) lineStarts_.insert(lineStarts_.begin() + static_cast<long>(line + 1), added.begin(), added.end());
}

void TextBuffer::Erase(size_t pos, size_t len) {
    const size_t size = Size();
    if (pos >= size) return;
    len = std::min(len, size - pos);
    if (len == 0) return;
    MoveGap(pos);
    gapEnd_ += len;

    // Line starts in (pos, pos + len] were inside the erased bytes; the ones after move back.
    const size_t first = FirstLineAfter(pos);
    const size_t last = FirstLineAfter(pos + len);
    MoveShift(first);
    lineStarts_.erase(lineStarts_.begin() + static_cast<long>(first), lineStarts_.begin() + static_cast<long>(last));
    shift_ -= static_cast<uint32_t>(len);
}

std::string TextBuffer::Copy(size_t pos, size_t len) const {
    std::string out;
    const size_t size = Size();
    if (pos >= size) return out;
    len = std::min(len, size - pos);
    out.resize(len);
    const size_t end = pos + len;
    const size_t a = std::min(end, gapStart_);
    if (pos < a) std::memcpy(&out[0], data_.data() + pos, a - pos);
    const size_t b = std::max(pos, gapStart_);
    if (b < end) std::memcpy(&out[b - pos], data_.data() + gapEnd_ + (b - gapStart_), end - b);
    return out;
}

std::string_view TextBuffer::Range(size_t pos, size_t len, std::string& scratch) const {
    const size_t size = Size();
    if (pos >= size) return {};
    len = std::min(len, size - pos);
    if (pos + len <= gapStart_) return std::string_view(data_.data() + pos, len);
    if (pos >= gapStart_) return std::string_view(data_.data() + gapEnd_ + (pos - gapStart_), len);
    scratch = Copy(pos, len);
    return scratch;
}

void TextBuffer::Pieces(size_t pos, size_t len, std::string_view& head, std::string_view& tail) const {
    head = tail = {};
    const size_t size = Size();
    if (pos >= size) return;
    len = std::min(len, size - pos);
    const size_t end = pos + len;
    if (end <= gapStart_) {
        head = std::string_view(data_.data() + pos, len);
    } else if (pos >= gapStart_) {
        head = std::string_view(data_.data() + gapEnd_ + (pos - gapStart_), len);
    } else {
        head = std::string_view(data_.data() + pos, gapStart_ - pos);
        tail = std::string_view(data_.data() + gapEnd_, end - gapStart_);
    }
}

std::string_view TextBuffer::View() {
    MoveGap(Size());
    return std::string_view(data_.data(), gapStart_);
}

size_t TextBuffer::LineOf(size_t pos) const {
    return FirstLineAfter(pos) - 1;
}

uint32_t TextBuffer::Decode(size_t pos, size_t& len) const {
    unsigned char bytes[4];
    const size_t avail = std::min<size_t>(4, Size() - pos);
    for (size_t i = 0; i < avail; i++) bytes[i] = static_cast<unsigned char>(At(pos + i));
    uint32_t cp = 0;
    len = SequenceLength(bytes, avail, cp);
    if (len == 0) {
        len = 1;
        return 0xFFFD;
    }
    return cp;
}

size_t TextBuffer::Prev(size_t pos) const {
    if (pos == 0) return 0;
    // Step back over up to three continuation bytes, then check the sequence ends at pos.
    for (size_t back = 1; back <= 4 && back <= pos; back++) {
        const unsigned char c = static_cast<unsigned char>(At(pos - back));
        if ((c & 0xC0) != 0x80) {
            size_t len;
            Decode(pos - back, len);
            return len == back ? pos - back : pos - 1;
        }
    }
    return pos - 1;
}

size_t TextBuffer::Next(size_t pos) const {
    if (pos >= Size()) return Size();
    size_t len;
    Decode(pos, len);
    return pos + len;
}

std::string NormalizeEditorText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        // Runs of ASCII other than CR are copied as they are.
        size_t run = i;
        while (run < text.size() && p[run] < 0x80 && p[run] != '\r') run++;
        if (run > i) {
            out.append(text.data() + i, run - i);
            i = run;
            continue;
        }
        const unsigned char c = p[i];
        if (c == '\r') {
            out.push_back('\n');
            i += (i + 1 < text.size() && p[i + 1] == '\n') ? 2 : 1;
            continue;
        }
        uint32_t cp;
        const size_t len = SequenceLength(p + i, text.size() - i, cp);
        if (len == 0) {
            out.append("\xEF\xBF\xBD");
            i++;
        } else {
            out.append(text.data() + i, len);
            i += len;
        }
    }
    return out;
}

} // namespace rgui::ui
