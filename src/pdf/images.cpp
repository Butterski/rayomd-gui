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
        if (name->Array().size() > 1) {
            SetReason(reason, "filter:chain");
            return false;
        }
        name = name->Array().empty() ? nullptr : &file.Resolve(name->Array()[0]);
        if (parms && parms->IsArray()) parms = parms->Array().empty() ? nullptr : &file.Resolve(parms->Array()[0]);
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
    if (cs.IsArray() && !cs.Array().empty()) family = file.Resolve(cs.Array()[0]).Name();
    if (family == "DeviceGray" || family == "G" || family == "CalGray") return 1;
    if (family == "DeviceRGB" || family == "RGB" || family == "CalRGB") return 3;
    if (family == "DeviceCMYK" || family == "CMYK") return 4;
    if (family == "ICCBased" && cs.Array().size() >= 2) {
        const int64_t n = file.Get(cs.Array()[1], "N").Int(0);
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
        // The pixel limit holds for masks too: checked on the header before the decoder sizes
        // its planes.
        const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.data.data());
        JpegInfo info;
        if (!ReadJpegInfo(data, raw.data.size(), info, reason)) return false;
        if (static_cast<uint64_t>(info.width) * static_cast<uint64_t>(info.height) > kMaxImagePixels) {
            SetReason(reason, "size");
            return false;
        }
        if (!DecodeJpegGray(data, raw.data.size(), mw, mh, gray, reason)) return false;
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

// Width times height of the image or mask `dict` (resolved), from its JPEG header for DCT; 0
// when it cannot be read.
uint64_t PixelCount(const PdfFile& file, const PdfValue& dict) {
    PdfRawStream raw;
    if (!file.RawStream(dict, raw)) return 0;
    Filter filter;
    const PdfValue* parms = nullptr;
    if (!ReadFilter(file, raw, filter, parms, nullptr)) return 0;
    if (filter == Filter::Dct) {
        JpegInfo info;
        if (!ReadJpegInfo(reinterpret_cast<const uint8_t*>(raw.data.data()), raw.data.size(), info, nullptr)) return 0;
        return static_cast<uint64_t>(info.width) * static_cast<uint64_t>(info.height);
    }
    const int64_t w = file.Get(dict, "Width").Int(0);
    const int64_t h = file.Get(dict, "Height").Int(0);
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535) return 0;
    return static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
}

} // namespace

int ImageLevel(int width, int height, double deviceWidth, double deviceHeight) {
    if (width <= 0 || height <= 0 || !(deviceWidth >= 0 && deviceWidth < 1e9) ||
        !(deviceHeight >= 0 && deviceHeight < 1e9)) {
        return 0;
    }
    int level = 0;
    while (level < kMaxImageLevel) {
        const int next = level + 1;
        const int64_t w = (static_cast<int64_t>(width) + (int64_t(1) << next) - 1) >> next;
        const int64_t h = (static_cast<int64_t>(height) + (int64_t(1) << next) - 1) >> next;
        if (static_cast<double>(w) < deviceWidth || static_cast<double>(h) < deviceHeight) break;
        level = next;
    }
    return level;
}

gfx::Image ReduceImage(const gfx::Image& image, int shift) {
    if (shift <= 0 || image.width <= 0 || image.height <= 0) return image;
    const int f = 1 << shift;
    gfx::Image out;
    out.width = (image.width + f - 1) >> shift;
    out.height = (image.height + f - 1) >> shift;
    out.opaque = image.opaque;
    out.pixels.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height));
    // Channel sums of each output column over the rows of one output row, at most 64 x 64 x 255.
    std::vector<uint32_t> sums(static_cast<size_t>(out.width) * 4);
    const size_t iw = static_cast<size_t>(image.width);
    for (int oy = 0; oy < out.height; oy++) {
        std::fill(sums.begin(), sums.end(), 0u);
        const int y0 = oy << shift;
        const int y1 = std::min(image.height, y0 + f);
        for (int y = y0; y < y1; y++) {
            const uint32_t* row = image.pixels.data() + static_cast<size_t>(y) * iw;
            for (size_t x = 0; x < iw; x++) {
                const uint32_t p = row[x];
                uint32_t* s = &sums[(x >> shift) * 4];
                s[0] += p & 0xFF;
                s[1] += (p >> 8) & 0xFF;
                s[2] += (p >> 16) & 0xFF;
                s[3] += p >> 24;
            }
        }
        uint32_t* dst = out.pixels.data() + static_cast<size_t>(oy) * static_cast<size_t>(out.width);
        for (int ox = 0; ox < out.width; ox++) {
            const int x0 = ox << shift;
            const uint32_t count = static_cast<uint32_t>((std::min(image.width, x0 + f) - x0) * (y1 - y0));
            const uint32_t* s = &sums[static_cast<size_t>(ox) * 4];
            uint32_t p = 0;
            for (int c = 0; c < 4; c++) p |= ((s[c] + count / 2) / count) << (8 * c);
            dst[ox] = p;
        }
    }
    return out;
}

uint64_t ImageDecodeCost(const PdfFile& file, const PdfValue& stream) {
    const PdfValue& dict = file.Resolve(stream);
    const uint64_t pixels = PixelCount(file, dict);
    if (pixels == 0) return 0;
    uint64_t cost = 4 * pixels;
    const PdfValue& mask = file.Get(dict, "SMask");
    if (mask.IsStream()) cost += PixelCount(file, mask);
    return cost;
}

bool DecodeImage(const PdfFile& file, const PdfValue& stream, gfx::Image& out, std::string* reason, int level,
                 uint64_t* budget) {
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
    level = std::clamp(level, 0, kMaxImageLevel);
    if (budget) {
        const uint64_t cost = ImageDecodeCost(file, dict);
        if (cost > *budget) {
            SetReason(reason, "budget");
            return false;
        }
        *budget -= cost;
    }
    const PdfValue& mask = file.Get(dict, "SMask");
    gfx::Image image;
    int reduced = 0;  // the levels the decoder already applied
    if (filter == Filter::Dct) {
        JpegInfo info;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.data.data());
        if (!ReadJpegInfo(data, raw.data.size(), info, reason)) return false;
        if (static_cast<uint64_t>(info.width) * static_cast<uint64_t>(info.height) > kMaxImagePixels) {
            SetReason(reason, "size");
            return false;
        }
        // With a soft mask the image is premultiplied at full size first, then reduced.
        if (!mask.IsStream()) reduced = std::min(level, 3);
        if (!DecodeJpeg(data, raw.data.size(), image, reason, reduced)) return false;
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
    out = level > reduced ? ReduceImage(image, level - reduced) : std::move(image);
    return true;
}

std::shared_ptr<const gfx::Image> ImageCache::Load(const PdfFile& file, const PdfValue& entry, std::string* reason,
                                                   int level, uint64_t* budget, bool* kept) {
    level = std::clamp(level, 0, kMaxImageLevel);
    const PdfHash key = file.ValueHash(entry);
    if (auto found = Find(key, level)) {
        if (kept) *kept = true;
        return found;
    }
    std::shared_ptr<gfx::Image> image;
    // A finer level kept: reduced from it, no decoding.
    for (int finer = level - 1; finer >= 0 && !image; finer--) {
        if (auto source = Find(key, finer)) image = std::make_shared<gfx::Image>(ReduceImage(*source, level - finer));
    }
    if (!image) {
        image = std::make_shared<gfx::Image>();
        if (!DecodeImage(file, entry, *image, reason, level, budget)) return nullptr;
    }
    image->id = key.lo ^ (static_cast<uint64_t>(level) << 56);
    const bool inserted = Insert(key, level, image);
    if (kept) *kept = inserted;
    return image;
}

std::shared_ptr<const gfx::Image> ImageCache::Find(const PdfHash& key, int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = map_.find(Key{key, level});
    if (found == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, found->second);
    return found->second->image;
}

bool ImageCache::Insert(const PdfHash& key, int level, std::shared_ptr<const gfx::Image> image) {
    if (!image) return false;
    const size_t bytes = image->pixels.size() * sizeof(uint32_t) + sizeof(gfx::Image) + 64;
    if (bytes > budget_) return false;
    const Key k{key, level};
    std::lock_guard<std::mutex> lock(mutex_);
    if (map_.count(k)) return false;  // another thread was first; the caller's copy is not the kept one
    lru_.push_front(Entry{k, std::move(image), bytes});
    map_.emplace(k, lru_.begin());
    bytes_ += bytes;
    while (bytes_ > budget_ && lru_.size() > 1) {
        bytes_ -= lru_.back().bytes;
        map_.erase(lru_.back().key);
        lru_.pop_back();
    }
    return true;
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
