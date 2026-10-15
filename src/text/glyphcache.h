// SPDX-License-Identifier: Apache-2.0
// Rasterized glyphs: 8-bit coverage bitmaps keyed by font, glyph, size and horizontal phase,
// evicted least recently used beyond a byte budget.
#pragma once

#include "text/glyphsource.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

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

// Not internally synchronized. The usual pattern: one thread fills it (Get) while no other
// thread reads it; between fills any number of threads may Find at once. The UI owns one,
// filled on the UI thread in the compositor's prepare pass and read by tile threads; each
// preview thread owns its own.
class GlyphCache {
public:
    static constexpr size_t kDefaultBudget = 32u << 20;
    // Sizes are clamped to this many pixels per em.
    static constexpr float kMaxSize = 1024.0f;

    explicit GlyphCache(size_t budgetBytes = kDefaultBudget) : budget_(budgetBytes) {}

    // The key of `glyph` at `sizePx` pixels per em drawn with the pen at x = `penX` (only its
    // fraction counts: the quarter-pixel phase).
    static GlyphKey KeyFor(const GlyphSource& source, uint16_t glyph, float sizePx, float penX);

    // The bitmap of `key`, rasterized from `source` on a miss (with gfx::Rasterizer, the
    // outline through UnitsToEm, scaled to the size, y flipped and shifted by the phase). A
    // glyph whose outline cannot be read is cached empty. The pointer stays valid until a Get
    // after the next BeginFrame evicts it, or Clear.
    const GlyphBitmap* Get(const GlyphSource& source, const GlyphKey& key);
    const GlyphBitmap* Get(const GlyphSource& source, uint16_t glyph, float sizePx, float penX) {
        return Get(source, KeyFor(source, glyph, sizePx, penX));
    }
    // The cached bitmap of `key` without filling or touching the LRU order; nullptr on a miss.
    const GlyphBitmap* Find(const GlyphKey& key) const;

    // Starts a frame: entries used since the previous BeginFrame become evictable. Entries used
    // in the current frame are never evicted, so the cache may exceed its budget for one frame.
    void BeginFrame() { frame_++; }
    void Clear();

    size_t Bytes() const { return bytes_; }
    size_t Count() const { return map_.size(); }

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
    size_t bytes_ = 0;
    uint64_t frame_ = 1;
    List lru_;   // most recent first
    std::unordered_map<GlyphKey, List::iterator, GlyphKeyHash> map_;
};

// Rasterizes one glyph (what GlyphCache::Get does on a miss).
void RasterizeGlyph(const GlyphSource& source, uint16_t glyph, float sizePx, float phasePx, GlyphBitmap& out);

} // namespace rgui::text
