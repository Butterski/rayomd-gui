// SPDX-License-Identifier: Apache-2.0
// The preview's page images (plan Phase 8 step 5): a pool of preview threads renders the pages
// of the newest preview PDF with pdf::RenderPage, the pages nearest the viewport first. Images
// are kept by (PdfFile::PageKey, width) in a 256 MiB LRU, so the pages an edit did not change
// reuse their images in the next build at once. Until a page's new image is ready, the image it
// showed before (the previous build's, or another width's) stays on screen. The build worker
// hands each new PDF to Prefetch as soon as it is parsed: the pages the window wanted last start
// rendering then, and their keys are computed there, off the UI thread.
//
// A tall page (StripCount: one long page, --page-size=WIDTHxauto) is drawn in strips of
// kStripRows rows with pdf::RenderPageRows, and only the strips in view and next to it are
// rendered; the LRU keys them by (PageKey, width, strip). The page's strips share a
// pdf::BandIndex: the first strip drawn reads the whole content and notes where its groups
// draw, the others pass over those outside them.
#pragma once

#include "gfx/surface.h"
#include "pdf/reader.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rgui {

class Platform;

namespace pdf {
class BandIndex;
class PdfFile;
}
namespace text {
class FontIndex;
}

class PreviewPages {
public:
    static constexpr size_t kCacheBudget = size_t(256) << 20;
    // Rows of a strip of a tall page.
    static constexpr int kStripRows = 2048;

    // The strips a page `widthPt` x `heightPt` points is drawn in at `widthPx`: 0 (drawn whole)
    // unless its image (pdf::RenderHeight) is over kStripRows rows and either more than twice
    // as tall as wide or beyond what pdf::RenderPage draws (kMaxRenderSide, kMaxRenderPixels).
    static int StripCount(double widthPt, double heightPt, int widthPx);

    // A page wanted, and for a page drawn in strips the rows of its image at the width
    // requested that are in view, [top, bottom) (top == bottom: the strip with row `top`).
    struct Want {
        int page = 0;
        int top = 0;
        int bottom = 0;
    };
    // A strip's image as drawn: `top` is its first row in the page's image `widthPx` across.
    struct Strip {
        const gfx::Image* image = nullptr;
        int widthPx = 0;
        int top = 0;
    };

    // clamp(hardware_concurrency / 2, 1, 3).
    static int DefaultThreads();

    // Starts `threads` preview threads (DefaultThreads() when < 1). `fontIndex` (outliving this)
    // serves the standard PDF fonts; images and fonts of the PDFs come from the process's shared
    // pdf::SharedImageCache() and pdf::SharedFontCache(). `onPageReady` runs on the UI thread
    // (through Platform::PostToUi) after a page got a new image: request a frame there.
    PreviewPages(Platform& platform, const text::FontIndex* fontIndex, std::function<void()> onPageReady,
                 int threads = 0);
    // Drops the queued pages and abandons the ones being drawn (they stop within about a
    // millisecond), then joins the threads; no result is delivered.
    ~PreviewPages();
    PreviewPages(const PreviewPages&) = delete;
    PreviewPages& operator=(const PreviewPages&) = delete;

    // ---- UI thread ----

    // The pages of a new build. Page i keeps showing the image it had until its own is ready.
    // `keys` are page keys already known (Prefetch's).
    void SetDocument(std::shared_ptr<const pdf::PdfFile> pdf, int generation,
                     const std::vector<pdf::PdfPageKey>& keys = {});
    const std::shared_ptr<const pdf::PdfFile>& Document() const { return pdf_; }
    int Generation() const { return generation_; }
    int PageCount() const { return static_cast<int>(pages_.size()); }
    // The page's MediaBox size in points.
    double PageWidthPt(int page) const { return pages_[static_cast<size_t>(page)].widthPt; }
    double PageHeightPt(int page) const { return pages_[static_cast<size_t>(page)].heightPt; }
    int StripCount(int page, int widthPx) const;

    // Wants `pages` (the most urgent first) at `widthPx` device pixels across: whole pages, and
    // of pages drawn in strips the strips holding the rows in view (the one nearest their middle
    // first), after all of them the strip above and below those. Pages and strips whose image is
    // in the cache get it now (true: draw them again, the frame drawn before this call shows
    // older images); the others are queued for the preview threads in this order,
    // replacing whatever was queued before (whatever is no longer wanted is dropped before it
    // starts). Renders of an older build that are not wanted now are abandoned
    // (pdf::RenderEnvironment::cancel): typing never waits behind a page no longer shown.
    bool Request(const std::vector<Want>& pages, int widthPx);

    // What to draw for `page`: its image, or while that renders the one it showed before;
    // nullptr when it has none yet (or its render failed: a blank page). A page drawn in strips
    // may keep a whole image of an earlier width or build here, under its strips, until the
    // strips in view are current.
    const gfx::Image* Image(int page) const;
    // The strip images `page` shows, into `out` in drawing order (those of an earlier width,
    // under the current ones, first): drawn over Image(page), each at its rows.
    void Strips(int page, std::vector<Strip>& out) const;
    // True when `page` shows this build's page at `widthPx` (or its render failed for good); for
    // a page drawn in strips, every strip holding a row of [top, bottom) (or row `top`).
    bool Current(int page, int widthPx, int top = 0, int bottom = 0) const;

    // ---- Any thread ----

    // A new build's PDF as soon as it is parsed (the build worker calls it, before the window
    // has the build): the pages the last Request wanted, at its width, start rendering at once,
    // ahead of whatever is queued: whole pages whose image the cache lacks and that are not
    // being drawn. Returns the keys of those pages it computed (off the UI thread), for
    // SetDocument.
    std::vector<pdf::PdfPageKey> Prefetch(const std::shared_ptr<const pdf::PdfFile>& pdf, int generation);

    // Bytes of images in the cache, pages and strips rendered since the start, and renders
    // abandoned (tests).
    size_t CacheBytes() const;
    uint64_t RenderedCount() const;
    uint64_t CancelledCount() const;

private:
    // A strip of a page drawn in strips, at the page's stripWidth.
    struct StripSlot {
        std::shared_ptr<const gfx::Image> image;
        pdf::PdfHash imageKey;   // the PageKey the image shows
        int imageWidth = 0;      // the page width it was drawn at (strips under the current ones)
        int top = 0;             // its first row at imageWidth
        bool failed = false;     // the render of (key, stripWidth, strip) failed
    };
    struct PageSlot {
        double widthPt = 612;
        double heightPt = 792;
        pdf::PdfHash key;     // PdfFile::PageKey, computed when first wanted
        bool keyKnown = false;
        std::shared_ptr<const gfx::Image> image;
        pdf::PdfHash imageKey;   // the PageKey and width the image shows
        int imageWidth = 0;
        bool failed = false;     // the render of (key, failedWidth) failed
        int failedWidth = 0;
        // Strips, when the page is drawn in strips at stripWidth: one slot per strip (the images
        // of those within two of the view's), and the strips of an earlier width still shown
        // under them.
        int stripWidth = 0;
        std::vector<StripSlot> strips;
        std::vector<StripSlot> under;
        // What its strips of this build share, at every width.
        std::shared_ptr<pdf::BandIndex> bands;
    };
    struct Job {
        std::shared_ptr<const pdf::PdfFile> pdf;
        int page = 0;
        int widthPx = 0;
        int strip = -1;          // -1: the whole page
        pdf::PdfHash key;
        int generation = 0;
        std::shared_ptr<pdf::BandIndex> bands;   // a strip's
    };
    struct CacheKey {
        pdf::PdfHash pageKey;
        int widthPx = 0;
        int strip = -1;
        bool operator==(const CacheKey& o) const {
            return pageKey == o.pageKey && widthPx == o.widthPx && strip == o.strip;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            const uint64_t mix = static_cast<uint64_t>(static_cast<uint32_t>(k.widthPx)) |
                                 (static_cast<uint64_t>(static_cast<uint32_t>(k.strip)) << 32);
            return pdf::PdfHashHasher()(k.pageKey) ^ static_cast<size_t>(mix * 0x9E3779B97F4A7C15ull);
        }
    };
    struct CacheEntry {
        CacheKey key;
        std::shared_ptr<const gfx::Image> image;   // nullptr: the render failed
        size_t bytes = 0;
    };
    // A render being drawn, and the flag that abandons it.
    struct InFlight {
        CacheKey key;
        int generation = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    // Shared with tasks posted to the UI thread, which may run after this is gone.
    struct Link {
        bool alive = true;   // UI thread only
        PreviewPages* owner = nullptr;
    };

    void Run();
    void Delivered(const Job& job, const std::shared_ptr<const gfx::Image>& image);
    // Looks `key` up (moving it to the front); false on a miss. Needs mutex_.
    bool Lookup(const CacheKey& key, std::shared_ptr<const gfx::Image>& image);
    void Insert(const CacheKey& key, std::shared_ptr<const gfx::Image> image);
    static void Assign(PageSlot& slot, const CacheKey& key, std::shared_ptr<const gfx::Image> image);
    static bool StripCurrent(const PageSlot& slot, int strip);
    // Lays the page's strips out for `widthPx` (`count` of them): those of another width go
    // under the new ones while they have images.
    static void LayOutStrips(PageSlot& slot, int widthPx, int count);
    // Wants (page, widthPx, strip) of the shown build: assigned from the cache (true), or added to
    // `jobs` unless being drawn or queued; its key goes to `wanted`. Needs mutex_.
    bool WantPiece(int page, int widthPx, int strip, std::vector<Job>& jobs, std::vector<CacheKey>& wanted);

    Platform& platform_;
    const text::FontIndex* fontIndex_;
    std::function<void()> onPageReady_;
    std::shared_ptr<Link> link_;

    // UI thread.
    std::shared_ptr<const pdf::PdfFile> pdf_;
    int generation_ = 0;
    int wantedWidth_ = 0;
    std::vector<PageSlot> pages_;

    // Shared with the preview threads.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::vector<Job> queue_;   // the most urgent first
    // What the last Request wanted, for Prefetch: its pages (the most urgent first) and width.
    std::vector<int> prefetchPages_;
    int prefetchWidth_ = 0;
    std::vector<InFlight> inFlight_;
    std::list<CacheEntry> lru_;   // most recent first
    std::unordered_map<CacheKey, std::list<CacheEntry>::iterator, CacheKeyHash> cache_;
    size_t cacheBytes_ = 0;
    uint64_t rendered_ = 0;
    uint64_t cancelled_ = 0;
    std::vector<std::thread> threads_;
};

} // namespace rgui
