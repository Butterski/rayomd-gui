// SPDX-License-Identifier: Apache-2.0
#include "platform/win32/frame_pacer.h"

#include <dwmapi.h>

#include <algorithm>

namespace rgui::win32 {

int64_t NextTick(int64_t now, int64_t phase, int64_t period) {
    if (period <= 0) return now;
    const int64_t since = now - phase;
    int64_t ticks = since / period;   // toward zero: already the ceiling when `since` < 0
    if (since > 0 && since % period != 0) ticks++;
    return phase + ticks * period;
}

FramePacer::FramePacer() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    frequency_ = frequency.QuadPart > 0 ? frequency.QuadPart : 10000000;
    period_ = frequency_ / 60;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    highResolution_ = timer_ != nullptr;
    // Before Windows 10 1803 the flag is refused: an ordinary timer, due on the clock's ticks.
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
}

FramePacer::~FramePacer() {
    if (timer_) CloseHandle(timer_);
}

bool FramePacer::Schedule() {
    if (!timer_) return false;
    if (scheduled_) return true;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const int64_t now = counter.QuadPart;
    // DWM's composition clock: its refresh period and the time of a vertical blank (asked each
    // time: the display, and with it the rate, can change). Without it (no composition), 60 Hz
    // from the previous frame's tick.
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    dwmTiming_ = DwmGetCompositionTimingInfo(nullptr, &timing) == S_OK &&
                 static_cast<int64_t>(timing.qpcRefreshPeriod) >= frequency_ / 1000 &&
                 static_cast<int64_t>(timing.qpcRefreshPeriod) <= frequency_ / 20;
    if (dwmTiming_) {
        period_ = static_cast<int64_t>(timing.qpcRefreshPeriod);
        phase_ = static_cast<int64_t>(timing.qpcVBlank);
    } else {
        period_ = frequency_ / 60;
        if (phase_ == 0) phase_ = now;
    }
    const int64_t due = NextTick(now, phase_, period_);
    if (!dwmTiming_) phase_ = due;
    // Relative, in 100 ns units (negative), at least one.
    LARGE_INTEGER relative{};
    relative.QuadPart = -std::max<int64_t>(1, (due - now) * 10000000 / frequency_);
    if (!SetWaitableTimer(timer_, &relative, 0, nullptr, nullptr, FALSE)) return false;
    scheduled_ = true;
    return true;
}

} // namespace rgui::win32
