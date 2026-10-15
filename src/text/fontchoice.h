// SPDX-License-Identifier: Apache-2.0
// The interface and editor font: Omarchy's fontconfig `monospace` on Linux, a Windows monospace
// family on Windows; its regular and bold faces and a per-OS fallback chain for code points it
// lacks.
#pragma once

#include "text/fontindex.h"
#include "text/mapped_file.h"
#include "text/sfnt.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rgui::text {

// The fontconfig files the choice reads; empty paths are skipped.
struct FontChoicePaths {
    // The user's file, as `omarchy-font-set` writes it: $XDG_CONFIG_HOME/fontconfig/fonts.conf
    // (default ~/.config/fontconfig/fonts.conf).
    std::string userFontsConf;
    // Omarchy's system file: /etc/fonts/conf.d/50-omarchy.conf.
    std::string omarchyFontsConf;

    // The paths above on Linux; both empty on Windows.
    static FontChoicePaths Default();
};

// A bounded reader of fontconfig XML (1 MiB, depth 64, 65,536 elements; the entities &amp;
// &lt; &gt; &quot; &apos;): the families it makes `monospace` resolve to first. A
// <match target="pattern"> whose tests are all family tests holding <string>monospace</string>
// gives the first <string> of its <edit name="family"> (unless the edit appends); with
// `aliases`, then an <alias> whose <family> is monospace gives the first <family> it prefers,
// accepts or defaults to. Malformed XML gives nothing.
std::vector<std::string> MonospaceFamilies(std::string_view xml, bool aliases);

// The families to try, in order: the user's fonts.conf (matches), 50-omarchy.conf (matches,
// then aliases), then JetBrainsMono Nerd Font, JetBrains Mono, DejaVu Sans Mono, Liberation
// Mono, Noto Sans Mono (Linux) or Cascadia Mono, Consolas, Courier New (Windows). No repeats.
std::vector<std::string> InterfaceFamilyCandidates(const FontChoicePaths& paths);

// The fallback chain for code points the interface family lacks.
// Linux: Noto Sans Mono, DejaVu Sans Mono, Noto Sans Mono CJK SC, Noto Sans CJK SC, Noto Sans,
// DejaVu Sans, Noto Sans Symbols, Noto Sans Symbols 2. Windows: Segoe UI, Microsoft YaHei UI,
// Yu Gothic UI, Malgun Gothic, Segoe UI Symbol, Segoe UI Emoji.
std::vector<std::string> DefaultFallbackFamilies();

// The interface font: regular (400) and bold (700; the regular face when the family has no
// bold) faces of the chosen family, and the fallback chain, each fallback face mapped the first
// time a code point needs it. Not thread-safe: the UI thread owns it (ShapeRun, Resolve). The
// faces stay valid, at the same addresses, as long as the FontSet lives.
class FontSet {
public:
    struct Resolved {
        const FontFace* face = nullptr;
        uint16_t glyph = 0;
    };

    FontSet() = default;
    FontSet(const FontSet&) = delete;
    FontSet& operator=(const FontSet&) = delete;

    // The first candidate the index has, else the index's first fixed-pitch face, else its first
    // face, with Status() `Interface font <first candidate> not found; using <file name>`.
    // False when no face can be opened at all.
    bool Load(const FontIndex& index, const std::vector<std::string>& candidates,
        const std::vector<std::string>& fallbacks);
    bool Load(const FontIndex& index, const FontChoicePaths& paths) {
        return Load(index, InterfaceFamilyCandidates(paths), DefaultFallbackFamilies());
    }

    bool IsLoaded() const { return regular_ != nullptr; }
    const std::string& Family() const { return family_; }
    const std::string& Status() const { return status_; }
    // The main face; Face(true) is the bold one (the regular one when there is none).
    const FontFace& Face(bool bold) const;

    // The face and glyph that show `cp`: the main face when it has it, else the first fallback
    // that does, else glyph 0 of the main face. Cached per code point and weight.
    Resolved Resolve(uint32_t cp, bool bold) const;

private:
    struct Loaded {
        MappedFile file;
        FontFace face;
    };
    struct Fallback {
        std::string family;
        std::unique_ptr<Loaded> faces[2];   // regular, bold
        bool tried[2] = {false, false};
    };

    static std::unique_ptr<Loaded> OpenEntry(const FontEntry& entry);
    const FontFace* FallbackFace(Fallback& fallback, bool bold) const;

    const FontIndex* index_ = nullptr;
    std::string family_;
    std::string status_;
    std::unique_ptr<Loaded> regular_;
    std::unique_ptr<Loaded> bold_;
    mutable std::vector<Fallback> fallbacks_;
    mutable std::unordered_map<uint32_t, Resolved> cache_;
};

} // namespace rgui::text
