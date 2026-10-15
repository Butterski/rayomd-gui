// SPDX-License-Identifier: Apache-2.0
// The keyboard layer: the XKB keymap reader against what libxkbcommon says about the same keymaps
// (tests/data/xkb, made by scripts/make_xkb_fixtures.sh), the shortcut keys, Caps Lock, key
// repeat, the keysym tables, the reader's bounds, and the Compose reader.
#include "runner.h"

#include "platform/linux/compose.h"
#include "platform/linux/xkb_keymap.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using rgui::Key;
using rgui::xkb::Compose;
using rgui::xkb::Keymap;
using rgui::xkb::KeysymFromName;
using rgui::xkb::KeysymToCodepoint;

constexpr uint32_t kShift = 0x1;
constexpr uint32_t kLock = 0x2;
constexpr uint32_t kControl = 0x4;
constexpr uint32_t kMod1 = 0x8;
constexpr uint32_t kMod4 = 0x40;
constexpr uint32_t kMod5 = 0x80;

std::string DataPath(const std::string& name) {
    return std::string(rgui::test::SourceDir()) + "/tests/data/xkb/" + name;
}

// The milliseconds `call` takes with a FIFO (no writer) at `path`; -1 when it cannot be made. A
// reader that blocks on the FIFO is let go after 3 s (a writer opens it), so a regression fails
// the time bound instead of hanging the test.
template <typename Call>
double MillisecondsWithFifo(const std::string& path, Call call) {
    unlink(path.c_str());
    if (mkfifo(path.c_str(), 0600) != 0) return -1;
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 300 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (done) return;
        const int fd = open(path.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) close(fd);
    });
    const auto start = std::chrono::steady_clock::now();
    call();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    done = true;
    watchdog.join();
    unlink(path.c_str());
    return ms;
}

std::string ReadText(const std::string& path) {
    std::string text;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return text;
    char buffer[65536];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
    std::fclose(f);
    return text;
}

std::unique_ptr<Keymap> LoadKeymap(const std::string& name) {
    const std::string text = ReadText(DataPath(name + ".xkb"));
    std::string error;
    std::unique_ptr<Keymap> keymap = Keymap::Parse(text, &error);
    if (!keymap) std::printf("  %s.xkb: %s\n", name.c_str(), error.c_str());
    return keymap;
}

struct Expectation {
    uint32_t keycode, layout, level, mods, levelKeysym, keysym, cp;
};

std::vector<Expectation> LoadExpectations(const std::string& name) {
    std::vector<Expectation> out;
    const std::string text = ReadText(DataPath(name + ".expect"));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty() || line[0] == '#') continue;
        Expectation e{};
        if (std::sscanf(line.c_str(), "%u %u %u %x %x %x %x", &e.keycode, &e.layout, &e.level, &e.mods,
                        &e.levelKeysym, &e.keysym, &e.cp) == 7)
            out.push_back(e);
    }
    return out;
}

// Sets an environment variable for the life of the object.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        if (old) {
            had_ = true;
            old_ = old;
        }
        if (value) setenv(name, value, 1);
        else unsetenv(name);
    }
    ~ScopedEnv() {
        if (had_) setenv(name_, old_.c_str(), 1);
        else unsetenv(name_);
    }

private:
    const char* name_;
    bool had_ = false;
    std::string old_;
};

const char* const kFixtures[] = {"us", "pl", "de", "fr", "us_pl", "us_compose", "ru_us"};

} // namespace

RGUI_TEST("linux.xkb.how_to_type") {
    // Every (keycode, modifiers, layout) xkbcli how-to-type lists for ą ę ł ż ü ß é @ € ç ñ,
    // Multi_key and ISO_Level3_Shift gives the keysym libxkbcommon's state gives. That is the
    // keysym of the level how-to-type names, except where Caps Lock is on and the key's type
    // keeps it: how-to-type leaves out the Caps Lock transformation (pl: Lock+AltGr+t is ẞ).
    size_t checked = 0;
    size_t transformed = 0;
    for (const char* name : kFixtures) {
        std::unique_ptr<Keymap> keymap = LoadKeymap(name);
        CHECK(keymap != nullptr);
        if (!keymap) continue;
        const std::vector<Expectation> expectations = LoadExpectations(name);
        CHECK(!expectations.empty());
        for (const Expectation& e : expectations) {
            const uint32_t evdev = e.keycode - 8;
            const uint32_t keysym = keymap->KeySymAt(evdev, e.layout - 1, e.mods);
            if (keysym != e.keysym)
                std::printf("  %s: keycode %u layout %u mods 0x%x: 0x%x, expected 0x%x\n", name, e.keycode,
                            e.layout, e.mods, keysym, e.keysym);
            CHECK_EQ(keysym, e.keysym);
            CHECK_EQ(KeysymToCodepoint(keysym), e.cp);
            // The same through the compositor's modifier state (depressed mods, locked group).
            keymap->UpdateModifiers(e.mods, 0, 0, e.layout - 1);
            CHECK_EQ(keymap->KeySym(evdev), e.keysym);
            if (e.levelKeysym != e.keysym) ++transformed;
            ++checked;
        }
    }
    CHECK(checked >= 60);
    CHECK(transformed >= 2);
}

RGUI_TEST("linux.xkb.state_table") {
    // libxkbcommon's state for keycodes 9-135 in every layout of every fixture, under 14 modifier
    // masks, and its repeat flags.
    size_t checked = 0;
    for (const char* name : kFixtures) {
        std::unique_ptr<Keymap> keymap = LoadKeymap(name);
        CHECK(keymap != nullptr);
        if (!keymap) continue;
        const std::string text = ReadText(DataPath(std::string(name) + ".state"));
        std::vector<uint32_t> masks;
        size_t pos = 0;
        size_t rows = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            const char* p = line.c_str();
            char* next = nullptr;
            if (line.compare(0, 8, "# masks ") == 0) {
                // # masks 0x0 0x1 ...
                p += 8;
                for (unsigned long v = std::strtoul(p, &next, 16); next != p; v = std::strtoul(p, &next, 16)) {
                    masks.push_back(static_cast<uint32_t>(v));
                    p = next;
                }
                continue;
            }
            if (line.empty() || line[0] == '#') continue;
            // <keycode> <layout> <repeats> <keysym per mask, hexadecimal>
            unsigned keycode = 0, layout = 0;
            int repeats = 0;
            int used = 0;
            if (std::sscanf(p, "%u %u %d%n", &keycode, &layout, &repeats, &used) != 3) continue;
            p += used;
            std::vector<uint32_t> keysyms;
            for (unsigned long v = std::strtoul(p, &next, 16); next != p; v = std::strtoul(p, &next, 16)) {
                keysyms.push_back(static_cast<uint32_t>(v));
                p = next;
            }
            CHECK_EQ(keysyms.size(), masks.size());
            if (keysyms.size() != masks.size() || masks.empty()) continue;
            const uint32_t evdev = keycode - 8;
            CHECK_EQ(keymap->Repeats(evdev), repeats != 0);
            for (size_t m = 0; m < masks.size(); ++m) {
                const uint32_t keysym = keymap->KeySymAt(evdev, layout - 1, masks[m]);
                if (keysym != keysyms[m])
                    std::printf("  %s: keycode %u layout %u mods 0x%x: 0x%x, expected 0x%x\n", name, keycode,
                                layout, masks[m], keysym, keysyms[m]);
                CHECK_EQ(keysym, keysyms[m]);
                ++checked;
            }
            ++rows;
        }
        CHECK(rows >= 100);
    }
    CHECK(checked >= 10000);
}

RGUI_TEST("linux.xkb.parse_time") {
    const std::string text = ReadText(DataPath("us_pl.xkb"));
    std::string error;
    double best = 1e9;
    for (int i = 0; i < 5; ++i) {
        const auto start = std::chrono::steady_clock::now();
        std::unique_ptr<Keymap> keymap = Keymap::Parse(text, &error);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        CHECK(keymap != nullptr);
        if (ms < best) best = ms;
    }
    std::printf("  us_pl.xkb (%zu bytes): parse %.2f ms\n", text.size(), best);
}

RGUI_TEST("linux.xkb.groups_and_levels") {
    std::unique_ptr<Keymap> keymap = LoadKeymap("us_pl");
    CHECK(keymap != nullptr);
    if (!keymap) return;
    // <AC09> (evdev 38): l, L in us; l, L, lstroke, Lstroke in pl.
    CHECK_EQ(keymap->KeySymAt(38, 0, 0), uint32_t('l'));
    CHECK_EQ(keymap->KeySymAt(38, 0, kMod5), uint32_t('l'));        // us has no third level
    CHECK_EQ(keymap->KeySymAt(38, 1, kMod5), 0x1b3u);               // lstroke
    CHECK_EQ(keymap->KeySymAt(38, 1, kMod5 | kShift), 0x1a3u);      // Lstroke
    CHECK_EQ(keymap->KeySymAt(38, 3, kMod5), 0x1b3u);               // group 3 wraps to pl
    CHECK_EQ(keymap->KeySymAt(38, 1, kMod5 | kControl), 0x1b3u);    // Control is not a level modifier
    keymap->UpdateModifiers(0, 0, 0, 1);
    CHECK_EQ(keymap->KeySym(38), uint32_t('l'));
    keymap->UpdateModifiers(kMod5, 0, 0, 1);
    CHECK_EQ(keymap->KeySym(38), 0x1b3u);
    keymap->UpdateModifiers(0, kShift, kMod5, 1);  // latched and locked count too
    CHECK_EQ(keymap->KeySym(38), 0x1a3u);
    CHECK_EQ(keymap->KeySym(5000), 0u);  // no such key
    CHECK_EQ(keymap->KeySym(0), 0u);
}

RGUI_TEST("linux.xkb.caps_lock") {
    std::unique_ptr<Keymap> pl = LoadKeymap("pl");
    std::unique_ptr<Keymap> us = LoadKeymap("us");
    std::unique_ptr<Keymap> fr = LoadKeymap("fr");
    CHECK(pl && us && fr);
    if (!pl || !us || !fr) return;
    // ALPHABETIC consumes Lock: Caps gives the upper case, Shift+Caps the lower case.
    CHECK_EQ(us->KeySymAt(30, 0, kLock), uint32_t('A'));
    CHECK_EQ(us->KeySymAt(30, 0, kLock | kShift), uint32_t('a'));
    // TWO_LEVEL does not use Lock, and digits have no case.
    CHECK_EQ(us->KeySymAt(2, 0, kLock), uint32_t('1'));
    CHECK_EQ(us->KeySymAt(2, 0, kLock | kShift), uint32_t('!'));
    // FOUR_LEVEL_ALPHABETIC (a, A, aogonek, Aogonek): Caps+AltGr is the fourth level.
    CHECK_EQ(pl->KeySymAt(30, 0, kLock | kMod5), 0x1a1u);           // Aogonek
    CHECK_EQ(pl->KeySymAt(30, 0, kLock | kShift | kMod5), 0x1b1u);  // aogonek
    // FOUR_LEVEL_SEMIALPHABETIC preserves Lock with AltGr: the Caps Lock transformation turns
    // ß into ẞ (U+1E9E), as libxkbcommon does.
    CHECK_EQ(pl->KeySymAt(20, 0, kMod5), 0xdfu);
    CHECK_EQ(pl->KeySymAt(20, 0, kMod5 | kLock), 0x1001e9eu);
    CHECK_EQ(KeysymToCodepoint(pl->KeySymAt(20, 0, kMod5 | kLock)), 0x1e9eu);
    // fr <AC02> s, S, ssharp, SSHARP: ß counts as lower case, so the key is FOUR_LEVEL_ALPHABETIC.
    CHECK_EQ(fr->KeySymAt(31, 0, kMod5), 0xdfu);
    CHECK_EQ(fr->KeySymAt(31, 0, kMod5 | kShift), 0x1001e9eu);
    CHECK_EQ(fr->KeySymAt(31, 0, kMod5 | kLock), 0x1001e9eu);
}

RGUI_TEST("linux.xkb.shortcut_keys") {
    std::unique_ptr<Keymap> us = LoadKeymap("us");
    std::unique_ptr<Keymap> pl = LoadKeymap("pl");
    std::unique_ptr<Keymap> de = LoadKeymap("de");
    std::unique_ptr<Keymap> fr = LoadKeymap("fr");
    std::unique_ptr<Keymap> ru = LoadKeymap("ru_us");
    CHECK(us && pl && de && fr && ru);
    if (!us || !pl || !de || !fr || !ru) return;
    // Named keys.
    CHECK(us->ShortcutKey(28) == Key::Enter);
    CHECK(us->ShortcutKey(1) == Key::Escape);
    CHECK(us->ShortcutKey(63) == Key::F5);
    CHECK(us->ShortcutKey(88) == Key::F12);
    CHECK(us->ShortcutKey(105) == Key::Left);
    CHECK(us->ShortcutKey(57) == Key::Space);
    CHECK(us->ShortcutKey(13) == Key::Equal);
    CHECK(us->ShortcutKey(12) == Key::Minus);
    CHECK(us->ShortcutKey(51) == Key::Comma);
    CHECK(us->ShortcutKey(78) == Key::Plus);  // KP_Add
    // Latin layouts: the letter the layout puts on the key, whatever the modifiers.
    CHECK(pl->ShortcutKey(30) == Key::A);
    pl->UpdateModifiers(kMod5 | kControl, 0, 0, 0);
    CHECK(pl->ShortcutKey(30) == Key::A);
    CHECK(de->ShortcutKey(21) == Key::Z);   // QWERTZ
    CHECK(de->ShortcutKey(44) == Key::Y);
    CHECK(de->ShortcutKey(27) == Key::Plus);
    CHECK(de->ShortcutKey(12) == Key::Minus);  // ß: no Latin key in any group, the US position
    CHECK(fr->ShortcutKey(16) == Key::A);   // AZERTY
    CHECK(fr->ShortcutKey(30) == Key::Q);
    CHECK(fr->ShortcutKey(44) == Key::W);
    CHECK(fr->ShortcutKey(2) == Key::Digit1);  // ampersand: the US position
    CHECK(fr->ShortcutKey(7) == Key::Minus);   // the key AZERTY labels '-'
    // A non-Latin first layout: the Latin layout's letter, in either group.
    CHECK(ru->ShortcutKey(30) == Key::A);
    CHECK(ru->ShortcutKey(46) == Key::C);
    CHECK(ru->ShortcutKey(2) == Key::Digit1);
    ru->UpdateModifiers(0, 0, 0, 1);
    CHECK(ru->ShortcutKey(30) == Key::A);
    CHECK(ru->KeySym(30) == uint32_t('a'));
    ru->UpdateModifiers(0, 0, 0, 0);
    CHECK_EQ(ru->KeySym(30), 0x6c6u);  // Cyrillic_ef
    // A key neither the keymap nor the US layout has.
    CHECK(us->ShortcutKey(4000) == Key::None);
}

RGUI_TEST("linux.xkb.repeat_and_modifiers") {
    std::unique_ptr<Keymap> us = LoadKeymap("us");
    CHECK(us != nullptr);
    if (!us) return;
    CHECK(us->Repeats(30));    // a
    CHECK(us->Repeats(28));    // Return
    CHECK(us->Repeats(14));    // BackSpace
    CHECK(us->Repeats(79));    // KP_End, an interpret with repeat= True
    CHECK(!us->Repeats(42));   // Shift_L
    CHECK(!us->Repeats(29));   // Control_L
    CHECK(!us->Repeats(58));   // Caps_Lock
    CHECK(!us->Repeats(84));   // ISO_Level3_Shift
    CHECK(!us->Repeats(4000));

    us->UpdateModifiers(kControl | kMod1, 0, 0, 0);
    CHECK_EQ(us->Modifiers(), uint8_t(rgui::kModCtrl | rgui::kModAlt));
    us->UpdateModifiers(0, kShift, 0, 0);
    CHECK_EQ(us->Modifiers(), uint8_t(rgui::kModShift));
    us->UpdateModifiers(kMod4, 0, 0, 0);
    CHECK_EQ(us->Modifiers(), uint8_t(rgui::kModSuper));
    us->UpdateModifiers(kMod5, 0, kLock, 0);  // AltGr and Caps Lock are not shortcut modifiers
    CHECK_EQ(us->Modifiers(), uint8_t(0));
}

RGUI_TEST("linux.xkb.keysyms") {
    CHECK_EQ(KeysymFromName("aogonek"), 0x1b1u);
    CHECK_EQ(KeysymFromName("dead_acute"), 0xfe51u);
    CHECK_EQ(KeysymFromName("Multi_key"), 0xff20u);
    CHECK_EQ(KeysymFromName("XF86Copy"), 0x1008ff57u);
    CHECK_EQ(KeysymFromName("U20AC"), 0x10020acu);
    CHECK_EQ(KeysymFromName("U00e9"), 0xe9u);       // Latin-1 code points are their own keysym
    CHECK_EQ(KeysymFromName("0x1001234"), 0x1001234u);
    CHECK_EQ(KeysymFromName("1"), uint32_t('1'));
    CHECK_EQ(KeysymFromName("Ukrainian_ie"), 0x6a4u);  // a name, not a U+ code
    CHECK_EQ(KeysymFromName("NoSymbol"), 0u);
    CHECK_EQ(KeysymFromName("no_such_keysym"), 0u);
    CHECK_EQ(KeysymFromName("U110000"), 0u);
    CHECK_EQ(KeysymFromName("U0007"), 0u);
    CHECK_EQ(KeysymFromName(""), 0u);

    CHECK_EQ(KeysymToCodepoint('A'), 0x41u);
    CHECK_EQ(KeysymToCodepoint(0xe9), 0xe9u);
    CHECK_EQ(KeysymToCodepoint(0x1b1), 0x105u);       // aogonek
    CHECK_EQ(KeysymToCodepoint(0x6c6), 0x444u);       // Cyrillic_ef
    CHECK_EQ(KeysymToCodepoint(0x20ac), 0x20acu);     // EuroSign
    CHECK_EQ(KeysymToCodepoint(0x10020ac), 0x20acu);
    CHECK_EQ(KeysymToCodepoint(0xffb5), uint32_t('5'));  // KP_5
    CHECK_EQ(KeysymToCodepoint(0xff80), uint32_t(' '));  // KP_Space
    CHECK_EQ(KeysymToCodepoint(0xff0d), 0u);           // Return types nothing
    CHECK_EQ(KeysymToCodepoint(0xff08), 0u);           // BackSpace
    CHECK_EQ(KeysymToCodepoint(0xffff), 0u);           // Delete
    CHECK_EQ(KeysymToCodepoint(0x7f), 0u);
    CHECK_EQ(KeysymToCodepoint(0x100d800), 0u);        // a surrogate
    CHECK_EQ(KeysymToCodepoint(0xfe51), 0u);           // dead_acute
    CHECK_EQ(KeysymToCodepoint(0xffe1), 0u);           // Shift_L
}

RGUI_TEST("linux.xkb.bounds") {
    const std::string text = ReadText(DataPath("us_pl.xkb"));
    CHECK(text.size() > 1000);
    std::string error;
    // A keymap cut anywhere before its final "};" is refused with a message.
    size_t refused = 0;
    size_t cuts = 0;
    for (size_t len = 0; len + 4 <= text.size(); len += 509) {
        ++cuts;
        error.clear();
        if (!Keymap::Parse(std::string_view(text.data(), len), &error) && !error.empty()) ++refused;
    }
    CHECK_EQ(refused, cuts);
    // Single-byte mutations either parse or are refused; nothing reads out of bounds (asan).
    uint32_t seed = 12345;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    size_t parsed = 0;
    for (int i = 0; i < 400; ++i) {
        std::string mutated = text;
        mutated[next() % mutated.size()] = static_cast<char>(next() & 0xff);
        std::unique_ptr<Keymap> keymap = Keymap::Parse(mutated, &error);
        if (!keymap) continue;
        ++parsed;
        for (uint32_t key = 0; key < 260; ++key) {
            keymap->UpdateModifiers(next() & 0xff, 0, 0, next() & 7);
            (void)keymap->KeySym(key);
            (void)keymap->ShortcutKey(key);
        }
    }
    CHECK(parsed > 0);
    // Hostile shapes: deep nesting, too many modifiers, huge numbers, stray bytes, no keys.
    std::string deep = "xkb_keymap { xkb_geometry \"g\" { ";
    for (int i = 0; i < 100; ++i) deep += "{ ";
    CHECK(!Keymap::Parse(deep, &error));
    std::string mods = "xkb_keymap { xkb_types { virtual_modifiers ";
    for (int i = 0; i < 30; ++i) mods += (i ? ",V" : "V") + std::to_string(i);
    mods += "; }; };";
    CHECK(!Keymap::Parse(mods, &error));
    CHECK(!error.empty());
    CHECK(!Keymap::Parse("xkb_keymap { xkb_keycodes { <A> = 99999999999999999999; }; };", &error));
    CHECK(!Keymap::Parse(std::string("xkb_keymap {\x01 };"), &error));
    CHECK(!Keymap::Parse("xkb_keymap { xkb_symbols { key <A> { [ a ] }; }; };", &error));  // no keycode
    CHECK(!Keymap::Parse("xkb_keymap { xkb_types { type \"T\" { map[Bogus] = 2; }; }; };", &error));
    CHECK(!Keymap::Parse("xkb_keymap { xkb_symbols { key <A> { [ a ] ] }; }; };", &error));
    CHECK(!Keymap::Parse("", &error));
    // The smallest keymap that works, with the NUL Wayland sends after it.
    const char minimal[] =
        "xkb_keymap { xkb_keycodes { <AC01> = 38; }; xkb_types { type \"ALPHABETIC\" { "
        "modifiers = Shift+Lock; map[Shift] = Level2; map[Lock] = 2; }; }; "
        "xkb_symbols { key <AC01> { [ a, A ] }; }; };";
    std::unique_ptr<Keymap> keymap = Keymap::Parse(std::string_view(minimal, sizeof(minimal)), &error);
    CHECK(keymap != nullptr);
    if (keymap) {
        CHECK_EQ(keymap->KeySymAt(30, 0, kShift), uint32_t('A'));
        CHECK_EQ(keymap->KeySymAt(30, 0, kLock), uint32_t('A'));
        CHECK(keymap->Repeats(30));
        CHECK(keymap->ShortcutKey(30) == Key::A);
    }
}

RGUI_TEST("linux.compose") {
    const std::string dir = DataPath("compose");
    ScopedEnv locale("LC_ALL", "pl_PL.UTF-8");  // not in the fixture's compose.dir: en_US.UTF-8
    Compose compose;
    CHECK(compose.Load(dir + "/Compose", dir + "/locale"));
    std::string text;
    auto type = [&](std::initializer_list<const char*> names) {
        std::string out;
        Compose::Result last = Compose::Result::Ignored;
        for (const char* name : names) {
            last = compose.Feed(KeysymFromName(name), text);
            if (last == Compose::Result::Composed) out = text;
        }
        return last == Compose::Result::Composed ? out : std::string("<none>");
    };
    // Through include "%L".
    CHECK_EQ(type({"dead_acute", "e"}), std::string("é"));
    CHECK_EQ(type({"dead_circumflex", "dead_acute", "e"}), std::string("ế"));
    CHECK_EQ(type({"Multi_key", "o", "c"}), std::string("©"));
    // The including file's own lines, the later <Multi_key> <a> <e> replacing the included one.
    CHECK_EQ(type({"Multi_key", "a", "e"}), std::string("æ"));
    CHECK_EQ(type({"Multi_key", "s", "s"}), std::string("ß"));
    CHECK_EQ(type({"Multi_key", "x", "x"}), std::string("A"));
    CHECK_EQ(type({"dead_grave", "a"}), std::string("à"));
    CHECK_EQ(type({"Multi_key", "c", "o", "o", "l"}), std::string("cool"));
    CHECK_EQ(type({"Multi_key", "U20AC", "e"}), std::string("€e"));
    // Through the self-including file, read once per level up to the include depth.
    CHECK_EQ(type({"Multi_key", "d", "d"}), std::string("đ"));
    // Lines that were skipped.
    CHECK_EQ(type({"Multi_key", "q", "q"}), std::string("<none>"));

    // The states one key at a time.
    CHECK(compose.Feed(KeysymFromName("a"), text) == Compose::Result::Ignored);
    CHECK(compose.Feed(KeysymFromName("Multi_key"), text) == Compose::Result::Composing);
    CHECK(compose.Feed(KeysymFromName("Shift_L"), text) == Compose::Result::Ignored);  // keeps going
    CHECK(compose.Feed(KeysymFromName("o"), text) == Compose::Result::Composing);       // <o> alone was skipped
    CHECK(compose.Feed(KeysymFromName("z"), text) == Compose::Result::Cancelled);
    CHECK(text.empty());
    CHECK(compose.Feed(KeysymFromName("e"), text) == Compose::Result::Ignored);  // the sequence is gone
    CHECK(compose.Feed(KeysymFromName("dead_acute"), text) == Compose::Result::Composing);
    compose.Reset();
    CHECK(compose.Feed(KeysymFromName("e"), text) == Compose::Result::Ignored);

    // A missing file loads nothing and composes nothing.
    Compose missing;
    CHECK(!missing.Load(dir + "/no-such-file", dir + "/locale"));
    CHECK(missing.Feed(KeysymFromName("Multi_key"), text) == Compose::Result::Ignored);

    // A FIFO where the Compose file should be (or one it includes) is refused at once, not waited
    // on: the UI thread reads Compose files.
    const std::string fifo = std::string(rgui::test::BuildDir()) + "/compose-fifo";
    Compose blocked;
    bool loaded = true;
    const double ms = MillisecondsWithFifo(fifo, [&] { loaded = blocked.Load(fifo, dir + "/locale"); });
    CHECK(ms >= 0 && ms < 1000);
    CHECK(!loaded);
}

RGUI_TEST("linux.compose.default_file") {
    // $XCOMPOSEFILE is read on the first Multi_key, not before.
    const std::string dir = DataPath("compose");
    ScopedEnv file("XCOMPOSEFILE", (dir + "/Compose").c_str());
    Compose compose;
    std::string text;
    CHECK(compose.Feed('a', text) == Compose::Result::Ignored);
    CHECK(compose.Feed(KeysymFromName("Multi_key"), text) == Compose::Result::Composing);
    CHECK(compose.Feed('a', text) == Compose::Result::Composing);
    CHECK(compose.Feed('e', text) == Compose::Result::Composed);
    CHECK_EQ(text, std::string("æ"));
}
