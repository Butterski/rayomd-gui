// SPDX-License-Identifier: Apache-2.0
#include "headless_platform.h"

#include "runner.h"

#include <algorithm>
#include <chrono>

namespace rgui::test {

HeadlessPlatform::HeadlessPlatform() = default;

void HeadlessPlatform::Present(const gfx::Surface& frame, const std::vector<gfx::RectI>& damage) {
    // As the Wayland window does: the damaged rectangles only, the whole frame when its size changed.
    const size_t pixels = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    const bool whole = lastFrameWidth != frame.width || lastFrameHeight != frame.height || lastFrame.size() != pixels;
    lastFrameWidth = frame.width;
    lastFrameHeight = frame.height;
    lastFrame.resize(pixels);
    const gfx::RectI bounds{0, 0, frame.width, frame.height};
    const auto copy = [&](const gfx::RectI& r) {
        const gfx::RectI c = r.Intersect(bounds);
        if (c.Empty()) return;
        for (int y = c.y0; y < c.y1; y++) {
            std::copy_n(frame.Row(y) + c.x0, c.Width(),
                        lastFrame.data() + static_cast<size_t>(y) * static_cast<size_t>(frame.width) + c.x0);
        }
    };
    if (whole) {
        copy(bounds);
    } else {
        for (const gfx::RectI& r : damage) copy(r);
    }
    lastDamage = damage;
    presents++;
}

void HeadlessPlatform::RequestClipboardText(std::function<void(std::string)> callback) {
    PostToUi([this, callback = std::move(callback)] { callback(clipboard); });
}

void HeadlessPlatform::ShowFileDialog(FileDialog kind, std::string suggestedPath,
                                      std::function<void(FileDialogResult)> callback) {
    dialogs.push_back({kind, suggestedPath});
    FileDialogResult answer;
    if (!dialogAnswers_.empty()) {
        answer = std::move(dialogAnswers_.front());
        dialogAnswers_.pop_front();
    }
    PostToUi([callback = std::move(callback), answer = std::move(answer)]() mutable {
        callback(std::move(answer));
    });
}

void HeadlessPlatform::PostToUi(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    posted_.notify_all();
}

uint64_t HeadlessPlatform::StartTimer(int ms, std::function<void()> callback) {
    Tick();
    const uint64_t id = nextTimerId_++;
    timers_.push_back({now_ + std::max(ms, 0), id, std::move(callback)});
    return id;
}

void HeadlessPlatform::CancelTimer(uint64_t id) {
    const auto it = Find(id);
    if (it != timers_.end()) timers_.erase(it);
}

std::vector<HeadlessPlatform::Timer>::iterator HeadlessPlatform::Find(uint64_t id) {
    return std::find_if(timers_.begin(), timers_.end(), [id](const Timer& t) { return t.id == id; });
}

int HeadlessPlatform::RunPosted() {
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(tasks_);
    }
    for (auto& task : batch) task();
    return static_cast<int>(batch.size());
}

bool HeadlessPlatform::RunNextTimer(int64_t until) {
    // Timers are kept in start order, so the first of the smallest deadline started first.
    auto next = timers_.end();
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->deadline <= until && (next == timers_.end() || it->deadline < next->deadline)) next = it;
    }
    if (next == timers_.end()) return false;
    if (next->deadline > now_) now_ = next->deadline;
    std::function<void()> callback = std::move(next->callback);
    timers_.erase(next);
    callback();
    return true;
}

int HeadlessPlatform::Pump() {
    int ran = 0;
    for (;;) {
        Tick();
        int round = RunPosted();
        while (RunNextTimer(now_)) {
            round++;
            round += RunPosted();
        }
        if (round == 0) return ran;
        ran += round;
    }
}

int HeadlessPlatform::Advance(int ms) {
    if (realClock_) return Pump();
    const int64_t target = now_ + std::max(ms, 0);
    int ran = Pump();
    while (RunNextTimer(target)) ran += 1 + Pump();
    now_ = target;
    return ran + Pump();
}

bool HeadlessPlatform::PumpUntil(const std::function<bool()>& done, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        Pump();
        if (done()) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        WaitForWork(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()) + 1);
    }
}

bool HeadlessPlatform::WaitForWork(int timeoutMs) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
    const auto timer = NextDeadline();
    if (timer < until) until = timer;
    std::unique_lock<std::mutex> lock(mutex_);
    posted_.wait_until(lock, until, [this] { return !tasks_.empty(); });
    return !tasks_.empty() || std::chrono::steady_clock::now() >= timer;
}

void HeadlessPlatform::UseRealClock() {
    if (realClock_) return;
    origin_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(now_);
    realClock_ = true;
}

double HeadlessPlatform::RealNowMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin_).count();
}

void HeadlessPlatform::Tick() {
    if (!realClock_) return;
    const int64_t real = static_cast<int64_t>(RealNowMs());
    if (real > now_) now_ = real;
}

std::chrono::steady_clock::time_point HeadlessPlatform::NextDeadline() const {
    if (!realClock_ || timers_.empty()) return std::chrono::steady_clock::time_point::max();
    int64_t first = timers_.front().deadline;
    for (const Timer& timer : timers_) first = std::min(first, timer.deadline);
    return origin_ + std::chrono::milliseconds(first);
}

std::filesystem::path HeadlessPlatform::ConfigDir() const {
    return std::filesystem::u8path(BuildDir()) / "test-config";
}

std::filesystem::path HeadlessPlatform::ImageCacheDir() const {
    return std::filesystem::u8path(BuildDir()) / "test-cache";
}

std::filesystem::path HeadlessPlatform::DocumentsDir() const {
    return std::filesystem::u8path(BuildDir()) / "test-documents";
}

} // namespace rgui::test
