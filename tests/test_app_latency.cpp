// SPDX-License-Identifier: Apache-2.0
// bench.preview_latency: how long a keystroke takes to reach the preview, with the real engine,
// the real worker threads and the real clock. One character is typed into the title of
// documents of 1, 8, 70 and 700 copies of tester.md; each sample runs from the key event to the
// frame in which the preview shows the build of the new text with every visible page (the
// edited page among them) current. It also sums the UI thread's own time per keystroke (input,
// frames, posted results; not the time it waits).
#include "app_harness.h"
#include "headless_platform.h"
#include "runner.h"

#include "common/file_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RGUI_HAS_ASAN 1
#endif
#endif

namespace {

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

struct Row {
    int copies;
    int keystrokes;
};

// Measured on the base commit a55b638 (the fixed 80 ms debounce, then the build), release,
// rgui-arch, the same machine and method: copies, p50, p99 (keystroke -> preview current, ms),
// UI thread per keystroke p50, p99 (ms).
struct Before {
    int copies;
    double p50, p99, uiP50, uiP99;
};
constexpr Before kBefore[] = {
    {1, 83.8, 85.4, 3.01, 3.89},
    {8, 86.2, 87.1, 3.31, 4.23},
    {70, 103.1, 105.6, 3.91, 7.70},
    {700, 272.1, 289.4, 11.00, 14.62},
};
// Quitting with the 700 copies' preview build in flight, on the base commit (ms; 238 and 292 in
// two runs: the destructor waited for the build to end).
constexpr double kQuitBefore = 265;

RGUI_TEST("bench.preview_latency") {
    if (!rgui::test::HaveFonts()) {
        std::printf("skip bench.preview_latency: DejaVu Sans Mono not installed\n");
        return;
    }
    std::string tester;
    CHECK(RayoMd::File::ReadAll(fs::u8path(rgui::test::TesterPath()), 1u << 20, tester));
#if defined(__SANITIZE_ADDRESS__) || defined(RGUI_HAS_ASAN)
    // Sanitized builds check the path, not the time (and run it in rayomd-gui-unit).
    const Row rows[] = {{1, 5}, {8, 5}};
#else
    const Row rows[] = {{1, 40}, {8, 40}, {70, 20}, {700, 10}};
#endif
    std::printf("bench.preview_latency: keystroke -> edited page's preview current (real clock, real threads)\n");
    std::printf("  %7s %10s %6s %4s | %21s | %21s | %25s\n", "copies", "bytes", "pages", "n", "before p50 / p99 ms",
                "after p50 / p99 ms", "UI ms/key before / after");
    for (const Row& row : rows) {
        const std::string name = "copies-" + std::to_string(row.copies);
        rgui::test::AppHarness h(name.c_str(), "bench-preview-latency");
        std::string text;
        text.reserve(tester.size() * static_cast<size_t>(row.copies));
        for (int i = 0; i < row.copies; i++) text += tester;
        const fs::path file = h.dir / "doc.md";
        CHECK(RayoMd::File::WriteAll(file, text));
        h.platform.UseRealClock();
        h.Start("ui.theme=tokyo-night\n", file.u8string());
        if (!h.Settle(120000)) {
            CHECK(false);
            return;
        }
        rgui::MainWindow& w = h.Window();
        // The end of the title, `# RayoMD Test Report`, on page 1.
        const size_t title = text.find('\n');
        w.Editor().Select(title, title);

        std::vector<double> latency, uiTime;
        for (int k = 0; k < row.keystrokes; k++) {
            const auto start = Clock::now();
            double busy = 0;
            rgui::InputEvent key;
            key.type = rgui::InputEvent::Type::Text;
            key.text = "x";
            h.app->OnInput(key);
            h.Frame();
            busy += MsBetween(start, Clock::now());
            bool settled = false;
            while (!(settled = w.PreviewSettled() && w.ShownGeneration() == w.RequestedGeneration())) {
                if (MsBetween(start, Clock::now()) > 60000) break;
                if (h.platform.frameRequests == h.framesSeen) h.platform.WaitForWork(100);
                const auto stepStart = Clock::now();
                h.Step();
                busy += MsBetween(stepStart, Clock::now());
            }
            CHECK(settled);
            if (!settled) return;
            latency.push_back(MsBetween(start, Clock::now()));
            uiTime.push_back(busy);
        }
        CHECK_EQ(w.Editor().Text().substr(title - 6, 6 + static_cast<size_t>(row.keystrokes)),
                 "Report" + std::string(static_cast<size_t>(row.keystrokes), 'x'));
        const Before* before = nullptr;
        for (const Before& b : kBefore) {
            if (b.copies == row.copies) before = &b;
        }
        std::printf("  %7d %10zu %6d %4d | %9.1f / %9.1f | %9.1f / %9.1f | %5.2f %5.2f / %5.2f %5.2f\n", row.copies,
                    text.size(), w.Preview().PageCount(), row.keystrokes, before ? before->p50 : 0.0,
                    before ? before->p99 : 0.0, Percentile(latency, 0.5), Percentile(latency, 0.99),
                    before ? before->uiP50 : 0.0, before ? before->uiP99 : 0.0, Percentile(uiTime, 0.5),
                    Percentile(uiTime, 0.99));

        if (row.copies == 700) {
            // Quitting while the preview of a keystroke builds: the app goes away at once.
            const int requested = w.RequestedGeneration();
            rgui::InputEvent key;
            key.type = rgui::InputEvent::Type::Text;
            key.text = "x";
            h.app->OnInput(key);
            h.Frame();
            CHECK(h.RunUntil([&] { return w.RequestedGeneration() > requested; }, 10000));
            const auto start = Clock::now();
            h.app.reset();
            std::printf("  quit with the preview build of the %d copies in flight: before %.1f ms, after %.1f ms\n",
                        row.copies, kQuitBefore, MsBetween(start, Clock::now()));
        }
    }
}

} // namespace
