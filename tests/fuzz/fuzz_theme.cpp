// SPDX-License-Identifier: Apache-2.0
// Omarchy theme files: colors.toml through the resolver, the theme's and the user's shell.toml
// through the token builder, and ParseColor / MixColorText on every value. The input is the
// three files separated by NUL bytes; the first byte's low bits pick the light.mode file and
// whether Hyprland's rounding is known.
#include "harness.h"

#include "ui/theme.h"
#include "ui/tokens.h"

#include <string_view>

namespace rgui::fuzz {
namespace {

std::string_view NextPart(std::string_view& rest) {
    const size_t nul = rest.find('\0');
    const std::string_view part = rest.substr(0, nul);
    rest = nul == std::string_view::npos ? std::string_view() : rest.substr(nul + 1);
    return part;
}

void ParseValues(const ui::Toml& toml) {
    for (const auto& [key, value] : toml.values) {
        (void)ui::ParseColor(value);
        (void)ui::MixColorText(value, key, 0.25);
    }
}

} // namespace

int FuzzTheme(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const uint8_t flags = data[0];
    std::string_view rest(reinterpret_cast<const char*>(data + 1), size - 1);
    const std::string_view colorsText = NextPart(rest);
    const std::string_view shellText = NextPart(rest);
    const std::string_view userText = NextPart(rest);

    ui::Toml colors;
    ui::ParseColorsToml(colorsText, colors);
    ui::Toml colorsSections;
    ui::ParseToml(colorsText, colorsSections, false);
    ui::Toml shell;
    const bool haveShell = ui::ParseToml(shellText, shell);
    ui::Toml user;
    const bool haveUser = ui::ParseToml(userText, user);
    ParseValues(colors);
    ParseValues(shell);

    const ui::Palette palette = ui::ResolvePalette(colors, (flags & 1) != 0);
    ui::TokenSources sources;
    sources.themeShell = haveShell ? &shell : nullptr;
    sources.userShell = haveUser ? &user : nullptr;
    if (flags & 2) sources.hyprlandRounding = static_cast<int>(flags >> 2) - 8;
    sources.themeRounding = flags >> 4;
    sources.systemTextScale = 0.5 + (flags >> 5) * 0.5;
    const ui::Tokens tokens = ui::BuildTokens(palette, sources);
    (void)ui::ContrastRatio(tokens.colors.text, tokens.colors.accent);
    return 0;
}

} // namespace rgui::fuzz
