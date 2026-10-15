// SPDX-License-Identifier: Apache-2.0
// The Compose file reader (~/.XCompose and the locale's file, libX11 format, includes), then
// sequences of the usual starters and keys through the table it built.
#include "harness.h"

#include "common/file_io.h"
#include "platform/linux/compose.h"

#include <cstdint>
#include <string>

namespace rgui::fuzz {

int FuzzCompose(const uint8_t* data, size_t size) {
    const std::string dir = TempDir();
    const std::string path = dir + "/Compose";
    if (!RayoMd::File::WriteAll(path, std::string(reinterpret_cast<const char*>(data), size))) {
        Fail("cannot write the input file");
    }
    xkb::Compose compose;
    if (!compose.Load(path, dir + "/no-locale")) return 0;

    // Multi_key, dead_grave, dead_acute, dead_tilde, dead_diaeresis; then a e o ' " / - < Shift_L.
    static const uint32_t kStarters[] = {0xff20, 0xfe50, 0xfe51, 0xfe53, 0xfe57};
    static const uint32_t kKeys[] = {'a', 'e', 'o', '\'', '"', '/', '-', '<', 0xffe1};
    std::string text;
    for (uint32_t starter : kStarters) {
        for (uint32_t first : kKeys) {
            for (uint32_t second : kKeys) {
                compose.Feed(starter, text);
                compose.Feed(first, text);
                compose.Feed(second, text);
                compose.Reset();
            }
        }
    }
    return 0;
}

} // namespace rgui::fuzz
