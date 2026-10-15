// SPDX-License-Identifier: Apache-2.0
// Compose sequences (<Multi_key> <a> <e> -> æ, <dead_acute> <e> -> é) from the user's or the
// locale's Compose file, in the libX11 format. The file is read on the first Multi_key or dead_*
// keysym: $XCOMPOSEFILE, else ~/.XCompose, else the locale's file named by
// /usr/share/X11/locale/compose.dir (locale: the first non-empty of LC_ALL, LC_CTYPE, LANG, else
// en_US.UTF-8). `include` lines are followed to a depth of 4, with %L (the locale's file), %H
// ($HOME) and %S (the system locale directory).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rgui::xkb {

class Compose {
public:
    enum class Result {
        Ignored,    // not part of a sequence: the keysym types as usual
        Composing,  // the keysym continues a sequence: it types nothing
        Composed,   // a sequence ended: `text` holds what it types
        Cancelled,  // the keysym does not continue the sequence: the sequence and the keysym are dropped
    };

    // Feeds the keysym of a key press. Modifier keysyms (Shift, Control, ISO_Level3_Shift, ...)
    // are Ignored and leave a sequence going. `text` is set when Composed, cleared otherwise.
    Result Feed(uint32_t keysym, std::string& text);
    // Drops a sequence in progress (focus change, layout change).
    void Reset();

    // Loads `path` now instead of the default file, with %S and the locale's file taken from
    // `localeDir` (tests). False when nothing could be read; the table is empty then.
    bool Load(const std::string& path, const std::string& localeDir);

    struct Node {
        uint32_t keysym = 0;
        uint32_t firstChild = 0;   // 0: none (node 0 is the root, never a child)
        uint32_t nextSibling = 0;  // 0: none
        uint32_t resultOffset = 0; // into results_
        uint32_t resultLength = 0;
        bool leaf = false;
    };

private:
    void LoadDefault();

    bool loaded_ = false;
    uint32_t current_ = 0;  // the node of the sequence so far; 0: no sequence
    std::vector<Node> nodes_;
    std::string results_;
};

} // namespace rgui::xkb
