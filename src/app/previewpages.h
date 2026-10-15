// SPDX-License-Identifier: Apache-2.0
// The preview's page images (plan Phase 8 step 5): a pool of preview threads renders the pages
// of the newest preview PDF with pdf::RenderPage, the pages nearest the viewport first. Images
// are kept by (PdfFile::PageKey, width) in a 256 MiB LRU, so the pages an edit did not change
// reuse their images in the next build at once. Until a page's new image is ready, the image it
// showed before (the previous build's, or another width's) stays on screen.
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
class PdfFile;
}
namespace text {
class FontIndex;
}

class PreviewPages {
public:
    static constexpr size_t kCacheBudget = size_t(256) << 20;

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
    void SetDocument(std::shared_ptr<const pdf::PdfFile> pdf, int generation);
    const std::shared_ptr<const pdf::PdfFile>& Document() const { return pdf_; }
    int Generation() const { return generation_; }
    int PageCount() const { return static_cast<int>(pages_.size()); }
    // The page's MediaBox size in points.
    double PageWidthPt(int page) const { return pages_[static_cast<size_t>(page)].widthPt; }
    double PageHeightPt(int page) const { return pages_[static_cast<size_t>(page)].heightPt; }

    // Wants `pages` (indices, the most urgent first) at `widthPx` device pixels across. Pages
    // whose image is in the cache get it now; the others are queued for the preview threads in
    // this order, replacing whatever was queued before (pages no longer wanted are dropped
    // before they start). Renders of an older build whose page and width are not wanted now
    // are abandoned (pdf::RenderEnvironment::cancel): typing never waits behind a page no
    // longer shown.
    void Request(const std::vector<int>& pages, int widthPx);

    // What to draw for `page`: its image, or while that renders the one it showed before;
    // nullptr when it has none yet (or its render failed: a blank page).
    const gfx::Image* Image(int page) const;
    // True when `page` shows this build's page at `widthPx` (or its render failed for good).
    bool Current(int page, int widthPx) const;

    // Bytes of images in the cache, pages rendered since the start, and renders abandoned
    // (tests).
    size_t CacheBytes() const;
    uint64_t RenderedCount() const;
    uint64_t CancelledCount() const;

private:
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
    };
    struct Job {
        std::shared_ptr<const pdf::PdfFile> pdf;
        int page = 0;
        int widthPx = 0;
        pdf::PdfHash key;
        int generation = 0;
    };
    struct CacheKey {
        pdf::PdfHash pageKey;
        int widthPx = 0;
        bool operator==(const CacheKey& o) const { return pageKey == o.pageKey && widthPx == o.widthPx; }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return pdf::PdfHashHasher()(k.pageKey) ^
                   static_cast<size_t>(static_cast<uint64_t>(static_cast<uint32_t>(k.widthPx)) * 0x9E3779B97F4A7C15ull);
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
    std::vector<InFlight> inFlight_;
    std::list<CacheEntry> lru_;   // most recent first
    std::unordered_map<CacheKey, std::list<CacheEntry>::iterator, CacheKeyHash> cache_;
    size_t cacheBytes_ = 0;
    uint64_t rendered_ = 0;
    uint64_t cancelled_ = 0;
    std::vector<std::thread> threads_;
};

} // namespace rgui
