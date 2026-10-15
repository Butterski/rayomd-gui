// SPDX-License-Identifier: Apache-2.0
// The XKB keymap a Wayland compositor sends (wl_keyboard.keymap, format xkb_v1), read by our own
// parser: keycodes and aliases, key types, the virtual-modifier part of the compatibility map,
// the symbols and the modifier map. Together with the modifier state of wl_keyboard.modifiers it
// answers which keysym a key gives, whether it repeats, and which Latin key a shortcut means.
// Actions, indicators and geometry are not interpreted: the compositor runs the state machine and
// sends us its result.
#pragma once

#include "app/platform.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace rgui::xkb {

// The character a keysym types (Latin-1 keysyms are their code point, 0x01000000 + cp are
// Unicode keysyms, legacy keysyms map through keysyms.inc); 0 when it types nothing: no mapping,
// a control character (< U+0020) or U+007F.
uint32_t KeysymToCodepoint(uint32_t keysym);
// The keysym of a name as keymaps and Compose files write it: a keysym name (`aogonek`,
// `dead_acute`, `XF86Copy`), `U20AC` (a Unicode keysym; Latin-1 code points give their legacy
// keysym) or `0x1001234`. 0 when the name is unknown (and for `NoSymbol`).
uint32_t KeysymFromName(std::string_view name);

class Keymap {
public:
    // Parses the keymap text (at most 8 MiB, NUL-terminated or not). nullptr with *error set when
    // the text is not a keymap this reader understands; never reads out of bounds.
    static std::unique_ptr<Keymap> Parse(std::string_view text, std::string* error);
    ~Keymap();
    Keymap(const Keymap&) = delete;
    Keymap& operator=(const Keymap&) = delete;

    // wl_keyboard.modifiers: the depressed, latched and locked modifier masks and the effective
    // group (layout index).
    void UpdateModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

    // The keysym of an evdev key code (XKB keycode = evdevKey + 8) in the current state, after
    // the Caps Lock transformation; 0 (NoSymbol) when the key gives none or several keysyms.
    uint32_t KeySym(uint32_t evdevKey) const;
    // The same for a given group (wrapped into the key's groups) and modifier mask (real
    // modifiers Shift 0x1, Lock 0x2, Control 0x4, Mod1-Mod5 0x8-0x80, plus any bits the keymap
    // gives virtual modifiers of its own).
    uint32_t KeySymAt(uint32_t evdevKey, uint32_t group, uint32_t mods) const;
    // Whether holding the key repeats it.
    bool Repeats(uint32_t evdevKey) const;
    // The key a shortcut means: the level-1 keysym of the current group when it is a Latin letter,
    // a digit or a named key; else the first group whose level-1 keysym is a Latin letter or
    // digit; else the key's US position. Key::None when none applies.
    Key ShortcutKey(uint32_t evdevKey) const;
    // rgui::Modifiers bits (Ctrl, Shift, Alt, Super) of the current state.
    uint8_t Modifiers() const;

    struct Data;

private:
    explicit Keymap(std::unique_ptr<Data> data);
    std::unique_ptr<Data> data_;
    uint32_t mods_ = 0;
    uint32_t group_ = 0;
};

} // namespace rgui::xkb
