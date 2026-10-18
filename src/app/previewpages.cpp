// SPDX-License-Identifier: Apache-2.0
#include "app/previewpages.h"

#include "app/platform.h"
#include "pdf/fonts.h"
#include "pdf/images.h"
#include "pdf/reader.h"
#include "pdf/renderer.h"

#include <algorithm>
#include <atomic>

namespace rgui {

namespace {

// Every rendered image gets an id of its own, so the compositor redraws the tiles of a page whose
// image changed (display lists hash an image's id and version, not its pixels).
std::atomic<uint64_t> g_nextImageId{1};

size_t ImageBytes(const gfx::Image* image) {
    return (image ? image->pixels.size() * sizeof(uint32_t) : 0) + 64;
}

// The strips of `count` that hold rows [top, bottom) (top == bottom: row `top`), clamped.
void StripRange(int top, int bottom, int count, int& first, int& last) {
    constexpr int kRows = PreviewPages::kStripRows;
    first = std::min(std::max(top, 0) / kRows, count - 1);
    last = std::max(first, std::min((std::max(bottom, 1) - 1) / kRows, count - 1));
}

} // namespace

int PreviewPages::DefaultThreads() {
    const int n = static_cast<int>(std::thread::hardware_concurrency()) / 2;
    return std::min(std::max(n, 1), 3);
}

int PreviewPages::StripCount(double widthPt, double heightPt, int widthPx) {
    const int height = pdf::RenderHeight(widthPt, heightPt, widthPx);
    if (height <= kStripRows) return 0;
    const bool whole = height <= pdf::kMaxRenderSide &&
                       static_cast<uint64_t>(widthPx) * static_cast<uint64_t>(height) <= pdf::kMaxRenderPixels;
    if (whole && height <= 2 * static_cast<int64_t>(widthPx)) return 0;
    return (height + kStripRows - 1) / kStripRows;
}

PreviewPages::PreviewPages(Platform& platform, const text::FontIndex* fontIndex, std::function<void()> onPageReady,
                           int threads)
    : platform_(platform), fontIndex_(fontIndex), onPageReady_(std::move(onPageReady)),
      link_(std::make_shared<Link>()) {
    link_->owner = this;
    if (threads < 1) threads = DefaultThreads();
    threads_.reserve(static_cast<size_t>(threads));
    for (int i = 0; i < threads; i++) threads_.emplace_back([this] { Run(); });
}

PreviewPages::~PreviewPages() {
    link_->alive = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        queue_.clear();
        for (const InFlight& running : inFlight_) running.cancel->store(true, std::memory_order_relaxed);
    }
    wake_.notify_all();
    for (std::thread& thread : threads_) thread.join();
}

void PreviewPages::SetDocument(std::shared_ptr<const pdf::PdfFile> pdf, int generation) {
    std::vector<PageSlot> pages(pdf ? static_cast<size_t>(pdf->PageCount()) : 0);
    for (size_t i = 0; i < pages.size(); i++) {
        const pdf::PdfRect box = pdf->PageMediaBox(static_cast<int>(i));
        if (box.Width() > 0 && box.Height() > 0) {
            pages[i].widthPt = box.Width();
            pages[i].heightPt = box.Height();
        }
        // The image the page showed in the previous build stays until its own is ready; so do
        // its strips, which keep their rows: they count from the page's top edge.
        if (i < pages_.size()) {
            PageSlot& old = pages_[i];
            pages[i].image = std::move(old.image);
            pages[i].imageKey = old.imageKey;
            pages[i].imageWidth = old.imageWidth;
            pages[i].stripWidth = old.stripWidth;
            pages[i].strips = std::move(old.strips);
            pages[i].under = std::move(old.under);
            for (StripSlot& strip : pages[i].strips) strip.failed = false;
        }
    }
    pages_ = std::move(pages);
    pdf_ = std::move(pdf);
    generation_ = generation;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

int PreviewPages::StripCount(int page, int widthPx) const {
    if (page < 0 || page >= PageCount()) return 0;
    const PageSlot& slot = pages_[static_cast<size_t>(page)];
    return StripCount(slot.widthPt, slot.heightPt, widthPx);
}

void PreviewPages::Assign(PageSlot& slot, const CacheKey& key, std::shared_ptr<const gfx::Image> image) {
    if (key.strip >= 0) {
        if (slot.stripWidth != key.widthPx || static_cast<size_t>(key.strip) >= slot.strips.size()) return;
        StripSlot& strip = slot.strips[static_cast<size_t>(key.strip)];
        if (image) {
            strip.image = std::move(image);
            strip.imageKey = key.pageKey;
            strip.imageWidth = key.widthPx;
            strip.top = key.strip * kStripRows;
            strip.failed = false;
        } else {
            strip.failed = true;
        }
        return;
    }
    if (image) {
        slot.image = std::move(image);
        slot.imageKey = key.pageKey;
        slot.imageWidth = key.widthPx;
        slot.failed = false;
        // The whole page covers whatever strips it showed.
        slot.stripWidth = 0;
        slot.strips.clear();
        slot.under.clear();
    } else {
        slot.failed = true;
        slot.failedWidth = key.widthPx;
    }
}

bool PreviewPages::StripCurrent(const PageSlot& slot, int strip) {
    const StripSlot& s = slot.strips[static_cast<size_t>(strip)];
    return (s.image && s.imageKey == slot.key && s.imageWidth == slot.stripWidth) || s.failed;
}

void PreviewPages::LayOutStrips(PageSlot& slot, int widthPx, int count) {
    if (slot.stripWidth != widthPx) {
        std::vector<StripSlot> shown;
        for (StripSlot& strip : slot.strips) {
            if (strip.image) shown.push_back(std::move(strip));
        }
        if (!shown.empty()) slot.under = std::move(shown);
        slot.strips.clear();
        slot.stripWidth = widthPx;
    }
    // At the same width a strip keeps its rows when the page's height changes.
    slot.strips.resize(static_cast<size_t>(count));
}

bool PreviewPages::WantPiece(int page, int widthPx, int strip, std::vector<Job>& jobs, std::vector<CacheKey>& wanted) {
    PageSlot& slot = pages_[static_cast<size_t>(page)];
    const CacheKey key{slot.key, widthPx, strip};
    wanted.push_back(key);
    if (strip < 0 ? Current(page, widthPx) : StripCurrent(slot, strip)) return false;
    std::shared_ptr<const gfx::Image> image;
    if (Lookup(key, image)) {
        Assign(slot, key, std::move(image));
        return true;
    }
    for (const InFlight& running : inFlight_) {
        if (running.key == key && !running.cancel->load()) return false;
    }
    for (const Job& job : jobs) {
        if (job.key == key.pageKey && job.widthPx == widthPx && job.strip == strip) return false;
    }
    if (strip >= 0 && !slot.bands) slot.bands = std::make_shared<pdf::BandIndex>();
    jobs.push_back({pdf_, page, widthPx, strip, slot.key, generation_, strip >= 0 ? slot.bands : nullptr});
    return false;
}

bool PreviewPages::Request(const std::vector<Want>& pages, int widthPx) {
    if (!pdf_ || widthPx < 1) return false;
    wantedWidth_ = widthPx;
    for (const Want& want : pages) {
        if (want.page < 0 || want.page >= PageCount()) continue;
        PageSlot& slot = pages_[static_cast<size_t>(want.page)];
        if (!slot.keyKnown) {
            slot.key = pdf_->PageKey(want.page);
            slot.keyKnown = true;
        }
    }
    std::vector<Job> jobs;
    std::vector<CacheKey> wanted;
    // The strips next to those in view: after everything in view.
    std::vector<std::pair<int, int>> later;
    bool wake = false, assigned = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Want& want : pages) {
            if (want.page < 0 || want.page >= PageCount()) continue;
            PageSlot& slot = pages_[static_cast<size_t>(want.page)];
            const int count = StripCount(slot.widthPt, slot.heightPt, widthPx);
            if (count == 0) {
                assigned |= WantPiece(want.page, widthPx, -1, jobs, wanted);
                continue;
            }
            LayOutStrips(slot, widthPx, count);
            int first = 0, last = 0;
            StripRange(want.top, want.bottom, count, first, last);
            // The strip nearest the middle of the rows in view first.
            const int64_t middle = (int64_t(std::max(want.top, 0)) + std::max(want.bottom, want.top)) / 2;
            const int center = static_cast<int>(std::min<int64_t>(std::max<int64_t>(middle / kStripRows, first), last));
            for (int d = 0; center - d >= first || center + d <= last; d++) {
                if (center + d <= last) assigned |= WantPiece(want.page, widthPx, center + d, jobs, wanted);
                if (d > 0 && center - d >= first) assigned |= WantPiece(want.page, widthPx, center - d, jobs, wanted);
            }
            if (want.bottom > want.top) {
                if (first > 0) later.push_back({want.page, first - 1});
                if (last + 1 < count) later.push_back({want.page, last + 1});
            }
            // Once the strips in view are current, nothing of another width or build lies under them.
            bool current = true;
            for (int s = first; s <= last && current; s++) current = StripCurrent(slot, s);
            if (current) {
                slot.image.reset();
                slot.under.clear();
            }
            // The page holds the strips near the view only; the others stay in the cache while it
            // keeps them.
            for (int s = 0; s < count; s++) {
                if (s < first - 2 || s > last + 2) slot.strips[static_cast<size_t>(s)] = StripSlot();
            }
        }
        for (const auto& [page, strip] : later) assigned |= WantPiece(page, widthPx, strip, jobs, wanted);
        // An older build's render nobody wants any more is abandoned.
        for (const InFlight& running : inFlight_) {
            if (running.generation >= generation_) continue;
            bool stillWanted = false;
            for (const CacheKey& key : wanted) stillWanted |= key == running.key;
            if (!stillWanted) running.cancel->store(true, std::memory_order_relaxed);
        }
        queue_ = std::move(jobs);
        wake = !queue_.empty();
    }
    if (wake) wake_.notify_all();
    return assigned;
}

const gfx::Image* PreviewPages::Image(int page) const {
    if (page < 0 || page >= PageCount()) return nullptr;
    return pages_[static_cast<size_t>(page)].image.get();
}

void PreviewPages::Strips(int page, std::vector<Strip>& out) const {
    out.clear();
    if (page < 0 || page >= PageCount()) return;
    const PageSlot& slot = pages_[static_cast<size_t>(page)];
    for (const std::vector<StripSlot>* list : {&slot.under, &slot.strips}) {
        for (const StripSlot& strip : *list) {
            if (strip.image) out.push_back({strip.image.get(), strip.imageWidth, strip.top});
        }
    }
}

bool PreviewPages::Current(int page, int widthPx, int top, int bottom) const {
    if (page < 0 || page >= PageCount()) return false;
    const PageSlot& slot = pages_[static_cast<size_t>(page)];
    if (!slot.keyKnown) return false;
    const int count = StripCount(slot.widthPt, slot.heightPt, widthPx);
    if (count == 0) {
        if (slot.image && slot.imageKey == slot.key && slot.imageWidth == widthPx) return true;
        return slot.failed && slot.failedWidth == widthPx;
    }
    if (slot.stripWidth != widthPx || slot.strips.size() != static_cast<size_t>(count)) return false;
    int first = 0, last = 0;
    StripRange(top, bottom, count, first, last);
    for (int s = first; s <= last; s++) {
        if (!StripCurrent(slot, s)) return false;
    }
    return true;
}

size_t PreviewPages::CacheBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cacheBytes_;
}

uint64_t PreviewPages::RenderedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rendered_;
}

uint64_t PreviewPages::CancelledCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
}

bool PreviewPages::Lookup(const CacheKey& key, std::shared_ptr<const gfx::Image>& image) {
    const auto found = cache_.find(key);
    if (found == cache_.end()) return false;
    lru_.splice(lru_.begin(), lru_, found->second);
    image = found->second->image;
    return true;
}

void PreviewPages::Insert(const CacheKey& key, std::shared_ptr<const gfx::Image> image) {
    const auto found = cache_.find(key);
    if (found != cache_.end()) {
        cacheBytes_ -= found->second->bytes;
        lru_.erase(found->second);
        cache_.erase(found);
    }
    const size_t bytes = ImageBytes(image.get());
    lru_.push_front({key, std::move(image), bytes});
    cache_[key] = lru_.begin();
    cacheBytes_ += bytes;
    // Least recently used first; the newest entry stays even when it alone is over the budget.
    while (cacheBytes_ > kCacheBudget && lru_.size() > 1) {
        const CacheEntry& last = lru_.back();
        cacheBytes_ -= last.bytes;
        cache_.erase(last.key);
        lru_.pop_back();
    }
}

void PreviewPages::Run() {
    for (;;) {
        Job job;
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.erase(queue_.begin());
            inFlight_.push_back({{job.key, job.widthPx, job.strip}, job.generation, cancel});
        }
        pdf::RenderEnvironment environment{fontIndex_, &pdf::SharedImageCache(), &pdf::SharedFontCache()};
        environment.cancel = cancel.get();
        auto image = std::make_shared<gfx::Image>();
        pdf::RenderStats stats;
        bool ok = false;
        if (job.strip < 0) {
            ok = pdf::RenderPage(*job.pdf, job.page, job.widthPx, *image, &stats, environment);
        } else {
            const pdf::PdfRect box = job.pdf->PageMediaBox(job.page);
            const int height = pdf::RenderHeight(box.Width(), box.Height(), job.widthPx);
            const int top = job.strip * kStripRows;
            ok = top < height && pdf::RenderPageRows(*job.pdf, job.page, job.widthPx, top,
                                                     std::min(kStripRows, height - top), *image, &stats, environment,
                                                     job.bands.get());
        }
        std::shared_ptr<const gfx::Image> result;
        if (ok) {
            image->id = g_nextImageId.fetch_add(1, std::memory_order_relaxed);
            image->version = 1;
            result = std::move(image);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t i = 0; i < inFlight_.size(); i++) {
                if (inFlight_[i].cancel == cancel) {
                    inFlight_.erase(inFlight_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            if (stats.cancelled) {
                // Abandoned: nothing to keep or show; a later Request queues it again if wanted.
                cancelled_++;
                continue;
            }
            Insert({job.key, job.widthPx, job.strip}, result);
            rendered_++;
        }
        job.pdf.reset();
        platform_.PostToUi([link = link_, job = std::move(job), result = std::move(result)] {
            if (link->alive) link->owner->Delivered(job, result);
        });
    }
}

void PreviewPages::Delivered(const Job& job, const std::shared_ptr<const gfx::Image>& image) {
    bool changed = false;
    for (PageSlot& slot : pages_) {
        if (!slot.keyKnown || slot.key != job.key) continue;
        if (job.strip < 0) {
            if (slot.image && slot.imageKey == slot.key && slot.imageWidth == wantedWidth_) continue;
            // A late page of another width does not replace the strips of the width wanted now.
            if (job.widthPx != wantedWidth_ && slot.stripWidth == wantedWidth_ && !slot.strips.empty()) continue;
        } else if (slot.stripWidth != job.widthPx || static_cast<size_t>(job.strip) >= slot.strips.size() ||
                   StripCurrent(slot, job.strip)) {
            continue;
        }
        Assign(slot, {job.key, job.widthPx, job.strip}, image);
        changed = true;
    }
    if (changed && onPageReady_) onPageReady_();
}

} // namespace rgui
