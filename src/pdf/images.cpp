// SPDX-License-Identifier: Apache-2.0
#include "pdf/images.h"

#include "core/inflate.h"
#include "core/png_filter.h"
#include "pdf/jpeg.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rgui::pdf {
namespace {

void SetReason(std::string* reason, std::string text) {
    if (reason) *reason = std::move(text);
}

enum class Filter : uint8_t { None, Flate, Dct };

// The stream's single filter; arrays of one filter are accepted (with their one /DecodeParms).
bool ReadFilter(const PdfFile& file, const PdfRawStream& raw, Filter& filter, const PdfValue*& parms,
                std::string* reason) {
    filter = Filter::None;
    parms = raw.decodeParms;
    const PdfValue* name = raw.filter;
    if (name && name->IsArray()) {
        if (name->items.size() > 1) {
            SetReason(reason, "filter:chain");
            return false;
        }
        name = name->items.empty() ? nullptr : &file.Resolve(name->items[0]);
        if (parms && parms->IsArray()) parms = parms->items.empty() ? nullptr : &file.Resolve(parms->items[0]);
    }
    if (!name || name->IsNull()) return true;
    if (name->IsName("FlateDecode") || name->IsName("Fl")) {
        filter = Filter::Flate;
        return true;
    }
    if (name->IsName("DCTDecode") || name->IsName("DCT")) {
        filter = Filter::Dct;
        return true;
    }
    SetReason(reason, "filter:" + std::string(name->IsName() ? name->Name() : std::string_view("?")));
    return false;
}

// The component count of a colour space, 0 (with `reason`) when it is not one we read.
int ColorComponents(const PdfFile& file, const PdfValue& space, std::string* reason) {
    const PdfValue& cs = file.Resolve(space);
    std::string_view family = cs.Name();
    if (cs.IsArray() && !cs.items.empty()) family = file.Resolve(cs.items[0]).Name();
    if (family == "DeviceGray" || family == "G" || family == "CalGray") return 1;
    if (family == "DeviceRGB" || family == "RGB" || family == "CalRGB") return 3;
    if (family == "DeviceCMYK" || family == "CMYK") return 4;
    if (family == "ICCBased" && cs.items.size() >= 2) {
        const int64_t n = file.Get(cs.items[1], "N").Int(0);
        if (n == 1 || n == 3 || n == 4) return static_cast<int>(n);
    }
    SetReason(reason, "colorspace:" + std::string(family.empty() ? std::string_view("?") : family));
    return 0;
}

bool ReadSize(const PdfFile& file, const PdfValue& dict, int& width, int& height, std::string* reason) {
    const int64_t w = file.Get(dict, "Width").Int(0);
    const int64_t h = file.Get(dict, "Height").Int(0);
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxImagePixels) {
        SetReason(reason, "size");
        return false;
    }
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

// Unpacks one row of `width * comps` samples of `bpc` bits into bytes.
void UnpackRow(const uint8_t* row, size_t samples, int bpc, uint8_t* out) {
    if (bpc == 8) {
        std::memcpy(out, row, samples);
        return;
    }
    if (bpc == 16) {
        for (size_t i = 0; i < samples; i++) out[i] = row[2 * i];
        return;
    }
    const unsigned max = (1u << bpc) - 1;
    for (size_t i = 0; i < samples; i++) {
        const size_t bit = i * static_cast<size_t>(bpc);
        const unsigned v = (row[bit >> 3] >> (8 - bpc - (bit & 7))) & max;
        out[i] = static_cast<uint8_t>(v * 255 / max);
    }
}

// The 8-bit samples of a stream that is not DCT: width * height * comps bytes.
bool DecodeSamples(const PdfFile& file, const PdfRawStream& raw, Filter filter,
                   const PdfValue* parms, int width, int height, int comps, int bpc, std::vector<uint8_t>& out,
                   std::string* reason) {
    const size_t samplesPerRow = static_cast<size_t>(width) * static_cast<size_t>(comps);
    const size_t rowBytes = (samplesPerRow * static_cast<size_t>(bpc) + 7) / 8;
    out.resize(samplesPerRow * static_cast<size_t>(height));
    if (filter == Filter::None) {
        if (raw.data.size() < rowBytes * static_cast<size_t>(height)) {
            SetReason(reason, "truncated");
            return false;
        }
        for (int y = 0; y < height; y++) {
            UnpackRow(reinterpret_cast<const uint8_t*>(raw.data.data()) + rowBytes * static_cast<size_t>(y),
                      samplesPerRow, bpc, out.data() + samplesPerRow * static_cast<size_t>(y));
        }
        return true;
    }
    int64_t predictor = 1;
    if (parms) predictor = file.Get(*parms, "Predictor").Int(1);
    const bool png = predictor >= 10;
    if (predictor != 1 && !png) {
        SetReason(reason, "predictor");
        return false;
    }
    if (png) {
        const int64_t colors = file.Get(*parms, "Colors").Int(1);
        const int64_t pbpc = file.Get(*parms, "BitsPerComponent").Int(8);
        const int64_t columns = file.Get(*parms, "Columns").Int(1);
        if (colors != comps || pbpc != bpc || columns != width) {
            SetReason(reason, "predictor");
            return false;
        }
    }
    const size_t stored = rowBytes + (png ? 1 : 0);
    RayoMd::Inflate::ZlibReader reader;
    if (!reader.Start(reinterpret_cast<const uint8_t*>(raw.data.data()), raw.data.size(),
                      stored * static_cast<size_t>(height), stored)) {
        SetReason(reason, "flate");
        return false;
    }
    std::vector<uint8_t> rows(png ? 2 * rowBytes : 0, 0);
    uint8_t* up = rows.data();
    uint8_t* cur = rows.data() + rowBytes;
    const size_t bpp = std::max<size_t>(1, static_cast<size_t>(comps * bpc) / 8);
    for (int y = 0; y < height; y++) {
        const uint8_t* data = reader.Take(stored);
        if (!data) {
            SetReason(reason, "flate");
            return false;
        }
        const uint8_t* row = data;
        if (png) {
            if (!TinyPdf::Internal::UnfilterPngRow(data[0], data + 1, up, rowBytes, bpp, cur)) {
                SetReason(reason, "predictor");
                return false;
            }
            row = cur;
            std::swap(up, cur);
        }
        UnpackRow(row, samplesPerRow, bpc, out.data() + samplesPerRow * static_cast<size_t>(y));
    }
    if (!reader.Finish()) {
        SetReason(reason, "flate");
        return false;
    }
    return true;
}

bool ReadBpc(const PdfFile& file, const PdfValue& dict, int& bpc, std::string* reason) {
    const int64_t v = file.Get(dict, "BitsPerComponent").Int(8);
    if (v != 1 && v != 2 && v != 4 && v != 8 && v != 16) {
        SetReason(reason, "bpc");
        return false;
    }
    bpc = static_cast<int>(v);
    return true;
}

// The soft mask `mask` (resolved) as width * height alpha bytes, resampled to the image.
bool DecodeMask(const PdfFile& file, const PdfValue& mask, int width, int height, std::vector<uint8_t>& alpha,
                std::string* reason) {
    PdfRawStream raw;
    if (!file.RawStream(mask, raw)) {
        SetReason(reason, "smask");
        return false;
    }
    Filter filter;
    const PdfValue* parms = nullptr;
    if (!ReadFilter(file, raw, filter, parms, reason)) return false;
    int mw = 0, mh = 0;
    std::vector<uint8_t> gray;
    if (filter == Filter::Dct) {
        if (!DecodeJpegGray(reinterpret_cast<const uint8_t*>(raw.data.data()), raw.data.size(), mw, mh, gray, reason)) {
            return false;
        }
    } else {
        int bpc = 8;
        if (!ReadSize(file, mask, mw, mh, reason) || !ReadBpc(file, mask, bpc, reason)) return false;
        const PdfValue& cs = file.Get(mask, "ColorSpace");
        if (!cs.IsNull() && ColorComponents(file, cs, reason) != 1) {
            SetReason(reason, "smask-colorspace");
            return false;
        }
        if (!DecodeSamples(file, raw, filter, parms, mw, mh, 1, bpc, gray, reason)) return false;
    }
    if (mw == width && mh == height) {
        alpha = std::move(gray);
        return true;
    }
    alpha.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int y = 0; y < height; y++) {
        const size_t sy = static_cast<size_t>((static_cast<int64_t>(y) * mh) / height);
        for (int x = 0; x < width; x++) {
            const size_t sx = static_cast<size_t>((static_cast<int64_t>(x) * mw) / width);
            alpha[static_cast<size_t>(y) * width + x] = gray[sy * mw + sx];
        }
    }
    return true;
}

} // namespace

bool DecodeImage(const PdfFile& file, const PdfValue& stream, gfx::Image& out, std::string* reason) {
    const PdfValue& dict = file.Resolve(stream);
    PdfRawStream raw;
    if (!file.RawStream(dict, raw)) {
        SetReason(reason, "stream");
        return false;
    }
    if (file.Get(dict, "ImageMask").Bool(false)) {
        SetReason(reason, "image-mask");
        return false;
    }
    Filter filter;
    const PdfValue* parms = nullptr;
    if (!ReadFilter(file, raw, filter, parms, reason)) return false;
    gfx::Image image;
    if (filter == Filter::Dct) {
        JpegInfo info;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.data.data());
        if (!ReadJpegInfo(data, raw.data.size(), info, reason)) return false;
        if (static_cast<uint64_t>(info.width) * static_cast<uint64_t>(info.height) > kMaxImagePixels) {
            SetReason(reason, "size");
            return false;
        }
        if (!DecodeJpeg(data, raw.data.size(), image, reason)) return false;
    } else {
        int width = 0, height = 0, bpc = 8;
        if (!ReadSize(file, dict, width, height, reason) || !ReadBpc(file, dict, bpc, reason)) return false;
        const int comps = ColorComponents(file, file.Get(dict, "ColorSpace"), reason);
        if (comps == 0) return false;
        std::vector<uint8_t> samples;
        if (!DecodeSamples(file, raw, filter, parms, width, height, comps, bpc, samples, reason)) return false;
        image.width = width;
        image.height = height;
        image.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        const uint8_t* s = samples.data();
        for (uint32_t& p : image.pixels) {
            if (comps == 1) {
                p = gfx::PackArgb(255, s[0], s[0], s[0]);
            } else if (comps == 3) {
                p = gfx::PackArgb(255, s[0], s[1], s[2]);
            } else {
                const uint32_t k = 255u - s[3];
                p = gfx::PackArgb(255, static_cast<uint8_t>(gfx::Mul255(255u - s[0], k)),
                                  static_cast<uint8_t>(gfx::Mul255(255u - s[1], k)),
                                  static_cast<uint8_t>(gfx::Mul255(255u - s[2], k)));
            }
            s += comps;
        }
        image.opaque = true;
    }
    const PdfValue& mask = file.Get(dict, "SMask");
    if (mask.IsStream()) {
        std::vector<uint8_t> alpha;
        if (!DecodeMask(file, mask, image.width, image.height, alpha, reason)) return false;
        bool opaque = true;
        for (size_t i = 0; i < image.pixels.size(); i++) {
            const uint32_t p = image.pixels[i];
            const uint8_t a = alpha[i];
            opaque = opaque && a == 255;
            image.pixels[i] = gfx::Premultiply(static_cast<uint8_t>(p >> 16), static_cast<uint8_t>(p >> 8),
                                               static_cast<uint8_t>(p), a);
        }
        image.opaque = opaque;
    } else if (!mask.IsNull() && !mask.IsName("None")) {
        SetReason(reason, "smask");
        return false;
    }
    out = std::move(image);
    return true;
}

std::shared_ptr<const gfx::Image> ImageCache::Load(const PdfFile& file, const PdfValue& entry, std::string* reason) {
    const uint64_t key = file.ValueHash(entry);
    if (auto found = Find(key)) return found;
    auto image = std::make_shared<gfx::Image>();
    if (!DecodeImage(file, entry, *image, reason)) return nullptr;
    image->id = key;
    Insert(key, image);
    return image;
}

std::shared_ptr<const gfx::Image> ImageCache::Find(uint64_t key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = map_.find(key);
    if (found == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, found->second);
    return found->second->image;
}

void ImageCache::Insert(uint64_t key, std::shared_ptr<const gfx::Image> image) {
    if (!image) return;
    const size_t bytes = image->pixels.size() * sizeof(uint32_t) + sizeof(gfx::Image) + 64;
    if (bytes > budget_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (map_.count(key)) return;
    lru_.push_front(Entry{key, std::move(image), bytes});
    map_.emplace(key, lru_.begin());
    bytes_ += bytes;
    while (bytes_ > budget_ && !lru_.empty()) {
        bytes_ -= lru_.back().bytes;
        map_.erase(lru_.back().key);
        lru_.pop_back();
    }
}

size_t ImageCache::Bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

size_t ImageCache::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

void ImageCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    map_.clear();
    lru_.clear();
    bytes_ = 0;
}

ImageCache& SharedImageCache() {
    static ImageCache cache;
    return cache;
}

} // namespace rgui::pdf
