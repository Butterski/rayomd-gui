// SPDX-License-Identifier: Apache-2.0
#include "ui/compositor.h"

#include "common/exact_math.h"
#include "gfx/canvas.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rgui::ui {

namespace {

using RayoMd::ExactMath::Floor;
using RayoMd::ExactMath::LlRound;

int FloorToInt(double v) {
    if (!(v > -(1 << 28))) return -(1 << 28);
    if (!(v < (1 << 28))) return 1 << 28;
    return static_cast<int>(Floor(v));
}

int RoundToInt(double v) {
    if (!(v > -(1 << 28))) return -(1 << 28);
    if (!(v < (1 << 28))) return 1 << 28;
    return static_cast<int>(LlRound(v));
}

gfx::RectI ToI(const gfx::RectF& r) {
    return {RoundToInt(r.x), RoundToInt(r.y), RoundToInt(static_cast<double>(r.x) + r.w),
            RoundToInt(static_cast<double>(r.y) + r.h)};
}

gfx::Matrix ImageMatrix(const Command& c) {
    return {c.rect.w, 0, 0, c.rect.h, c.rect.x, c.rect.y};
}

// A worker's drawing tools.
struct Worker {
    gfx::Canvas canvas;
    gfx::Path path;
};

void DrawCommand(Worker& w, const Command& c, const gfx::RectI& tile, const PlacedGlyph* placed, const IconCache& icons) {
    gfx::Canvas& canvas = w.canvas;
    switch (c.type) {
    case CommandType::Rect:
        canvas.FillRect(ToI(c.rect), c.color);
        break;
    case CommandType::RectStroke: {
        const gfx::RectI r = ToI(c.rect);
        const int s = static_cast<int>(c.width);
        if (s * 2 >= r.Width() || s * 2 >= r.Height()) {
            canvas.FillRect(r, c.color);
            break;
        }
        canvas.FillRect({r.x0, r.y0, r.x1, r.y0 + s}, c.color);
        canvas.FillRect({r.x0, r.y1 - s, r.x1, r.y1}, c.color);
        canvas.FillRect({r.x0, r.y0 + s, r.x0 + s, r.y1 - s}, c.color);
        canvas.FillRect({r.x1 - s, r.y0 + s, r.x1, r.y1 - s}, c.color);
        break;
    }
    case CommandType::RoundRect: {
        const float r = std::min(c.radius, std::min(c.rect.w, c.rect.h) / 2);
        w.path.Clear();
        w.path.AddRoundRect(c.rect.x, c.rect.y, c.rect.w, c.rect.h, r);
        canvas.FillPath(w.path, gfx::Matrix(), gfx::FillRule::NonZero, gfx::Paint::Solid(c.color));
        break;
    }
    case CommandType::RoundRectStroke: {
        const float r = std::min(c.radius, std::min(c.rect.w, c.rect.h) / 2);
        w.path.Clear();
        w.path.AddRoundRect(c.rect.x, c.rect.y, c.rect.w, c.rect.h, r);
        const float iw = c.rect.w - 2 * c.width, ih = c.rect.h - 2 * c.width;
        if (iw > 0 && ih > 0) {
            const float ir = std::max(0.0f, std::min(r - c.width, std::min(iw, ih) / 2));
            w.path.AddRoundRect(c.rect.x + c.width, c.rect.y + c.width, iw, ih, ir);
        }
        canvas.FillPath(w.path, gfx::Matrix(), gfx::FillRule::EvenOdd, gfx::Paint::Solid(c.color));
        break;
    }
    case CommandType::Glyphs: {
        const PlacedGlyph* g = placed + c.glyphBegin;
        for (uint32_t i = 0; i < c.glyphCount; i++) {
            const text::GlyphBitmap* bm = g[i].bitmap;
            if (!bm) continue;
            const gfx::RectI dst{g[i].x0, g[i].y0, g[i].x0 + bm->width, g[i].y0 + bm->height};
            if (dst.Intersect(tile).Empty()) continue;
            canvas.DrawMask(bm->alpha.data(), bm->width, dst, c.color);
        }
        break;
    }
    case CommandType::Image: {
        if (!c.image) break;
        canvas.DrawImage(*c.image, ImageMatrix(c), c.opacity, true);
        break;
    }
    case CommandType::Icon: {
        const gfx::RectI r = ToI(c.rect);
        const IconMask* mask = icons.Find(c.icon, r.Width());
        if (!mask || mask->size != r.Width()) break;
        canvas.DrawMask(mask->alpha.data(), mask->size, {r.x0, r.y0, r.x0 + mask->size, r.y0 + mask->size}, c.color);
        break;
    }
    case CommandType::ClipPush:
    case CommandType::ClipPop:
        break;
    }
}

bool Contains(const gfx::RectI& outer, const gfx::RectI& inner) {
    return inner.x0 >= outer.x0 && inner.y0 >= outer.y0 && inner.x1 <= outer.x1 && inner.y1 <= outer.y1;
}

// Whether `c` stores every pixel of `tile` whatever was there: an opaque rectangle or an image
// copied row by row (Canvas::CopiedImageRect) covering the tile within the command's clip. The
// commands before it and the background are then not drawn there.
bool Covers(const Command& c, const gfx::RectI& tile) {
    if (!Contains(c.clip, tile)) return false;
    if (c.type == CommandType::Rect) return (c.color >> 24) == 255 && Contains(ToI(c.rect), tile);
    gfx::RectI copied;
    return c.type == CommandType::Image && c.image &&
           gfx::Canvas::CopiedImageRect(*c.image, ImageMatrix(c), c.opacity, copied) && Contains(copied, tile);
}

} // namespace

struct Compositor::Pool {
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<Worker>> workers;   // [0] is the calling thread's
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;
    uint64_t generation = 0;
    int busy = 0;
    bool quit = false;
    std::atomic<size_t> next{0};
};

int Compositor::DefaultThreads() {
    const int n = static_cast<int>(std::thread::hardware_concurrency()) - 1;
    return std::min(4, std::max(1, n));
}

Compositor::Compositor(int threads) : pool_(std::make_unique<Pool>()) {
    const int n = threads < 1 ? DefaultThreads() : threads;
    for (int i = 0; i <= n; i++) pool_->workers.push_back(std::make_unique<Worker>());
    for (int i = 1; i <= n; i++) {
        pool_->threads.emplace_back([this, i] {
            Pool& p = *pool_;
            uint64_t seen = 0;
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(p.mutex);
                    p.wake.wait(lock, [&] { return p.quit || p.generation != seen; });
                    if (p.quit) return;
                    seen = p.generation;
                }
                RasterTiles(i);
                std::lock_guard<std::mutex> lock(p.mutex);
                if (--p.busy == 0) p.done.notify_one();
            }
        });
    }
}

Compositor::~Compositor() {
    {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        pool_->quit = true;
    }
    pool_->wake.notify_all();
    for (std::thread& t : pool_->threads) t.join();
}

int Compositor::Threads() const { return static_cast<int>(pool_->threads.size()); }

void Compositor::InvalidateAll() { invalid_ = true; }

void Compositor::RasterTiles(int worker) {
    Worker& w = *pool_->workers[static_cast<size_t>(worker)];
    w.canvas.SetTarget(target_);
    const std::vector<Command>& commands = list_->Commands();
    for (;;) {
        const size_t n = pool_->next.fetch_add(1, std::memory_order_relaxed);
        if (n >= dirtyTiles_.size()) break;
        const uint32_t t = dirtyTiles_[n];
        const int col = static_cast<int>(t % static_cast<uint32_t>(cols_));
        const int row = static_cast<int>(t / static_cast<uint32_t>(cols_));
        const gfx::RectI tile{col * kTileSize, row * kTileSize, std::min(width_, (col + 1) * kTileSize),
                              std::min(height_, (row + 1) * kTileSize)};
        // The last command that stores every pixel of the tile hides the background and all
        // before it.
        const std::vector<uint32_t>& indices = tileCommands_[t];
        size_t first = indices.size();
        while (first > 0 && !Covers(commands[indices[first - 1]], tile)) first--;
        if (first == 0) {
            for (int y = tile.y0; y < tile.y1; y++) {
                gfx::FillPixels(target_.Row(y) + tile.x0, static_cast<size_t>(tile.x1 - tile.x0), background_);
            }
        } else {
            first--;
        }
        for (size_t k = first; k < indices.size(); k++) {
            const Command& c = commands[indices[k]];
            const gfx::RectI clip = c.clip.Intersect(tile);
            if (clip.Empty()) continue;
            w.canvas.PushClipRect(clip);
            DrawCommand(w, c, tile, placed_.data(), *iconCache_);
            w.canvas.PopClip();
        }
    }
}

const std::vector<gfx::RectI>& Compositor::Render(const DisplayList& list, uint32_t background,
                                                  const gfx::Surface& target, text::GlyphCache& glyphs,
                                                  IconCache& icons) {
    damage_.clear();
    dirtyTiles_.clear();
    const int width = std::min(list.Width(), target.width);
    const int height = std::min(list.Height(), target.height);
    if (!target.pixels || width <= 0 || height <= 0) return damage_;

    const int cols = (width + kTileSize - 1) / kTileSize;
    const int rows = (height + kTileSize - 1) / kTileSize;
    if (cols != cols_ || rows != rows_ || width != width_ || height != height_ || list.Scale() != scale_ ||
        target.pixels != lastPixels_ || target.stride != lastStride_) {
        invalid_ = true;
    }
    cols_ = cols;
    rows_ = rows;
    width_ = width;
    height_ = height;
    scale_ = list.Scale();
    lastPixels_ = target.pixels;
    lastStride_ = target.stride;
    const size_t tiles = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    tileHashes_.resize(tiles, 0);
    tileCommands_.resize(tiles);
    for (auto& v : tileCommands_) v.clear();

    // Binning: every drawing command into the tiles its bounds (already within its clip) touch.
    const std::vector<Command>& commands = list.Commands();
    for (size_t i = 0; i < commands.size(); i++) {
        const Command& c = commands[i];
        if (c.type == CommandType::ClipPush || c.type == CommandType::ClipPop) continue;
        const gfx::RectI b = c.bounds.Intersect({0, 0, width, height});
        if (b.Empty()) continue;
        const int c0 = b.x0 / kTileSize, c1 = (b.x1 - 1) / kTileSize;
        const int r0 = b.y0 / kTileSize, r1 = (b.y1 - 1) / kTileSize;
        for (int r = r0; r <= r1; r++) {
            for (int col = c0; col <= c1; col++) {
                tileCommands_[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(col)]
                    .push_back(static_cast<uint32_t>(i));
            }
        }
    }

    // Hashes and dirty tiles.
    dirtyMask_.assign(tiles, 0);
    for (size_t t = 0; t < tiles; t++) {
        uint64_t h = Fnv1a64(&background, sizeof(background));
        for (const uint32_t index : tileCommands_[t]) {
            const uint64_t ch = commands[index].hash;
            h = Fnv1a64(&ch, sizeof(ch), h);
        }
        if (invalid_ || h != tileHashes_[t]) {
            tileHashes_[t] = h;
            dirtyMask_[t] = 1;
            dirtyTiles_.push_back(static_cast<uint32_t>(t));
        }
    }
    invalid_ = false;
    if (dirtyTiles_.empty()) return damage_;

    // Prepare: the glyphs and icons the dirty tiles draw, into the caches, on this thread, and
    // where each glyph's bitmap lands.
    glyphs.BeginFrame();
    prepared_.assign(commands.size(), 0);
    placed_.resize(list.GlyphArena().size());
    unkept_.clear();
    for (const uint32_t t : dirtyTiles_) {
        for (const uint32_t index : tileCommands_[t]) {
            if (prepared_[index]) continue;
            prepared_[index] = 1;
            const Command& c = commands[index];
            if (c.type == CommandType::Glyphs) {
                const GlyphInstance* g = list.GlyphArena().data() + c.glyphBegin;
                PlacedGlyph* out = placed_.data() + c.glyphBegin;
                for (uint32_t i = 0; i < c.glyphCount; i++) {
                    out[i] = PlacedGlyph{};
                    if (!g[i].source) continue;
                    const text::GlyphBitmap* bm = glyphs.Get(*g[i].source, g[i].glyph, c.size, g[i].x);
                    if (!bm) continue;
                    if (!glyphs.Keeps(bm)) {
                        unkept_.push_back({c.glyphBegin + i, index});
                        continue;
                    }
                    if (bm->width <= 0 || bm->height <= 0) continue;
                    out[i] = PlacedGlyph{bm, FloorToInt(g[i].x) + bm->left, RoundToInt(g[i].y) + bm->top};
                }
            } else if (c.type == CommandType::Icon) {
                icons.Get(c.icon, RoundToInt(c.rect.w));
            }
        }
    }
    // A glyph the cache did not keep is drawn only if a later Get of its key kept it.
    for (const auto& [i, index] : unkept_) {
        const GlyphInstance& g = list.GlyphArena()[i];
        const Command& c = commands[index];
        const text::GlyphBitmap* bm = glyphs.Find(text::GlyphCache::KeyFor(*g.source, g.glyph, c.size, g.x));
        if (bm && bm->width > 0 && bm->height > 0) {
            placed_[i] = PlacedGlyph{bm, FloorToInt(g.x) + bm->left, RoundToInt(g.y) + bm->top};
        }
    }

    // Raster: tile threads and this one take dirty tiles by an atomic counter.
    list_ = &list;
    iconCache_ = &icons;
    target_ = target;
    background_ = background;
    Pool& p = *pool_;
    p.next.store(0, std::memory_order_relaxed);
    const bool parallel = dirtyTiles_.size() > 2 && !p.threads.empty();
    if (parallel) {
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            p.busy = static_cast<int>(p.threads.size());
            p.generation++;
        }
        p.wake.notify_all();
    }
    RasterTiles(0);
    if (parallel) {
        std::unique_lock<std::mutex> lock(p.mutex);
        p.done.wait(lock, [&] { return p.busy == 0; });
    }
    list_ = nullptr;

    MergeDamage(dirtyMask_, cols, rows, kTileSize, width, height, damage_);
    return damage_;
}

void Compositor::MergeDamage(const std::vector<uint8_t>& dirty, int cols, int rows, int tileSize, int width,
                             int height, std::vector<gfx::RectI>& out) {
    out.clear();
    // Rectangles still open for vertical merging: those ending at the previous tile row.
    std::vector<size_t> open, nextOpen;
    for (int r = 0; r < rows; r++) {
        nextOpen.clear();
        const int y0 = r * tileSize, y1 = std::min(height, (r + 1) * tileSize);
        int c = 0;
        while (c < cols) {
            if (!dirty[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)]) {
                c++;
                continue;
            }
            const int start = c;
            while (c < cols && dirty[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)]) c++;
            const int x0 = start * tileSize, x1 = std::min(width, c * tileSize);
            bool merged = false;
            for (const size_t i : open) {
                if (out[i].x0 == x0 && out[i].x1 == x1 && out[i].y1 == y0) {
                    out[i].y1 = y1;
                    nextOpen.push_back(i);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                nextOpen.push_back(out.size());
                out.push_back({x0, y0, x1, y1});
            }
        }
        open.swap(nextOpen);
    }
    if (out.size() > kMaxDamageRects) {
        gfx::RectI box;
        for (const gfx::RectI& r : out) box = box.Union(r);
        out.assign(1, box);
    }
}

} // namespace rgui::ui
