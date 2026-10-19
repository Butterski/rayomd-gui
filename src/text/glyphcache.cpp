// SPDX-License-Identifier: Apache-2.0
#include "text/glyphcache.h"

#include "common/exact_math.h"
#include "common/thread_object.h"
#include "gfx/budget.h"
#include "gfx/raster.h"

#include <algorithm>
#include <cstring>

namespace rgui::text {
namespace {

using RayoMd::ExactMath::Ceil;
using RayoMd::ExactMath::Floor;

class BitmapSink final : public gfx::SpanSink {
public:
    BitmapSink(GlyphBitmap& bitmap, gfx::RectI box) : bitmap_(bitmap), box_(box) {}
    void Span(int y, int x0, int width, const uint8_t* alpha) override {
        if (y < box_.y0 || y >= box_.y1 || x0 < box_.x0 || width <= 0 || x0 + width > box_.x1) return;
        std::memcpy(&bitmap_.alpha[static_cast<size_t>(y - box_.y0) * static_cast<size_t>(box_.Width()) +
                        static_cast<size_t>(x0 - box_.x0)],
            alpha, static_cast<size_t>(width));
    }

private:
    GlyphBitmap& bitmap_;
    gfx::RectI box_;
};

} // namespace

size_t GlyphKeyHash::operator()(const GlyphKey& k) const {
    uint64_t h = (static_cast<uint64_t>(k.source) << 32) ^ (static_cast<uint64_t>(k.glyph) << 16) ^ k.phase;
    h ^= static_cast<uint64_t>(k.size64) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return static_cast<size_t>(h);
}

GlyphKey GlyphCache::KeyFor(const GlyphSource& source, uint16_t glyph, float sizePx, float penX) {
    GlyphKey key;
    key.source = source.SourceId();
    key.glyph = glyph;
    double size = sizePx;
    if (!(size > 0)) size = 0;
    if (size > kMaxSize) size = kMaxSize;
    key.size64 = static_cast<uint32_t>(RayoMd::ExactMath::LlRound(size * 64.0));
    const double x = penX;
    double fraction = (x == x && x > -1e9 && x < 1e9) ? x - Floor(x) : 0.0;
    int phase = static_cast<int>(fraction * 4.0);
    key.phase = static_cast<uint8_t>(std::clamp(phase, 0, 3));
    return key;
}

void RasterizeGlyph(const GlyphSource& source, uint16_t glyph, float sizePx, float phasePx, GlyphBitmap& out,
                    gfx::WorkBudget* budget) {
    out = GlyphBitmap();
    if (!(sizePx > 0)) return;
    gfx::Path path;
    if (!source.GlyphPath(glyph, path) || path.Empty()) return;
    const gfx::Matrix m = source.UnitsToEm(glyph)
                              .Multiply(gfx::Matrix::Scale(sizePx, -sizePx))
                              .Multiply(gfx::Matrix::Translate(phasePx, 0));
    const gfx::RectF b = path.Bounds();
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    const double cx[4] = {b.x, b.x + b.w, b.x, b.x + b.w};
    const double cy[4] = {b.y, b.y, b.y + b.h, b.y + b.h};
    for (int i = 0; i < 4; i++) {
        double x = 0, y = 0;
        m.Apply(cx[i], cy[i], x, y);
        if (!(x == x && y == y)) return;
        if (i == 0 || x < minX) minX = x;
        if (i == 0 || x > maxX) maxX = x;
        if (i == 0 || y < minY) minY = y;
        if (i == 0 || y > maxY) maxY = y;
    }
    // A malformed outline may reach far out; nothing real does past 1.5 em plus a margin.
    const double limit = 1.5 * sizePx + 32.0;
    minX = std::max(minX, -limit);
    minY = std::max(minY, -limit);
    maxX = std::min(maxX, limit);
    maxY = std::min(maxY, limit);
    const gfx::RectI box{static_cast<int>(Floor(minX)), static_cast<int>(Floor(minY)), static_cast<int>(Ceil(maxX)),
        static_cast<int>(Ceil(maxY))};
    if (box.Empty()) return;
    out.left = box.x0;
    out.top = box.y0;
    out.width = box.Width();
    out.height = box.Height();
    const size_t pixels = static_cast<size_t>(out.width) * static_cast<size_t>(out.height);
    if (budget && !budget->Take(budget->pixels, pixels, gfx::WorkBudget::Stop::Pixels)) {
        out = GlyphBitmap();
        return;
    }
    out.alpha.assign(pixels, 0);
    // The thread's rasterizer, its buffers kept from glyph to glyph. A thread object, never a plain
    // thread_local: MinGW frees thread storage before thread_local destructors run (thread_object.h).
    RAYOMD_THREAD_OBJECT(gfx::Rasterizer, rasterizer);
    rasterizer.SetBudget(budget);
    rasterizer.Reset(box);
    rasterizer.AddPath(path, m);
    BitmapSink sink(out, box);
    rasterizer.Sweep(gfx::FillRule::NonZero, sink);
    rasterizer.SetBudget(nullptr);
}

const GlyphBitmap* GlyphCache::Get(const GlyphSource& source, const GlyphKey& key, gfx::WorkBudget* budget) {
    const auto found = map_.find(key);
    if (found != map_.end()) {
        found->second->frame = frame_;
        lru_.splice(lru_.begin(), lru_, found->second);
        return &found->second->bitmap;
    }
    Entry entry;
    entry.key = key;
    entry.frame = frame_;
    if (!store_ || !store_->Find(key, entry.bitmap)) {
        RasterizeGlyph(source, key.glyph, static_cast<float>(key.size64 / 64.0), static_cast<float>(key.phase / 4.0),
            entry.bitmap, budget);
        if (store_ && !(budget && budget->Stopped())) store_->Put(key, entry.bitmap);
    }
    const size_t cost = Cost(entry.bitmap);
    if (bytes_ + cost > budget_) {
        bytes_ += cost;  // what Evict must free for this entry to fit
        Evict();
        bytes_ -= cost;
    }
    if ((budget && budget->Stopped()) || bytes_ + cost > 2 * budget_) {
        scratch_ = std::move(entry.bitmap);
        return &scratch_;
    }
    lru_.push_front(std::move(entry));
    map_.emplace(key, lru_.begin());
    bytes_ += cost;
    return &lru_.front().bitmap;
}

const GlyphBitmap* GlyphCache::Find(const GlyphKey& key) const {
    const auto found = map_.find(key);
    return found != map_.end() ? &found->second->bitmap : nullptr;
}

void GlyphCache::Evict() {
    while (bytes_ > budget_ && !lru_.empty() && lru_.back().frame != frame_) {
        bytes_ -= Cost(lru_.back().bitmap);
        map_.erase(lru_.back().key);
        lru_.pop_back();
    }
}

void GlyphCache::Clear() {
    map_.clear();
    lru_.clear();
    bytes_ = 0;
}

bool GlyphStore::Find(const GlyphKey& key, GlyphBitmap& out) const {
    Shared found;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = map_.find(key);
        if (it == map_.end()) return false;
        found = it->second;
    }
    out = *found;
    return true;
}

void GlyphStore::Put(const GlyphKey& key, const GlyphBitmap& bitmap) {
    auto copy = std::make_shared<const GlyphBitmap>(bitmap);
    const size_t cost = Cost(bitmap);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!map_.emplace(key, std::move(copy)).second) return;
    order_.push_back(key);
    bytes_ += cost;
    while (bytes_ > budget_ && order_.size() > 1) {
        const auto oldest = map_.find(order_.front());
        bytes_ -= Cost(*oldest->second);
        map_.erase(oldest);
        order_.pop_front();
    }
}

size_t GlyphStore::Bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

} // namespace rgui::text
