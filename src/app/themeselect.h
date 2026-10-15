// SPDX-License-Identifier: Apache-2.0
// Which theme the app wears (plan Phase 8 step 7): the current Omarchy theme, the system's light
// or dark choice among the bundled themes, or one bundled theme by slug; and the Interface
// section's Theme menu.
#pragma once

#include "ui/theme.h"
#include "ui/tokens.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rgui {

// The `ui.theme` values besides a bundled slug.
inline constexpr const char* kThemeOmarchy = "omarchy";
inline constexpr const char* kThemeAuto = "auto";
inline constexpr const char* kThemeFallbackSlug = "tokyo-night";      // dark, and Omarchy's default
inline constexpr const char* kThemeLightSlug = "catppuccin-latte";   // automatic on a light system
inline constexpr const char* kOmarchyUnreadable = "Omarchy theme unreadable; using Tokyo Night";

struct ThemeRequest {
    // `omarchy`, `auto` or a bundled slug. `omarchy` and `auto` both mean the menu's first
    // entry: follow Omarchy when its theme directory exists, else automatic (on Omarchy its
    // theme is the system's). An unknown value means the same.
    std::string setting = kThemeOmarchy;
    std::optional<std::filesystem::path> omarchyThemeDir;      // Platform::OmarchyThemeDir()
    std::optional<std::filesystem::path> userShellToml;        // Platform::OmarchyUserShellToml()
    bool systemDark = true;
    double systemTextScale = 1.0;
    std::optional<int> hyprlandRounding;
};

struct LoadedTheme {
    ui::Palette palette;
    ui::Tokens tokens;
    // What is worn: `omarchy` or a bundled slug.
    std::string source;
    // Non-empty when the choice could not be honoured: kOmarchyUnreadable.
    std::string warning;
};

// Following Omarchy: <dir>/colors.toml (and light.mode beside it) through Omarchy's resolver,
// tokens from <dir>/shell.toml; an unreadable colors.toml gives tokyo-night and
// kOmarchyUnreadable. Automatic: tokyo-night on a dark system, catppuccin-latte on a light one.
// A slug: that bundled theme with its shell.toml and rounding. The user's shell.toml overrides
// the tokens in every case.
LoadedTheme LoadTheme(const ThemeRequest& request);

// The Theme menu: `Follow Omarchy` (with an Omarchy theme directory) or `Automatic`, then the
// bundled themes' names in slug order.
std::vector<std::string> ThemeMenuItems(bool omarchy);
// The menu entry of a `ui.theme` value: 0 for `omarchy`, `auto` and unknown values.
int ThemeMenuIndex(std::string_view setting);
// The `ui.theme` value of a menu entry.
std::string ThemeSettingOf(int index, bool omarchy);

} // namespace rgui
