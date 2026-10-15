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
// "flate", "predictor", "jpeg-...", "size", "budget", ...), which the renderer reports as
// image:<reason>. Every read is bounded: at most kMaxImagePixels pixels (the soft mask's too),
// outputs of exactly the expected size.
//
// Levels: an image drawn small is kept small. Level l is the image reduced by 2^l per axis
// (ceil(width / 2^l) by ceil(height / 2^l), each pixel the mean of the premultiplied pixels of
// its square); a JPEG without a soft mask is decoded reduced (DecodeJpeg's scaleShift, up to 3),
// anything else is decoded whole and reduced. The renderer draws the coarsest level that still
// has at least one image pixel per device pixel (ImageLevel), sampling far fewer pixels and
// keeping far fewer bytes than the image has.
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
// The coarsest level: 1/64 per axis.
inline constexpr int kMaxImageLevel = 6;

// The level to draw a `width` x `height` image whose pixels' columns and rows span `deviceWidth`
// and `deviceHeight` device pixels: the largest l <= kMaxImageLevel with ceil(width / 2^l) >=
// deviceWidth and ceil(height / 2^l) >= deviceHeight (0 when none, or for sizes not finite).
int ImageLevel(int width, int height, double deviceWidth, double deviceHeight);

// Reduces `image` by 2^shift per axis (shift 0: a copy): each pixel the rounded mean of its
// square's pixels within the image.
gfx::Image ReduceImage(const gfx::Image& image, int shift);

// What decoding an image costs, in bytes of full-size samples: 4 per pixel, and 1 per pixel of
// its soft mask. What DecodeImage charges `budget`; 0 when the size cannot be read (decoding then
// fails before it spends anything).
uint64_t ImageDecodeCost(const PdfFile& file, const PdfValue& stream);

// Decodes the image XObject `stream` (resolved) of `file` at `level`. False, with `reason`, when
// it cannot. `budget` (nullptr: unlimited) holds the decode cost this may still spend
// (ImageDecodeCost): when the image costs more, it fails with "budget" and spends nothing, else
// the cost is taken from it.
bool DecodeImage(const PdfFile& file, const PdfValue& stream, gfx::Image& out, std::string* reason, int level = 0,
                 uint64_t* budget = nullptr);

// Decoded images keyed by PdfFile::ValueHash of the image's entry (its dictionary, data and
// soft mask, whatever the object numbers) and the level, least recently used evicted beyond a
// byte budget; an image bigger than the budget is handed out but not kept. Safe to use from any
// number of threads at once; images handed out stay valid as long as their holders keep them,
// evicted or not.
class ImageCache {
public:
    static constexpr size_t kDefaultBudget = 128u << 20;

    explicit ImageCache(size_t budgetBytes = kDefaultBudget) : budget_(budgetBytes) {}
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // The image of `entry` (an image XObject or a reference to one) at `level`: from the cache,
    // reduced from a finer level in the cache, or decoded now (DecodeImage with `budget`); kept
    // when it fits. Nullptr, with `reason`, when it does not decode; failures are not kept.
    // `kept` (may be nullptr) tells whether the cache holds the image handed out.
    std::shared_ptr<const gfx::Image> Load(const PdfFile& file, const PdfValue& entry, std::string* reason,
                                           int level = 0, uint64_t* budget = nullptr, bool* kept = nullptr);

    std::shared_ptr<const gfx::Image> Find(const PdfHash& key, int level);
    // False when the image is bigger than the budget (not kept).
    bool Insert(const PdfHash& key, int level, std::shared_ptr<const gfx::Image> image);

    size_t Bytes() const;
    size_t Count() const;
    void Clear();

private:
    struct Key {
        PdfHash hash;
        int level = 0;
        bool operator==(const Key& o) const { return hash == o.hash && level == o.level; }
    };
    struct KeyHasher {
        size_t operator()(const Key& k) const {
            return PdfHashHasher()(k.hash) ^ static_cast<size_t>(static_cast<uint64_t>(k.level) * 0x9E3779B97F4A7C15ull);
        }
    };
    struct Entry {
        Key key;
        std::shared_ptr<const gfx::Image> image;
        size_t bytes = 0;
    };
    using List = std::list<Entry>;

    mutable std::mutex mutex_;
    size_t budget_;
    size_t bytes_ = 0;
    List lru_;  // most recent first
    std::unordered_map<Key, List::iterator, KeyHasher> map_;
};

// The process's image cache, which preview builds share.
ImageCache& SharedImageCache();

} // namespace rgui::pdf
