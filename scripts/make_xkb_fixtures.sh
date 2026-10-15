#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Writes the keymap fixtures of the linux.xkb tests, from libxkbcommon:
#
# tests/data/xkb/<name>.xkb     the keymap, compiled by `xkbcli compile-keymap` in the xkb_v1 text
#                               format compositors send;
# tests/data/xkb/<name>.expect  every key and modifier combination `xkbcli how-to-type` lists for
#                               ą ę ł ż ü ß é @ € ç ñ, Multi_key and ISO_Level3_Shift:
#     <keycode> <layout> <level> <mods> <level keysym> <state keysym> <code point>  # <name> <char>
#                               how-to-type gives the keysym of the level; the state keysym is
#                               what libxkbcommon's state answers for the same key and modifiers
#                               (xkb_state_key_get_one_sym), which adds the Caps Lock
#                               transformation (Lock + AltGr + ß is ẞ where the key's type keeps
#                               Lock); the code point is that keysym's character, 0 when none;
# tests/data/xkb/<name>.state   libxkbcommon's state answers for keycodes 9-135 in every layout:
#     <keycode> <layout> <repeats> <keysym for each mask of the header line>
#
# keycode is the XKB keycode (evdev + 8); layout and level count from 1; mods are real modifier
# masks (Shift 0x1, Lock 0x2, Control 0x4, Mod1-Mod5 0x8-0x80). Run from the repository root in a
# container with xkbcli, libxkbcommon's headers, a C compiler and python3 (rgui-arch); commit the
# output.
set -eu

out=tests/data/xkb
mkdir -p "$out"
chars="ą ę ł ż ü ß é @ € ç ñ"
keysyms="Multi_key ISO_Level3_Shift"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/state.c" <<'EOF'
#include <xkbcommon/xkbcommon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char text[1 << 23];
static const unsigned masks[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x8, 0x10, 0x11, 0x20, 0x40,
                                 0x80, 0x81, 0x82, 0x83};

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    struct xkb_keymap* keymap = xkb_keymap_new_from_string(ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    if (!keymap) return 1;
    struct xkb_state* state = xkb_state_new(keymap);
    if (strcmp(argv[2], "expect") == 0) {
        char line[1024];
        while (fgets(line, sizeof line, stdin)) {
            unsigned keycode, layout, level, mods, keysym;
            char rest[900] = "";
            if (line[0] == '#' ||
                sscanf(line, "%u %u %u %x %x %899[^\n]", &keycode, &layout, &level, &mods, &keysym, rest) < 5) {
                fputs(line, stdout);
                continue;
            }
            xkb_state_update_mask(state, mods, 0, 0, 0, 0, layout - 1);
            xkb_keysym_t sym = xkb_state_key_get_one_sym(state, keycode);
            uint32_t cp = xkb_keysym_to_utf32(sym);
            if (cp < 0x20 || cp == 0x7f) cp = 0;
            printf("%u %u %u 0x%x 0x%x 0x%x 0x%x %s\n", keycode, layout, level, mods, keysym,
                   (unsigned)sym, (unsigned)cp, rest);
        }
        return 0;
    }
    printf("# masks");
    for (size_t m = 0; m < sizeof masks / sizeof masks[0]; ++m) printf(" 0x%x", masks[m]);
    printf("\n");
    for (unsigned keycode = 9; keycode <= 135; ++keycode) {
        for (unsigned layout = 0; layout < xkb_keymap_num_layouts_for_key(keymap, keycode); ++layout) {
            printf("%u %u %d", keycode, layout + 1, xkb_keymap_key_repeats(keymap, keycode));
            for (size_t m = 0; m < sizeof masks / sizeof masks[0]; ++m) {
                xkb_state_update_mask(state, masks[m], 0, 0, 0, 0, layout);
                printf(" %x", (unsigned)xkb_state_key_get_one_sym(state, keycode));
            }
            printf("\n");
        }
    }
    return 0;
}
EOF
cc -O2 -o "$tmp/state" "$tmp/state.c" -lxkbcommon

# name|layout|options
fixtures="us|us|
pl|pl|
de|de|
fr|fr|
us_pl|us,pl|grp:alts_toggle
us_compose|us|compose:ralt
ru_us|ru,us|grp:alts_toggle"

version=$(xkbcli --version)
echo "$fixtures" | while IFS='|' read -r name layout options; do
    set -- --rules evdev --model pc105 --layout "$layout"
    if [ -n "$options" ]; then set -- "$@" --options "$options"; fi
    xkbcli compile-keymap --format 1 "$@" > "$out/$name.xkb"
    {
        echo "# xkbcli how-to-type $* (libxkbcommon $version)"
        for c in $chars; do echo "@char $c"; xkbcli how-to-type --disable-compose "$@" "$c"; done
        for k in $keysyms; do echo "@char -"; xkbcli how-to-type --disable-compose --keysym "$@" "$k"; done
    } | python3 -c '
import re, sys
real = {"Shift": 1, "Lock": 2, "Control": 4, "Mod1": 8, "Mod2": 16, "Mod3": 32, "Mod4": 64, "Mod5": 128}
virtual = {"LevelThree", "LevelFive", "NumLock", "Alt", "Meta", "Super"}
row = re.compile(r"^(\d+)\s+(\S+)\s+(\d+)\s+(.*?)\s+(\d+)\s+\[(.*)\]\s*$")
keysym = name = char = None
for line in sys.stdin:
    line = line.rstrip("\n")
    if line.startswith("# "):
        print(line)
        continue
    if line.startswith("@char "):
        char, keysym = line[6:], None
        continue
    m = re.match(r"^keysym: (\S+) \((0x[0-9a-fA-F]+)\)", line)
    if m:
        name, keysym = m.group(1), int(m.group(2), 16)
        continue
    m = row.match(line)
    if not m:
        continue
    mods = 0
    for mod in m.group(6).split():
        if mod in real:
            mods |= real[mod]
        elif mod not in virtual:
            sys.exit("unexpected modifier " + mod)
    if keysym is None:
        sys.exit("row before a keysym line")
    print(f"{m.group(1)} {m.group(3)} {m.group(5)} 0x{mods:x} 0x{keysym:x} # {name} {char}")
' | "$tmp/state" "$out/$name.xkb" expect > "$out/$name.expect"
    {
        echo "# libxkbcommon $version state for xkbcli compile-keymap $*"
        "$tmp/state" "$out/$name.xkb" table
    } > "$out/$name.state"
    echo "$out/$name: $(grep -vc '^#' "$out/$name.expect") expectations, $(grep -vc '^#' "$out/$name.state") state rows"
done
