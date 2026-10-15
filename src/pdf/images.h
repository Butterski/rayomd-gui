// SPDX-License-Identifier: Apache-2.0
// Image XObjects of the preview (plan Phase 6 step 4): decoded to premultiplied pixels and kept
// in a cache shared by every preview build and thread.
//
// What decodes:
// - no filter, /FlateDecode (with /DecodeParms /Predictor >= 10, PNG rows undone with the
//   engine's UnfilterPngRow, or without a predictor), through the engine's ZlibReader with the
//   output size the image's parameters give;
// - /DCTDecode through our JPEG decoder (pdf/jpeg.h);
// - colour spaces DeviceGray, DeviceRGB (DeviceCMYK for DCT), and ICCBased, CalGray and CalRGB
//   read as the device space of their component count;
// - 1, 2, 4, 8 and 16 bits per component (16: the high byte; below 8: scaled to 0..255);
// - an /SMask decoded the same way as gray, used as alpha (resampled to the image when its size
//   differs), then premultiplied.
// Anything else fails with a reason ("filter:<name>", "colorspace:<name>", "image-mask",
// "flate", "predictor", "jpeg-...", "size", ...), which the renderer reports as image:<reason>.
// Every read is bounded: at most kMaxImagePixels pixels, outputs of exactly the expected size.
#pragma once

#include "gfx/surface.h"
#include "pdf/reader.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rgui::pdf {

// Images with more pixels than this fail ("size").
inline constexpr uint64_t kMaxImagePixels = 1ull << 25;

// Decodes the image XObject `stream` (resolved) of `file`. False, with `reason`, when it cannot.
bool DecodeImage(const PdfFile& file, const PdfValue& stream, gfx::Image& out, std::string* reason);

// Decoded images keyed by PdfFile::ValueHash of the image's entry (its dictionary, data and
// soft mask, whatever the object numbers), least recently used evicted beyond a byte budget.
// Safe to use from any number of threads at once; images handed out stay valid as long as their
// holders keep them, evicted or not.
class ImageCache {
public:
    static constexpr size_t kDefaultBudget = 128u << 20;

    explicit ImageCache(size_t budgetBytes = kDefaultBudget) : budget_(budgetBytes) {}
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // The image of `entry` (an image XObject or a reference to one) decoded, from the cache or
    // decoded now and kept. Nullptr, with `reason`, when it does not decode; failures are not
    // kept.
    std::shared_ptr<const gfx::Image> Load(const PdfFile& file, const PdfValue& entry, std::string* reason);

    std::shared_ptr<const gfx::Image> Find(uint64_t key);
    void Insert(uint64_t key, std::shared_ptr<const gfx::Image> image);

    size_t Bytes() const;
    size_t Count() const;
    void Clear();

private:
    struct Entry {
        uint64_t key = 0;
        std::shared_ptr<const gfx::Image> image;
        size_t bytes = 0;
    };
    using List = std::list<Entry>;

    mutable std::mutex mutex_;
    size_t budget_;
    size_t bytes_ = 0;
    List lru_;  // most recent first
    std::unordered_map<uint64_t, List::iterator> map_;
};

// The process's image cache, which preview builds share.
ImageCache& SharedImageCache();

} // namespace rgui::pdf
