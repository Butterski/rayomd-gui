// SPDX-License-Identifier: Apache-2.0
// The Win32 frame pacing (src/platform/win32/frame_pacer.h): animation frames at the display's
// refresh, where a 16 ms WM_TIMER came every 31 ms (two ticks of the 15.6 ms system clock).
#include "runner.h"

#include "platform/win32/frame_pacer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

RGUI_TEST("win32.next_tick") {
    using rgui::win32::NextTick;
    CHECK_EQ(NextTick(100, 100, 10), 100);   // on a tick
    CHECK_EQ(NextTick(101, 100, 10), 110);
    CHECK_EQ(NextTick(110, 100, 10), 110);
    CHECK_EQ(NextTick(119, 100, 10), 120);
    CHECK_EQ(NextTick(95, 100, 10), 100);    // the phase after now
    CHECK_EQ(NextTick(75, 100, 10), 80);
    CHECK_EQ(NextTick(80, 100, 10), 80);
    CHECK_EQ(NextTick(7, 100, 0), 7);        // no period
}

// Sixty frames, each scheduled when the previous one fired: they come a refresh period apart.
RGUI_TEST("win32.frame_pacer") {
    rgui::win32::FramePacer pacer;
    CHECK(pacer.Timer() != nullptr);
    if (!pacer.Timer()) return;
    std::vector<double> intervals;
    LARGE_INTEGER last{};
    for (int i = 0; i <= 60; i++) {
        CHECK(pacer.Schedule());
        CHECK(pacer.Scheduled());
        CHECK(pacer.Schedule());   // armed once
        CHECK_EQ(WaitForSingleObject(pacer.Timer(), 1000), static_cast<DWORD>(WAIT_OBJECT_0));
        pacer.Fired();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (i > 0) intervals.push_back(static_cast<double>(now.QuadPart - last.QuadPart) * 1000.0 /
                                       static_cast<double>(pacer.Frequency()));
        last = now;
    }
    std::sort(intervals.begin(), intervals.end());
    const double period = static_cast<double>(pacer.Period()) * 1000.0 / static_cast<double>(pacer.Frequency());
    const double median = intervals[intervals.size() / 2];
    std::printf("win32.frame_pacer: %s timer, %s, period %.2f ms; frames apart: median %.2f ms, "
                "min %.2f ms, max %.2f ms\n",
                pacer.HighResolution() ? "high-resolution" : "ordinary",
                pacer.DwmTiming() ? "DWM's clock" : "no DWM clock (60 Hz)", period, median, intervals.front(),
                intervals.back());
    CHECK(period > 1.0 && period < 50.0);
    // The ordinary timer (before Windows 10 1803) waits for the clock's ticks: no bound to check.
    if (pacer.HighResolution()) CHECK(median > period * 0.75 && median < period * 1.25);
}
