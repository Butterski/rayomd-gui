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

} // namespace

int PreviewPages::DefaultThreads() {
    const int n = static_cast<int>(std::thread::hardware_concurrency()) / 2;
    return std::min(std::max(n, 1), 3);
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
        // The image the page showed in the previous build stays until its own is ready.
        if (i < pages_.size()) {
            pages[i].image = pages_[i].image;
            pages[i].imageKey = pages_[i].imageKey;
            pages[i].imageWidth = pages_[i].imageWidth;
        }
    }
    pages_ = std::move(pages);
    pdf_ = std::move(pdf);
    generation_ = generation;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void PreviewPages::Assign(PageSlot& slot, const CacheKey& key, std::shared_ptr<const gfx::Image> image) {
    if (image) {
        slot.image = std::move(image);
        slot.imageKey = key.pageKey;
        slot.imageWidth = key.widthPx;
        slot.failed = false;
    } else {
        slot.failed = true;
        slot.failedWidth = key.widthPx;
    }
}

void PreviewPages::Request(const std::vector<int>& pages, int widthPx) {
    if (!pdf_ || widthPx < 1) return;
    wantedWidth_ = widthPx;
    for (const int page : pages) {
        if (page < 0 || page >= PageCount()) continue;
        PageSlot& slot = pages_[static_cast<size_t>(page)];
        if (!slot.keyKnown) {
            slot.key = pdf_->PageKey(page);
            slot.keyKnown = true;
        }
    }
    std::vector<Job> jobs;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const int page : pages) {
            if (page < 0 || page >= PageCount() || Current(page, widthPx)) continue;
            PageSlot& slot = pages_[static_cast<size_t>(page)];
            const CacheKey key{slot.key, widthPx};
            std::shared_ptr<const gfx::Image> image;
            if (Lookup(key, image)) {
                Assign(slot, key, std::move(image));
                continue;
            }
            if (inFlight_.count(key) != 0) continue;
            bool queued = false;
            for (const Job& job : jobs) queued |= job.key == key.pageKey && job.widthPx == widthPx;
            if (!queued) jobs.push_back({pdf_, page, widthPx, slot.key});
        }
        queue_ = std::move(jobs);
        wake = !queue_.empty();
    }
    if (wake) wake_.notify_all();
}

const gfx::Image* PreviewPages::Image(int page) const {
    if (page < 0 || page >= PageCount()) return nullptr;
    return pages_[static_cast<size_t>(page)].image.get();
}

bool PreviewPages::Current(int page, int widthPx) const {
    if (page < 0 || page >= PageCount()) return false;
    const PageSlot& slot = pages_[static_cast<size_t>(page)];
    if (!slot.keyKnown) return false;
    if (slot.image && slot.imageKey == slot.key && slot.imageWidth == widthPx) return true;
    return slot.failed && slot.failedWidth == widthPx;
}

size_t PreviewPages::CacheBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cacheBytes_;
}

uint64_t PreviewPages::RenderedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rendered_;
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
    const pdf::RenderEnvironment environment{fontIndex_, &pdf::SharedImageCache(), &pdf::SharedFontCache()};
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.erase(queue_.begin());
            inFlight_.insert({job.key, job.widthPx});
        }
        auto image = std::make_shared<gfx::Image>();
        const bool ok = pdf::RenderPage(*job.pdf, job.page, job.widthPx, *image, nullptr, environment);
        std::shared_ptr<const gfx::Image> result;
        if (ok) {
            image->id = g_nextImageId.fetch_add(1, std::memory_order_relaxed);
            image->version = 1;
            result = std::move(image);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const CacheKey key{job.key, job.widthPx};
            inFlight_.erase(key);
            Insert(key, result);
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
    // Every page of the shown build with this content gets it, unless it already shows its
    // image at the width wanted now (a late result of an older width).
    for (PageSlot& slot : pages_) {
        if (!slot.keyKnown || slot.key != job.key) continue;
        if (slot.image && slot.imageKey == slot.key && slot.imageWidth == wantedWidth_) continue;
        Assign(slot, {job.key, job.widthPx}, image);
        changed = true;
    }
    if (changed && onPageReady_) onPageReady_();
}

} // namespace rgui
