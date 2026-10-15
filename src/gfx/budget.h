// SPDX-License-Identifier: Apache-2.0
// Work limits of one drawing job (a preview page): what the canvas, the rasterizer and the stroker
// may still spend, and a flag another thread sets to abandon the job. Once a limit runs out or
// the flag is seen, the job is stopped: every later drawing call of the job does nothing, and the
// caller learns why from `stop`. Not synchronized: one job, one thread (the flag excepted).
#pragma once

#include <atomic>
#include <cstdint>

namespace rgui::gfx {

struct WorkBudget {
    enum class Stop : uint8_t {
        None,
        Cancelled,  // the cancel flag was set
        Geometry,   // flattened segments and outline points
        Pixels,     // covered pixels
        Dashes,     // dashes of dashed strokes
    };

    static constexpr uint64_t kUnlimited = ~uint64_t(0);

    // Set to true by another thread to abandon the job; nullptr: never.
    const std::atomic<bool>* cancel = nullptr;
    // What is left of each limit.
    uint64_t geometry = kUnlimited;  // line segments handed to a rasterizer, polygon points a stroker emits
    uint64_t pixels = kUnlimited;    // pixels a rasterizer sweeps or a fill, mask or image copy writes
    uint64_t dashes = kUnlimited;    // dashes strokes make
    Stop stop = Stop::None;

    bool Stopped() const { return stop != Stop::None; }

    // Takes `n` from `left`; false, and stopped for `why`, when not that much is left.
    bool Take(uint64_t& left, uint64_t n, Stop why) {
        if (stop != Stop::None) return false;
        if (n > left) {
            left = 0;
            stop = why;
            return false;
        }
        left -= n;
        return true;
    }

    // Looks at the cancel flag; false once the job is stopped for any reason.
    bool Poll() {
        if (stop == Stop::None && cancel && cancel->load(std::memory_order_relaxed)) stop = Stop::Cancelled;
        return stop == Stop::None;
    }
};

} // namespace rgui::gfx
