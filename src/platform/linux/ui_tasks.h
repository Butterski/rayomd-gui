// SPDX-License-Identifier: Apache-2.0
// Platform::PostToUi on Linux: tasks queued under a mutex by any thread, run on the UI thread by
// the event loop after rgui_loop_wakeup.
#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace rgui::wl {

class UiTaskQueue {
public:
    // Becomes the loop's wakeup handler (rgui_loop_init must have run).
    UiTaskQueue();
    ~UiTaskQueue();
    UiTaskQueue(const UiTaskQueue&) = delete;
    UiTaskQueue& operator=(const UiTaskQueue&) = delete;

    // Any thread.
    void Post(std::function<void()> task);
    // UI thread: runs every task queued so far, in order. Tasks posted meanwhile wait for the
    // next wakeup.
    void RunPending();

private:
    std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
    std::vector<std::function<void()>> running_;
};

} // namespace rgui::wl
