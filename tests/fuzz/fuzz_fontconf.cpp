// SPDX-License-Identifier: Apache-2.0
// fontchoice's bounded fontconfig XML reader (fonts.conf, 50-omarchy.conf).
#include "harness.h"

#include "text/fontchoice.h"

#include <string_view>

namespace rgui::fuzz {

int FuzzFontconf(const uint8_t* data, size_t size) {
    const std::string_view xml(reinterpret_cast<const char*>(data), size);
    for (const bool aliases : {false, true}) {
        for (const std::string& family : text::MonospaceFamilies(xml, aliases)) {
            if (family.empty()) Fail("MonospaceFamilies gave an empty family");
        }
    }
    return 0;
}

} // namespace rgui::fuzz
