// SPDX-License-Identifier: Apache-2.0
// The XKB keymap reader (the xkb_v1 text a compositor sends through wl_keyboard.keymap), then
// every lookup on every key code in a few modifier and group states.
#include "harness.h"

#include "platform/linux/xkb_keymap.h"

#include <string>
#include <string_view>

namespace rgui::fuzz {

int FuzzXkb(const uint8_t* data, size_t size) {
    std::string error;
    const auto keymap = xkb::Keymap::Parse(std::string_view(reinterpret_cast<const char*>(data), size), &error);
    if (!keymap) return 0;
    static const uint32_t kMods[] = {0, 0x1, 0x2, 0x4, 0x8, 0x80, 0x81, 0xffffffffu};
    static const uint32_t kGroups[] = {0, 1, 3, 0xffffffffu};
    for (uint32_t group : kGroups) {
        keymap->UpdateModifiers(kMods[group & 7], 0, group == 1 ? 0x2 : 0, group);
        (void)keymap->Modifiers();
        for (uint32_t key = 0; key < 256 + 8; ++key) {
            (void)keymap->KeySym(key);
            (void)keymap->Repeats(key);
            (void)keymap->ShortcutKey(key);
            for (uint32_t mods : kMods) (void)xkb::KeysymToCodepoint(keymap->KeySymAt(key, group, mods));
        }
    }
    return 0;
}

} // namespace rgui::fuzz
