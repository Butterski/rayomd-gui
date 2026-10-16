// SPDX-License-Identifier: Apache-2.0
// bench.scroll: what a frame costs while the preview and the editor scroll, the whole app at
// 1788 x 1097 (a large window) on the real clock and threads. A wheel notch every 50 ms for two
// seconds, frames every 16.7 ms as a 60 Hz display asks for them; each frame's time is the UI
// thread's: input, layout, the display list, the compositor's raster of the tiles that changed and
// the present. Also counts the frames that showed a page of the preview without its current image.
#include "app_harness.h"
#include "headless_platform.h"
#include "runner.h"

#include "common/file_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RGUI_HAS_ASAN 1
#endif
#endif

namespace {

// Sanitized builds check the path, not the time (and run it in rayomd-gui-unit).
#if defined(__SANITIZE_ADDRESS__) || defined(RGUI_HAS_ASAN)
constexpr int kFrames = 20;
#else
constexpr int kFrames = 120;
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double MsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double Percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    size_t rank = static_cast<size_t>(p * static_cast<double>(values.size()) + 0.999999);
    if (rank < 1) rank = 1;
    return values[std::min(rank, values.size()) - 1];
}

struct ScrollResult {
    std::vector<double> frameMs;
    int unsettledFrames = 0;
};

// Two seconds of wheel notches at (x, y), a frame each 1/60 s (kFrames frames).
ScrollResult Scroll(rgui::test::AppHarness& h, float x, float y, float notches) {
    ScrollResult result;
    rgui::InputEvent move;
    move.type = rgui::InputEvent::Type::PointerMove;
    move.x = x;
    move.y = y;
    h.app->OnInput(move);
    h.Frame();
    const auto start = Clock::now();
    auto nextFrame = start;
    double nextNotch = 0;
    for (int frame = 0; frame < kFrames; frame++) {
        const double elapsed = MsBetween(start, Clock::now());
        if (elapsed >= nextNotch && elapsed < 2000) {
            rgui::InputEvent wheel;
            wheel.type = rgui::InputEvent::Type::Wheel;
            wheel.x = x;
            wheel.y = y;
            wheel.notchesY = notches;
            h.app->OnInput(wheel);
            nextNotch += 50;
        }
        const auto t0 = Clock::now();
        h.Frame();
        result.frameMs.push_back(MsBetween(t0, Clock::now()));
        if (!h.Window().PreviewSettled()) result.unsettledFrames++;
        nextFrame += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(nextFrame);
    }
    return result;
}

} // namespace

RGUI_TEST("bench.scroll") {
    if (!rgui::test::HaveFonts()) {
        std::printf("skip bench.scroll: DejaVu Sans Mono not installed\n");
        return;
    }
    std::string tester;
    CHECK(RayoMd::File::ReadAll(fs::u8path(rgui::test::TesterPath()), 1u << 20, tester));
    if (tester.empty()) return;
    std::string text;
    for (int i = 0; i < 8; i++) text += tester;

    rgui::test::AppHarness h("scroll", "bench-scroll");
    const fs::path file = h.dir / "doc.md";
    CHECK(RayoMd::File::WriteAll(file, text));
    h.platform.UseRealClock();
    h.Start("ui.theme=tokyo-night\n", file.u8string());
    constexpr int kWidth = 1788, kHeight = 1097;
    h.app->OnResize(kWidth, kHeight, 1.0);
    if (!h.Settle(120000)) {
        CHECK(false);
        return;
    }
    const ScrollResult preview = Scroll(h, kWidth * 0.75f, kHeight * 0.5f, 1.0f);
    const ScrollResult editor = Scroll(h, kWidth * 0.25f, kHeight * 0.5f, 1.0f);
    CHECK(h.Window().PreviewScroll() > 0);
    std::printf("bench.scroll: %dx%d, %zu-byte document, %d pages, a notch every 50 ms, 60 frames a second:\n",
                kWidth, kHeight, text.size(), h.Window().Preview().PageCount());
    std::printf("  preview: frame p50 %.2f ms, p99 %.2f ms, max %.2f ms; %d of %zu frames with a page not yet drawn\n",
                Percentile(preview.frameMs, 0.5), Percentile(preview.frameMs, 0.99),
                Percentile(preview.frameMs, 1.0), preview.unsettledFrames, preview.frameMs.size());
    std::printf("  editor:  frame p50 %.2f ms, p99 %.2f ms, max %.2f ms\n", Percentile(editor.frameMs, 0.5),
                Percentile(editor.frameMs, 0.99), Percentile(editor.frameMs, 1.0));
}
