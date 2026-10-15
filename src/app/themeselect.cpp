// SPDX-License-Identifier: Apache-2.0
#include "app/themeselect.h"

#include <system_error>

namespace rgui {

namespace {

namespace fs = std::filesystem;

bool IsFollowSetting(std::string_view setting) {
    return setting == kThemeOmarchy || setting == kThemeAuto || ui::FindBundledTheme(setting) == nullptr;
}

ui::TokenSources Sources(const ThemeRequest& request, const ui::Toml* themeShell, const ui::Toml* userShell,
                         int themeRounding) {
    ui::TokenSources sources;
    sources.themeShell = themeShell;
    sources.userShell = userShell;
    sources.hyprlandRounding = request.hyprlandRounding;
    sources.themeRounding = themeRounding;
    sources.systemTextScale = request.systemTextScale;
    return sources;
}

void WearBundled(const ui::BundledTheme& theme, const ThemeRequest& request, const ui::Toml* userShell,
                 LoadedTheme& out) {
    ui::Toml shell;
    const bool hasShell = theme.shell != nullptr && ui::ParseToml(theme.shell, shell, true);
    out.palette = ui::BundledPalette(theme);
    out.tokens = ui::BuildTokens(out.palette, Sources(request, hasShell ? &shell : nullptr, userShell, theme.rounding));
    out.source = theme.slug;
}

} // namespace

LoadedTheme LoadTheme(const ThemeRequest& request) {
    LoadedTheme out;
    ui::Toml user;
    const bool hasUser = request.userShellToml && ui::ReadTomlFile(*request.userShellToml, user, true);
    const ui::Toml* userShell = hasUser ? &user : nullptr;

    if (!IsFollowSetting(request.setting)) {
        WearBundled(*ui::FindBundledTheme(request.setting), request, userShell, out);
        return out;
    }
    if (request.omarchyThemeDir) {
        const fs::path& dir = *request.omarchyThemeDir;
        ui::Toml colors;
        if (ui::ReadTomlFile(dir / "colors.toml", colors, false)) {
            std::error_code error;
            const bool lightMode = fs::exists(dir / "light.mode", error);
            ui::Toml shell;
            const bool hasShell = ui::ReadTomlFile(dir / "shell.toml", shell, true);
            out.palette = ui::ResolvePalette(colors, lightMode);
            out.tokens = ui::BuildTokens(out.palette, Sources(request, hasShell ? &shell : nullptr, userShell, 0));
            out.source = kThemeOmarchy;
            return out;
        }
        WearBundled(*ui::FindBundledTheme(kThemeFallbackSlug), request, userShell, out);
        out.warning = kOmarchyUnreadable;
        return out;
    }
    WearBundled(*ui::FindBundledTheme(request.systemDark ? kThemeFallbackSlug : kThemeLightSlug), request, userShell,
                out);
    return out;
}

std::vector<std::string> ThemeMenuItems(bool omarchy) {
    std::vector<std::string> items;
    items.emplace_back(omarchy ? "Follow Omarchy" : "Automatic");
    for (const ui::BundledTheme& theme : ui::kBundledThemes) items.emplace_back(theme.name);
    return items;
}

int ThemeMenuIndex(std::string_view setting) {
    int index = 1;
    for (const ui::BundledTheme& theme : ui::kBundledThemes) {
        if (setting == theme.slug) return index;
        index++;
    }
    return 0;
}

std::string ThemeSettingOf(int index, bool omarchy) {
    constexpr int count = static_cast<int>(sizeof(ui::kBundledThemes) / sizeof(ui::kBundledThemes[0]));
    if (index >= 1 && index <= count) return ui::kBundledThemes[index - 1].slug;
    return omarchy ? kThemeOmarchy : kThemeAuto;
}

} // namespace rgui
