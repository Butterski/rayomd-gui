// SPDX-License-Identifier: Apache-2.0
// Animation frames at the display's refresh rate. A WM_TIMER cannot pace them: SetTimer waits for
// the system clock's ticks, 15.6 ms apart, so a 16 ms timer fires every second tick, 31 ms (32
// frames a second, at no fixed phase of the display). FramePacer arms a high-resolution waitable
// timer (Windows 10 1803 and later; before, an ordinary one) for the next vertical blank of DWM's
// composition clock, and Win32Window's message loop waits for it and the messages together.
//
//     FramePacer pacer;
//     pacer.Schedule();                                  // during a frame that wants another
//     HANDLE timer = pacer.Timer();
//     if (MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
//         WAIT_OBJECT_0) { pacer.Fired(); /* the frame */ }
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

namespace rgui::win32 {

// The first of the ticks phase + k * period (any whole k) at or after `now`; `now` when
// period <= 0.
int64_t NextTick(int64_t now, int64_t phase, int64_t period);

class FramePacer {
public:
    FramePacer();
    ~FramePacer();
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // Null when no timer could be made: Schedule then fails and the caller falls back to WM_TIMER.
    HANDLE Timer() const { return timer_; }
    bool HighResolution() const { return highResolution_; }
    // Arms the timer for the next vertical blank, unless it is armed already.
    bool Schedule();
    bool Scheduled() const { return scheduled_; }
    // A wait returned the timer: it may be armed again.
    void Fired() { scheduled_ = false; }
    // The refresh period and the counter's frequency (QueryPerformanceCounter units): DWM's
    // period as of the last Schedule, 60 Hz while DWM has not told one.
    int64_t Period() const { return period_; }
    int64_t Frequency() const { return frequency_; }
    // Whether the last Schedule had DWM's clock (else it kept its own 60 Hz phase).
    bool DwmTiming() const { return dwmTiming_; }

private:
    HANDLE timer_ = nullptr;
    bool highResolution_ = false;
    bool scheduled_ = false;
    bool dwmTiming_ = false;
    int64_t frequency_ = 0;
    int64_t period_ = 0;
    int64_t phase_ = 0;
};

} // namespace rgui::win32
