// SPDX-License-Identifier: Apache-2.0
// Omarchy themes: the colors.toml reader and palette resolver ported from Omarchy's own
// bin/omarchy-theme-color, colour parsing, and the themes bundled from Omarchy.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace rgui::ui {

// A straight-alpha colour.
struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    // The same colour with its alpha multiplied by `alpha` (clamped to 0..1).
    Rgba WithAlpha(double alpha) const;
    // As a premultiplied 0xAARRGGBB pixel (gfx::Premultiply).
    uint32_t Premultiplied() const;
    bool operator==(const Rgba& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
    bool operator!=(const Rgba& other) const { return !(*this == other); }
};

// A flat TOML-ish key = value table: "section.key" -> raw value (sections from `[section]`).
struct Toml {
    std::map<std::string, std::string, std::less<>> values;

    const std::string* Find(std::string_view key) const;
};

inline constexpr size_t kMaxTomlBytes = 256 * 1024;

// Reads `key = value` lines the way bin/omarchy-theme-color reads colors.toml: the key loses its
// quotes and spaces; empty keys and keys starting with `#` are skipped; a value holding a quote
// is the text between its first quote and the next one, else it is trimmed; keys outside
// [A-Za-z0-9_-] and values outside Omarchy's value charset are dropped; a later key wins.
// `[section]` lines prefix the keys that follow with `section.` when `sections` is true
// (shell.toml, where an unquoted value may also carry a `#` comment, as in the shell's own
// reader); colors.toml has no sections. Text over kMaxTomlBytes is refused (false, `out` untouched).
bool ParseToml(std::string_view text, Toml& out, bool sections = true);

// A file through ParseToml; false when it is missing, not a regular file (ReadRegularFile: a
// FIFO or device is refused at once), unreadable or over kMaxTomlBytes.
bool ReadTomlFile(const std::filesystem::path& path, Toml& out, bool sections = true);

// Parses colors.toml as the resolver does: `while read` skips a last line without a newline.
bool ParseColorsToml(std::string_view text, Toml& out);

// Accepts #rgb, #rrggbb, #rrggbbaa, rgb(r,g,b), rgba(r,g,b,a) with a 0-1 alpha, Hyprland's
// rgb(rrggbb), rgba(rrggbbaa) and 0xaarrggbb. For a gradient ("<c1> <c2> 45deg") the first
// colour is used. Spaces around the parts and inside the parentheses are allowed.
std::optional<Rgba> ParseColor(std::string_view text);

// The awk mix_color of bin/omarchy-theme-color, digit for digit, for `amount` a fraction (the
// script's "25%" is 0.25): each channel is int(start * (1 - amount) + end * amount + 0.5) and the
// result "#rrggbb" in gawk's %02x, so a malformed input gives what the script gives.
std::string MixColorText(std::string_view start, std::string_view end, double amount);

// mix(a, b, t) = a + (b - a) * t per channel, rounded; alpha from `a`. (hype's mix.)
Rgba Mix(Rgba a, Rgba b, double t);

// A resolved Omarchy palette: every key bin/omarchy-theme-color --all prints, by name.
struct Palette {
    std::map<std::string, std::string, std::less<>> values;
    bool dark = true;

    // The raw value ("" when absent).
    const std::string& Get(std::string_view key) const;
    // The value as a colour; `fallback` when absent or unparsable.
    Rgba Color(std::string_view key, Rgba fallback = {}) const;

    Rgba Background() const;
    Rgba DarkBackground() const;
    Rgba Foreground() const;
    Rgba Accent() const;
    Rgba Selection() const;
    Rgba SelectionForeground() const;
    Rgba Cursor() const;
    Rgba Muted() const;
    Rgba Red() const;
    Rgba Yellow() const;
    Rgba Green() const;
};

// resolve_theme_colors and resolve_theme_mode of bin/omarchy-theme-color over parsed colors.toml
// values. `lightModeFile`: a light.mode file sits beside the colors.toml.
Palette ResolvePalette(const Toml& colors, bool lightModeFile);

#include "ui/omarchy_themes.inc"

// The bundled theme with this slug, or null.
const BundledTheme* FindBundledTheme(std::string_view slug);
// The palette of a bundled theme.
Palette BundledPalette(const BundledTheme& theme);

} // namespace rgui::ui
