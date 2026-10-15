// SPDX-License-Identifier: Apache-2.0
#include "platform/linux/ui_tasks.h"

#include "platform/linux/loop.h"

namespace rgui::wl {

namespace {

void OnWakeup(void* user) { static_cast<UiTaskQueue*>(user)->RunPending(); }

} // namespace

UiTaskQueue::UiTaskQueue() { rgui_loop_set_wakeup_handler(OnWakeup, this); }

UiTaskQueue::~UiTaskQueue() { rgui_loop_set_wakeup_handler(nullptr, nullptr); }

void UiTaskQueue::Post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    rgui_loop_wakeup();
}

void UiTaskQueue::RunPending() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.swap(tasks_);
    }
    for (auto& task : running_) {
        if (task) task();
    }
    running_.clear();
}

} // namespace rgui::wl
