// SPDX-License-Identifier: Apache-2.0
// Replays the minimized inputs the fuzzers found (tests/fuzz/regressions/<harness>/) through the
// same harness functions, so a fixed crash, hang or broken invariant stays fixed in every
// build, the sanitizer builds included. A failure aborts with the harness's message.
#include "fuzz/harness.h"
#include "runner.h"

#include "common/file_io.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct Harness {
    const char* name;
    int (*run)(const uint8_t*, size_t);
};

const Harness kHarnesses[] = {
    {"pdf", rgui::fuzz::FuzzPdf},
    {"jpeg", rgui::fuzz::FuzzJpeg},
    {"font", rgui::fuzz::FuzzFont},
    {"theme", rgui::fuzz::FuzzTheme},
    {"fontconf", rgui::fuzz::FuzzFontconf},
    {"settings", rgui::fuzz::FuzzSettings},
#if defined(__linux__)
    {"xkb", rgui::fuzz::FuzzXkb},
    {"compose", rgui::fuzz::FuzzCompose},
    {"dbus", rgui::fuzz::FuzzDbus},
    {"wl_wire", rgui::fuzz::FuzzWlWire},
#endif
};

} // namespace

RGUI_TEST("fuzz.regressions") {
    const std::filesystem::path root = std::filesystem::path(RGUI_SOURCE_DIR) / "tests" / "fuzz" / "regressions";
    size_t replayed = 0;
    for (const Harness& harness : kHarnesses) {
        // Empty input and one zero byte: every harness takes them.
        const uint8_t zero = 0;
        harness.run(&zero, 0);
        harness.run(&zero, 1);

        std::error_code ec;
        std::vector<std::filesystem::path> inputs;
        for (const auto& entry : std::filesystem::directory_iterator(root / harness.name, ec)) {
            if (entry.is_regular_file()) inputs.push_back(entry.path());
        }
        std::sort(inputs.begin(), inputs.end());
        for (const auto& input : inputs) {
            std::vector<uint8_t> bytes;
            CHECK(RayoMd::File::ReadAll(input, 64u << 20, bytes));
            std::fprintf(stderr, "replay %s/%s\n", harness.name, input.filename().string().c_str());
            harness.run(bytes.data(), bytes.size());
            ++replayed;
        }
    }
    std::fprintf(stderr, "fuzz.regressions: %zu inputs replayed\n", replayed);
}
