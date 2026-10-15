// SPDX-License-Identifier: Apache-2.0
// The tile compositor: a display list into a framebuffer, redrawing only the 64 x 64 tiles
// whose commands changed, on a pool of tile threads, and reporting the damage.
#pragma once

#include "gfx/geometry.h"
#include "gfx/surface.h"
#include "text/glyphcache.h"
#include "ui/displaylist.h"
#include "ui/icons.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rgui::ui {

class Compositor {
public:
    static constexpr int kTileSize = 64;
    static constexpr size_t kMaxDamageRects = 32;

    // clamp(hardware_concurrency - 1, 1, 4).
    static int DefaultThreads();

    // `threads` tile threads (DefaultThreads() when < 1); the calling thread rasterizes too.
    explicit Compositor(int threads = 0);
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Draws `list` over `background` (premultiplied) into `target`, whose size must be the
    // list's; returns the device rectangles that changed. A tile is redrawn when the hash of the
    // background and its commands (in order, with their clips) differs from the last Render's;
    // a new size, scale or target redraws all. The glyphs and icons of dirty tiles are put into
    // the caches on this thread first (the prepare pass); tile threads then only read them.
    const std::vector<gfx::RectI>& Render(const DisplayList& list, uint32_t background, const gfx::Surface& target,
                                          text::GlyphCache& glyphs, IconCache& icons);
    // The next Render redraws everything.
    void InvalidateAll();

    size_t TileCount() const { return tileHashes_.size(); }
    size_t DirtyTileCount() const { return dirtyTiles_.size(); }
    int Threads() const;

    // The damage of a grid of `cols` x `rows` tiles of `tileSize` (`dirty` row-major, nonzero
    // dirty) over a `width` x `height` frame: each tile row's runs of dirty tiles, runs of
    // equal extent in consecutive rows merged into one rectangle; more than kMaxDamageRects
    // become their bounding box.
    static void MergeDamage(const std::vector<uint8_t>& dirty, int cols, int rows, int tileSize, int width, int height,
                            std::vector<gfx::RectI>& out);

private:
    struct Pool;

    void RasterTiles(int worker);

    std::unique_ptr<Pool> pool_;
    int cols_ = 0;
    int rows_ = 0;
    int width_ = 0;
    int height_ = 0;
    double scale_ = 0;
    const uint32_t* lastPixels_ = nullptr;
    int lastStride_ = 0;
    bool invalid_ = true;
    std::vector<uint64_t> tileHashes_;
    std::vector<std::vector<uint32_t>> tileCommands_;   // command indices per tile
    std::vector<uint32_t> dirtyTiles_;
    std::vector<uint8_t> dirtyMask_;
    std::vector<uint8_t> prepared_;
    std::vector<gfx::RectI> damage_;

    // The job of the current Render, read by tile threads.
    const DisplayList* list_ = nullptr;
    const text::GlyphCache* glyphCache_ = nullptr;
    const IconCache* iconCache_ = nullptr;
    gfx::Surface target_;
    uint32_t background_ = 0;
};

} // namespace rgui::ui
