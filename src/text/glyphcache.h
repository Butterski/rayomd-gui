// SPDX-License-Identifier: Apache-2.0
// Rasterized glyphs: 8-bit coverage bitmaps keyed by font, glyph, size and horizontal phase,
// evicted least recently used beyond a byte budget.
#pragma once

#include "text/glyphsource.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rgui::gfx {
struct WorkBudget;
}

namespace rgui::text {

struct GlyphKey {
    uint32_t source = 0;   // GlyphSource::SourceId()
    uint16_t glyph = 0;
    uint8_t phase = 0;     // horizontal pen phase in quarter pixels, 0..3
    uint32_t size64 = 0;   // size in 1/64 px (the em in device pixels)

    bool operator==(const GlyphKey& o) const {
        return source == o.source && glyph == o.glyph && phase == o.phase && size64 == o.size64;
    }
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const;
};

// A glyph's coverage: `width` x `height` bytes, row-major, top row first. Its top-left pixel
// sits at (left, top) relative to the pen position rounded down to whole pixels, on the
// baseline; top is usually negative (above the baseline). An empty glyph (a space) has width 0.
struct GlyphBitmap {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> alpha;
};

// Rasterized glyphs shared by threads that each keep a GlyphCache of their own (the preview's
// rendering threads): a miss in one thread's cache is rasterized once for all of them. Bitmaps
// never change once stored; beyond the byte budget the oldest stored go first. Thread-safe.
class GlyphStore {
public:
    static constexpr size_t kDefaultBudget = 32u << 20;

    explicit GlyphStore(size_t budgetBytes = kDefaultBudget) : budget_(budgetBytes) {}

    // Copies the stored bitmap of `key` into `out`; false (out untouched) when there is none.
    bool Find(const GlyphKey& key, GlyphBitmap& out) const;
    // Stores a copy of `bitmap` as `key`'s, unless one is stored already.
    void Put(const GlyphKey& key, const GlyphBitmap& bitmap);
    size_t Bytes() const;

private:
    using Shared = std::shared_ptr<const GlyphBitmap>;
    static size_t Cost(const GlyphBitmap& bitmap) { return bitmap.alpha.size() + sizeof(GlyphBitmap) + 64; }

    size_t budget_;
    mutable std::mutex mutex_;
    size_t bytes_ = 0;
    std::unordered_map<GlyphKey, Shared, GlyphKeyHash> map_;
    std::deque<GlyphKey> order_;  // oldest first
};

// Not internally synchronized. The usual pattern: one thread fills it (Get) while no other
// thread reads it; between fills any number of threads may Find at once. The UI owns one,
// filled on the UI thread in the compositor's prepare pass and read by tile threads; each
// preview thread owns its own, over the preview's GlyphStore.
class GlyphCache {
public:
    static constexpr size_t kDefaultBudget = 32u << 20;
    // Sizes are clamped to this many pixels per em.
    static constexpr float kMaxSize = 1024.0f;

    // With a `store`, a miss takes the store's bitmap when it has one, and a glyph rasterized
    // whole goes into the store too.
    explicit GlyphCache(size_t budgetBytes = kDefaultBudget, GlyphStore* store = nullptr)
        : budget_(budgetBytes), store_(store) {}

    // The key of `glyph` at `sizePx` pixels per em drawn with the pen at x = `penX` (only its
    // fraction counts: the quarter-pixel phase).
    static GlyphKey KeyFor(const GlyphSource& source, uint16_t glyph, float sizePx, float penX);

    // The bitmap of `key`, rasterized from `source` on a miss (with gfx::Rasterizer, the
    // outline through UnitsToEm, scaled to the size, y flipped and shifted by the phase). A
    // glyph whose outline cannot be read is cached empty. The pointer stays valid until a Get
    // after the next BeginFrame evicts it, or Clear.
    //
    // The cache never holds more than twice its budget: a miss that would pass that ceiling (the
    // current frame's entries cannot be evicted) is rasterized into a scratch bitmap of its own
    // and not kept, the pointer then valid until the next Get. `budget` (nullptr: unlimited) pays
    // for the rasterization (nothing for a bitmap from the store); a glyph left incomplete because
    // the budget stopped is not kept either.
    const GlyphBitmap* Get(const GlyphSource& source, const GlyphKey& key, gfx::WorkBudget* budget = nullptr);
    const GlyphBitmap* Get(const GlyphSource& source, uint16_t glyph, float sizePx, float penX) {
        return Get(source, KeyFor(source, glyph, sizePx, penX));
    }
    // The cached bitmap of `key` without filling or touching the LRU order; nullptr on a miss.
    const GlyphBitmap* Find(const GlyphKey& key) const;
    // Whether `bitmap`, which Get returned, is held by the cache (Find finds it, and it stays
    // valid until a Get after the next BeginFrame evicts it), not the scratch bitmap of a glyph
    // it did not keep.
    bool Keeps(const GlyphBitmap* bitmap) const { return bitmap != &scratch_; }

    // Starts a frame: entries used since the previous BeginFrame become evictable. Entries used
    // in the current frame are not evicted, so the cache may exceed its budget for one frame, up
    // to the ceiling of twice the budget.
    void BeginFrame() { frame_++; }
    void Clear();

    size_t Bytes() const { return bytes_; }
    size_t Count() const { return map_.size(); }
    size_t Budget() const { return budget_; }

private:
    struct Entry {
        GlyphKey key;
        GlyphBitmap bitmap;
        uint64_t frame = 0;
    };
    using List = std::list<Entry>;

    static size_t Cost(const GlyphBitmap& bitmap) { return bitmap.alpha.size() + sizeof(Entry) + 32; }
    void Evict();

    size_t budget_;
    GlyphStore* store_;
    GlyphBitmap scratch_;  // a glyph not kept (over the ceiling)
    size_t bytes_ = 0;
    uint64_t frame_ = 1;
    List lru_;   // most recent first
    std::unordered_map<GlyphKey, List::iterator, GlyphKeyHash> map_;
};

// Rasterizes one glyph (what GlyphCache::Get does on a miss), paying `budget` (nullptr: free):
// the bitmap's pixels, the segments and the swept pixels.
void RasterizeGlyph(const GlyphSource& source, uint16_t glyph, float sizePx, float phasePx, GlyphBitmap& out,
                    gfx::WorkBudget* budget = nullptr);

} // namespace rgui::text
