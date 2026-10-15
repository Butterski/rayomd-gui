// SPDX-License-Identifier: Apache-2.0
#include "runner.h"

#include "common/file_io.h"
#include "ui/settingsfile.h"
#include "ui/theme.h"
#include "ui/tokens.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

using rgui::ui::Palette;
using rgui::ui::Rgba;
using rgui::ui::Toml;

std::filesystem::path ThemeData() { return std::filesystem::path(rgui::test::SourceDir()) / "tests/data/themes"; }

std::filesystem::path TempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::path(rgui::test::BuildDir()) / "test-tmp" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

// Every key<TAB>value line of an .expect file resolves to the same value, and the palette has no
// other keys.
void CheckExpectation(const Palette& palette, const std::filesystem::path& expectFile) {
    std::string text;
    CHECK(RayoMd::File::ReadAll(expectFile, 1 << 20, text));
    size_t lines = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t tab = line.find('\t');
        CHECK(tab != std::string::npos);
        if (tab == std::string::npos) continue;
        const std::string key = line.substr(0, tab);
        const std::string expected = line.substr(tab + 1);
        const bool present = palette.values.count(key) != 0;
        if (!present || palette.Get(key) != expected) {
            std::printf("  %s: key %s\n", expectFile.filename().string().c_str(), key.c_str());
        }
        CHECK(present);
        CHECK_EQ(palette.Get(key), expected);
        ++lines;
    }
    CHECK_EQ(palette.values.size(), lines);
}

Rgba Hex(uint32_t rgb, uint8_t a = 255) {
    return Rgba{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb), a};
}

Toml ParseShell(const char* text) {
    Toml toml;
    CHECK(rgui::ui::ParseToml(text, toml));
    return toml;
}

} // namespace

RGUI_TEST("ui.theme_resolver_bundled") {
    size_t count = 0;
    for (const auto& theme : rgui::ui::kBundledThemes) {
        const auto expect = ThemeData() / (std::string(theme.slug) + ".expect");
        CHECK(std::filesystem::exists(expect));
        CheckExpectation(rgui::ui::BundledPalette(theme), expect);
        ++count;
    }
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(ThemeData())) {
        files += entry.path().extension() == ".expect" ? 1 : 0;
    }
    CHECK_EQ(count, size_t(23));
    CHECK_EQ(files, count);
}

RGUI_TEST("ui.theme_resolver_fixtures") {
    // Legacy ANSI-only and short-name palettes, mode detection by theme_type, light.mode and
    // luminance, and parser corner cases, each against the script's own output.
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(ThemeData() / "fixtures")) {
        if (!entry.is_directory()) continue;
        Toml colors;
        CHECK(rgui::ui::ReadTomlFile(entry.path() / "colors.toml", colors, false));
        const bool light = std::filesystem::exists(entry.path() / "light.mode");
        const auto expect = entry.path().parent_path() / (entry.path().filename().string() + ".expect");
        CheckExpectation(rgui::ui::ResolvePalette(colors, light), expect);
        ++count;
    }
    CHECK_EQ(count, size_t(5));
}

RGUI_TEST("ui.theme_modes") {
    for (const auto& theme : rgui::ui::kBundledThemes) {
        const std::string slug = theme.slug;
        const bool light = slug == "catppuccin-latte" || slug == "flexoki-light" || slug == "lupine" ||
                           slug == "rose-pine" || slug == "white";
        CHECK_EQ(rgui::ui::BundledPalette(theme).dark, !light);
    }
    const auto* tokyo = rgui::ui::FindBundledTheme("tokyo-night");
    CHECK(tokyo != nullptr);
    if (tokyo) {
        CHECK_EQ(std::string(tokyo->name), std::string("Tokyo Night"));
        CHECK(rgui::ui::BundledPalette(*tokyo).Background() == Hex(0x1a1b26));
    }
    const auto* retro = rgui::ui::FindBundledTheme("retro-82");
    CHECK(retro && std::string(retro->name) == "Retro 82");
    const auto* solitude = rgui::ui::FindBundledTheme("solitude");
    CHECK(solitude && solitude->rounding == 6);
    CHECK(rgui::ui::FindBundledTheme("no-such-theme") == nullptr);
}

RGUI_TEST("ui.parse_color") {
    using rgui::ui::ParseColor;
    CHECK(ParseColor("#abc") == Hex(0xaabbcc));
    CHECK(ParseColor("#1A1b26") == Hex(0x1a1b26));
    CHECK(ParseColor("#7aa2f780") == Hex(0x7aa2f7, 0x80));
    CHECK(ParseColor("rgb(10,20,30)") == Hex(0x0a141e));
    CHECK(ParseColor("rgb( 10, 20 ,30 )") == Hex(0x0a141e));
    CHECK(ParseColor("rgba(10,20,30,0.5)") == Hex(0x0a141e, 128));
    CHECK(ParseColor("rgba(10,20,30,1)") == Hex(0x0a141e, 255));
    CHECK(ParseColor("rgba(300,20,30,2)") == Hex(0xff141e, 255));  // clamped like Omarchy's scripts
    CHECK(ParseColor("rgba(798186ee)") == Hex(0x798186, 0xee));
    CHECK(ParseColor("RGB(dcd7ba)") == Hex(0xdcd7ba));
    CHECK(ParseColor("0xff112233") == Hex(0x112233, 0xff));
    CHECK(ParseColor("  #ffffff  ") == Hex(0xffffff));
    // A gradient gives its first colour; an angle first is skipped.
    CHECK(ParseColor("rgba(26a269ee) rgba(2ec27eee) 45deg") == Hex(0x26a269, 0xee));
    CHECK(ParseColor("-12.5deg #102030 #405060") == Hex(0x102030));
    CHECK(ParseColor("rgba(1, 2, 3, 0.5) #ffffff") == Hex(0x010203, 128));
    // Refused.
    CHECK(!ParseColor(""));
    CHECK(!ParseColor("#12"));
    CHECK(!ParseColor("#12345"));
    CHECK(!ParseColor("#ggg"));
    CHECK(!ParseColor("rgb(1,2)"));
    CHECK(!ParseColor("rgb(1,2,3,4,5)"));
    CHECK(!ParseColor("rgba(1,2,3,x)"));
    CHECK(!ParseColor("rgb(1,2,3"));
    CHECK(!ParseColor("hsl(1,2,3)"));
    CHECK(!ParseColor("accent"));
    CHECK(!ParseColor("45deg"));
}

RGUI_TEST("ui.mix") {
    // mix_color of the script, including its rounding and what it does with a short colour.
    CHECK_EQ(rgui::ui::MixColorText("#1a1b26", "#000000", 0.25), std::string("#14141d"));
    CHECK_EQ(rgui::ui::MixColorText("#eb927b", "#000000", 0.5), std::string("#76493e"));
    CHECK_EQ(rgui::ui::MixColorText("#abc", "#ffffff", 0.2), std::string("#bccd33"));
    CHECK_EQ(rgui::ui::MixColorText("", "#000000", 0.5), std::string("#000000"));
    CHECK_EQ(rgui::ui::MixColorText("#gg0000", "#000000", 0.5), std::string("#fffffffffffffff80000"));
    CHECK(rgui::ui::Mix(Hex(0x000000), Hex(0xffffff), 0.5) == Hex(0x808080));
    CHECK(rgui::ui::Mix(Hex(0x1a1b26), Hex(0xa9b1d6), 0.62) == Hex(0x737893));
}

RGUI_TEST("ui.toml") {
    Toml toml;
    CHECK(rgui::ui::ParseToml("top = 1\n[font]\nbase-size = 15 # bigger\n"
                              "[ controls ]\n[spacing] # spacing\nscale = \"1.5\"\n"
                              "bad key = 3\nnote = 'a;b'\n",
                              toml));
    CHECK_EQ(toml.values.size(), size_t(4));
    // As in the script, spaces inside a key are dropped; a value with `;` is dropped.
    CHECK(toml.Find("spacing.badkey") && *toml.Find("spacing.badkey") == "3");
    CHECK(!toml.Find("spacing.note"));
    CHECK(toml.Find("top") && *toml.Find("top") == "1");
    CHECK(toml.Find("font.base-size") && *toml.Find("font.base-size") == "15");
    CHECK(toml.Find("spacing.scale") && *toml.Find("spacing.scale") == "1.5");
    // "[ controls ]" is not a section header (and its key has spaces stripped to "[controls]").
    CHECK(!toml.Find("controls.scale"));

    // colors.toml ignores sections, and like `while read` its last line needs a newline.
    Toml colors;
    CHECK(rgui::ui::ParseColorsToml("[x]\na = \"#111111\"\nb = \"#222222\"", colors));
    CHECK(colors.Find("a") && *colors.Find("a") == "#111111");
    CHECK(!colors.Find("x.a"));
    CHECK(!colors.Find("b"));

    Toml big;
    CHECK(!rgui::ui::ParseToml(std::string(rgui::ui::kMaxTomlBytes + 1, '\n'), big));
    CHECK(rgui::ui::ParseToml(std::string(rgui::ui::kMaxTomlBytes, '\n'), big));
}

RGUI_TEST("ui.tokens_defaults") {
    const auto* tokyo = rgui::ui::FindBundledTheme("tokyo-night");
    CHECK(tokyo != nullptr);
    if (!tokyo) return;
    const Palette palette = rgui::ui::BundledPalette(*tokyo);
    const Rgba bg = Hex(0x1a1b26), fg = Hex(0xa9b1d6), accent = Hex(0x7aa2f7);
    const rgui::ui::Tokens t = rgui::ui::BuildTokens(palette, {});

    // [font] at base-size 12: the template's commented sizes.
    CHECK_EQ(t.font.baseSize, 12);
    CHECK_EQ(t.font.caption, 10);
    CHECK_EQ(t.font.bodySmall, 11);
    CHECK_EQ(t.font.body, 12);
    CHECK_EQ(t.font.subtitle, 13);
    CHECK_EQ(t.font.title, 14);
    CHECK_EQ(t.font.heading, 16);
    CHECK_EQ(t.font.display, 24);
    CHECK_EQ(t.font.displayLarge, 28);
    CHECK_EQ(t.font.iconLarge, 18);
    // [spacing]: the commented per-token values at scale 1.
    CHECK_EQ(t.spacing.xxs, 2);
    CHECK_EQ(t.spacing.sm, 4);
    CHECK_EQ(t.spacing.md, 6);
    CHECK_EQ(t.spacing.huge, 18);
    CHECK_EQ(t.spacing.controlGap, 8);
    CHECK_EQ(t.spacing.controlPaddingX, 10);
    CHECK_EQ(t.spacing.inputPaddingY, 7);
    CHECK_EQ(t.spacing.controlHeight, 28);
    CHECK_EQ(t.spacing.popupRowHeight, 28);
    CHECK_EQ(t.spacing.rowGap, 8);
    CHECK_EQ(t.spacing.panelGap, 14);
    CHECK_EQ(t.spacing.panelPadding, 18);
    CHECK_EQ(t.spacing.dropdownWidth, 240);
    CHECK_EQ(t.spacing.numberFieldWidth, 120);
    // [controls].
    CHECK(t.normal.Fill() == fg.WithAlpha(0.04));
    CHECK(t.normal.border == fg.WithAlpha(0.4));
    CHECK_EQ(t.normal.borderWidth, 1);
    CHECK(t.hoverCursor.Fill() == fg.WithAlpha(0.08));
    CHECK(t.hoverCursor.border == fg.WithAlpha(0.25));
    CHECK_EQ(t.hoverCursor.borderWidth, 1);
    CHECK(t.focus.Fill() == t.hoverCursor.Fill());
    CHECK(t.focus.border == t.hoverCursor.border);
    CHECK(t.selected.Fill() == fg.WithAlpha(0.18));
    CHECK_EQ(t.selected.borderWidth, 0);
    CHECK(t.pressedFill == fg.WithAlpha(0.22));
    CHECK(t.selectionFill == fg.WithAlpha(0.35));
    CHECK_EQ(int(t.normal.Fill().a), 10);  // 0.04 x 255
    // Surfaces: popups take the Hyprland active border (the accent here), tooltips the foreground.
    CHECK(t.popups.background == bg);
    CHECK(t.popups.text == fg);
    CHECK(t.popups.border == accent);
    CHECK_EQ(t.popups.borderWidth, 2);
    CHECK(t.tooltip.background == bg.WithAlpha(0.97));
    CHECK(t.tooltip.border == fg);
    CHECK_EQ(t.tooltip.borderWidth, 1);
    CHECK(t.menu.scrim == bg.WithAlpha(0.5));
    CHECK(t.menu.selectedBackground == fg.WithAlpha(0.08));
    CHECK(t.menu.selectedText == accent);
    CHECK(t.menu.border == fg);
    // Derived colours.
    CHECK(t.colors.windowBackground == bg);
    CHECK(t.colors.mutedText == rgui::ui::Mix(bg, fg, 0.62));
    CHECK(t.colors.previewBackground == Hex(0x13141c));
    CHECK(t.colors.hairline == fg.WithAlpha(0.12));
    CHECK(t.colors.selection == Hex(0x292e42));
    CHECK(t.colors.selectedText == Hex(0xc0caf5));
    CHECK(t.colors.caret == Hex(0xc0caf5));
    CHECK(t.colors.accentHover == rgui::ui::Mix(accent, fg, 0.15));
    CHECK(t.colors.accentPressed == rgui::ui::Mix(accent, bg, 0.15));
    CHECK(t.colors.textOnAccent == bg);  // dark text on Tokyo Night's light blue
    CHECK(t.colors.error == Hex(0xf7768e));
    CHECK(t.colors.warning == Hex(0xe0af68));
    CHECK(t.colors.success == Hex(0x9ece6a));
    CHECK(t.dark);
    // No Omarchy shell.toml: the system text scale applies, clamped.
    rgui::ui::TokenSources sources;
    sources.systemTextScale = 1.25;
    CHECK_NEAR(rgui::ui::BuildTokens(palette, sources).textScale, 1.25, 1e-12);
    sources.systemTextScale = 9;
    CHECK_NEAR(rgui::ui::BuildTokens(palette, sources).textScale, 3.0, 1e-12);
    sources.systemTextScale = 0.1;
    CHECK_NEAR(rgui::ui::BuildTokens(palette, sources).textScale, 0.5, 1e-12);
}

RGUI_TEST("ui.tokens_text_on_accent") {
    // A light theme with a dark accent: the light background is the readable text.
    const auto* latte = rgui::ui::FindBundledTheme("catppuccin-latte");
    CHECK(latte != nullptr);
    if (!latte) return;
    const Palette palette = rgui::ui::BundledPalette(*latte);
    const auto t = rgui::ui::BuildTokens(palette, {});
    const Rgba other = t.colors.textOnAccent == palette.Background() ? palette.Foreground() : palette.Background();
    CHECK(rgui::ui::ContrastRatio(t.colors.textOnAccent, palette.Accent()) >=
          rgui::ui::ContrastRatio(other, palette.Accent()));
    CHECK_NEAR(rgui::ui::ContrastRatio(Hex(0x000000), Hex(0xffffff)), 21.0, 1e-6);
    CHECK_NEAR(rgui::ui::ContrastRatio(Hex(0x777777), Hex(0x777777)), 1.0, 1e-12);
}

RGUI_TEST("ui.tokens_gradient_border") {
    // Hackerman's hyprland_active_border is a gradient: popups take its first colour, with the
    // border alpha replacing the colour's own.
    const auto* hackerman = rgui::ui::FindBundledTheme("hackerman");
    CHECK(hackerman != nullptr);
    if (!hackerman) return;
    const Palette palette = rgui::ui::BundledPalette(*hackerman);
    const Toml merged = rgui::ui::MergedShellValues(palette, {});
    CHECK(merged.Find("hyprland.active-border") &&
          *merged.Find("hyprland.active-border") == "rgba(26a269ee) rgba(2ec27eee) 45deg");
    CHECK(merged.Find("lock.placeholder") && *merged.Find("lock.placeholder") ==
                                                 rgui::ui::MixColorText(palette.Get("foreground"), palette.Get("background"), 0.34));
    const auto t = rgui::ui::BuildTokens(palette, {});
    CHECK(t.popups.border == Hex(0x26a269));
    CHECK(t.tooltip.border == Hex(0x26a269));
}

RGUI_TEST("ui.tokens_overrides") {
    const auto* tokyo = rgui::ui::FindBundledTheme("tokyo-night");
    CHECK(tokyo != nullptr);
    if (!tokyo) return;
    const Palette palette = rgui::ui::BundledPalette(*tokyo);

    // The theme's shell.toml, then the user's ~/.config/omarchy/shell.toml, key by key.
    const Toml theme = ParseShell("[font]\nbase-size = 13\n[controls]\nnormal-fill-alpha = 0.1\n"
                                  "hover-cursor-color = \"accent\"\n[tooltip]\nborder = \"#ff0000\"\n"
                                  "border-alpha = 0.5\nborder-width = 3\n");
    const Toml user = ParseShell("[font]\nbase-size = 15\n");
    rgui::ui::TokenSources sources;
    sources.themeShell = &theme;
    sources.userShell = &user;
    sources.systemTextScale = 2.0;
    auto t = rgui::ui::BuildTokens(palette, sources);
    CHECK_EQ(t.font.baseSize, 15);
    CHECK_EQ(t.font.body, 15);
    CHECK_EQ(t.font.caption, 12);  // round(15 x 0.833)
    CHECK_EQ(t.font.heading, 20);
    CHECK_EQ(t.spacing.controlHeight, 35);
    CHECK_EQ(t.spacing.controlPaddingX, 13);  // 12.5 rounds up
    CHECK_EQ(t.spacing.rowGap, 10);
    CHECK_EQ(t.popups.borderWidth, 3);  // max(1, space(2)) = round(2.5)
    CHECK(t.normal.Fill() == Hex(0xa9b1d6).WithAlpha(0.1));
    CHECK(t.hoverCursor.Fill() == Hex(0x7aa2f7).WithAlpha(0.08));
    CHECK(t.focus.color == Hex(0xa9b1d6));  // the template's own focus-color stays foreground
    CHECK(t.tooltip.border == Hex(0xff0000).WithAlpha(0.5));
    CHECK_EQ(t.tooltip.borderWidth, 3);
    CHECK_NEAR(t.textScale, 1.0, 1e-12);  // an Omarchy shell.toml carries the text size

    // A per-token px override wins over the scale.
    const Toml pinned = ParseShell("[font]\nbase-size = 15\nheading = 40\n[spacing]\ncontrol-height = 30\n");
    sources.userShell = &pinned;
    t = rgui::ui::BuildTokens(palette, sources);
    CHECK_EQ(t.font.heading, 40);
    CHECK_EQ(t.font.body, 15);
    CHECK_EQ(t.spacing.controlHeight, 30);
    CHECK_EQ(t.spacing.rowGap, 10);

    // scale-with-font off and an explicit scale.
    const Toml scaled = ParseShell("[font]\nbase-size = 15\n[spacing]\nscale = 1.5\nscale-with-font = false\n");
    sources.userShell = &scaled;
    t = rgui::ui::BuildTokens(palette, sources);
    CHECK_EQ(t.spacing.controlHeight, 42);
    CHECK_EQ(t.font.body, 15);

    // Malformed values fall back to the defaults.
    const Toml junk = ParseShell("[font]\nbase-size = big\n[spacing]\nscale = -2\n[controls]\nnormal-fill-alpha = lots\n");
    sources.themeShell = nullptr;
    sources.userShell = &junk;
    t = rgui::ui::BuildTokens(palette, sources);
    CHECK_EQ(t.font.body, 12);
    CHECK_EQ(t.spacing.controlHeight, 28);
    CHECK(t.normal.Fill() == Hex(0xa9b1d6).WithAlpha(0.04));
}

RGUI_TEST("ui.tokens_radius") {
    const Palette palette = rgui::ui::BundledPalette(*rgui::ui::FindBundledTheme("solitude"));
    rgui::ui::TokenSources sources;
    sources.themeRounding = 6;
    auto t = rgui::ui::BuildTokens(palette, sources);
    CHECK_EQ(t.radius, 6);
    CHECK_EQ(t.controlRadius, 4);
    sources.hyprlandRounding = 2;
    t = rgui::ui::BuildTokens(palette, sources);
    CHECK_EQ(t.radius, 2);
    CHECK_EQ(t.controlRadius, 2);
    sources.hyprlandRounding = 0;
    t = rgui::ui::BuildTokens(palette, sources);
    CHECK_EQ(t.radius, 0);
    t = rgui::ui::BuildTokens(palette, {});
    CHECK_EQ(t.radius, 0);
}

RGUI_TEST("ui.settings_round_trip") {
    const auto dir = TempDir("ui-settings");
    const auto path = dir / "nested" / "deeper" / "settings.ini";
    rgui::ui::SettingsFile settings;
    CHECK(settings.SetString("ui.theme", "tokyo-night"));
    CHECK(settings.SetString("ui.lastDirectory", "/home/zoë/Документы/a=b #c"));
    CHECK(settings.SetString("export.themeFile", ""));
    CHECK(settings.SetInt("export.tocDepth", -3));
    CHECK(settings.SetDouble("ui.splitRatio", 0.1 + 0.2));
    CHECK(settings.SetDouble("export.customWidthMm", 5080));
    CHECK(settings.SetBool("export.pdfa", true));
    CHECK(settings.SetBool("export.compress", false));
    CHECK(settings.Save(path));
    CHECK(std::filesystem::exists(path));
    CHECK(!std::filesystem::exists(dir / "nested" / "deeper" / "settings.ini.tmp"));

    rgui::ui::SettingsFile loaded;
    CHECK(loaded.Load(path));
    CHECK_EQ(loaded.GetString("ui.theme"), std::string("tokyo-night"));
    CHECK_EQ(loaded.GetString("ui.lastDirectory"), std::string("/home/zoë/Документы/a=b #c"));
    CHECK(loaded.Has("export.themeFile"));
    CHECK_EQ(loaded.GetString("export.themeFile", "x"), std::string());
    CHECK_EQ(loaded.GetInt("export.tocDepth", 0), -3);
    CHECK(loaded.GetDouble("ui.splitRatio", 0) == 0.1 + 0.2);  // exact round trip
    CHECK(loaded.GetDouble("export.customWidthMm", 0) == 5080.0);
    CHECK(loaded.GetBool("export.pdfa", false));
    CHECK(!loaded.GetBool("export.compress", true));
    CHECK_EQ(loaded.GetInt("missing", 7), 7);
    CHECK(loaded.Values() == settings.Values());

    // Saving again replaces the file atomically.
    loaded.Erase("ui.theme");
    CHECK(loaded.Save(path));
    rgui::ui::SettingsFile again;
    CHECK(again.Load(path));
    CHECK(!again.Has("ui.theme"));
    CHECK(!std::filesystem::exists(dir / "nested" / "deeper" / "settings.ini.tmp"));
}

RGUI_TEST("ui.settings_malformed") {
    const auto dir = TempDir("ui-settings-malformed");
    const auto path = dir / "settings.ini";
    CHECK(RayoMd::File::WriteAll(path, "# comment=1\n"
                                       "\n"
                                       "no equals sign\n"
                                       "=empty key\n"
                                       "bad key=1\n"
                                       "tab\tkey=1\n"
                                       "ctl=a\x01" "b\n"
                                       "ui.ok=yes\r\n"
                                       "ui.int=12x\n"
                                       "ui.double=nan\n"
                                       "ui.bool=maybe\n"
                                       "ui.ok2=a=b\n"
                                       "ui.dup=1\n"
                                       "ui.dup=2\n"
                                       "  # not a comment=3\n"
                                       "last=no newline"));
    rgui::ui::SettingsFile settings;
    CHECK(settings.Load(path));
    CHECK_EQ(settings.Values().size(), size_t(7));
    CHECK_EQ(settings.GetString("ui.ok"), std::string("yes"));
    CHECK_EQ(settings.GetString("ui.ok2"), std::string("a=b"));
    CHECK_EQ(settings.GetString("ui.dup"), std::string("2"));
    CHECK_EQ(settings.GetString("last"), std::string("no newline"));
    CHECK_EQ(settings.GetInt("ui.int", 5), 5);
    CHECK_NEAR(settings.GetDouble("ui.double", 1.5), 1.5, 0);
    CHECK(settings.GetBool("ui.bool", true));
    CHECK(!settings.GetBool("ui.bool", false));
    CHECK(!settings.Has("bad key"));
    CHECK(!settings.Has("ctl"));
    CHECK(!settings.Has(""));

    // Setters refuse what the file could not hold.
    CHECK(!settings.SetString("", "x"));
    CHECK(!settings.SetString("a=b", "x"));
    CHECK(!settings.SetString("#a", "x"));
    CHECK(!settings.SetString("a b", "x"));
    CHECK(!settings.SetString("a", "line\nbreak"));
    CHECK(!settings.SetString("a", "\xff\xfe"));
    CHECK(!settings.SetDouble("a", 1e308 * 10));
    CHECK(!settings.Has("a"));

    // Invalid UTF-8 and oversize files are refused whole; a missing file loads nothing.
    CHECK(RayoMd::File::WriteAll(path, "ui.a=\xc3\x28\n"));
    CHECK(!settings.Load(path));
    CHECK(settings.Values().empty());
    std::string big = "ui.a=1\n";
    big.resize(rgui::ui::SettingsFile::kMaxBytes + 1, '#');
    CHECK(RayoMd::File::WriteAll(path, big));
    CHECK(!settings.Load(path));
    CHECK(settings.Values().empty());
    big.resize(rgui::ui::SettingsFile::kMaxBytes);
    CHECK(RayoMd::File::WriteAll(path, big));
    CHECK(settings.Load(path));
    CHECK_EQ(settings.GetInt("ui.a", 0), 1);
    CHECK(!settings.Load(dir / "missing.ini"));
}

RGUI_TEST("ui.settings_save_failure") {
    // A parent that is a file cannot become a directory: Save fails and leaves nothing behind.
    const auto dir = TempDir("ui-settings-failure");
    CHECK(RayoMd::File::WriteAll(dir / "file", "x"));
    rgui::ui::SettingsFile settings;
    CHECK(settings.SetInt("a", 1));
    CHECK(!settings.Save(dir / "file" / "settings.ini"));
    // The target is a directory: the rename fails and the .tmp is removed.
    std::error_code error;
    std::filesystem::create_directories(dir / "target.ini" / "child", error);
    CHECK(!settings.Save(dir / "target.ini"));
    CHECK(!std::filesystem::exists(dir / "target.ini.tmp"));
}

RGUI_TEST("ui.theme_parser_hardening") {
    // 1,000 random byte mutations of a theme's colors.toml and of a shell.toml: whatever the
    // bytes, the readers keep only well-formed keys and the resolver always decides a mode.
    const auto* theme = rgui::ui::FindBundledTheme("hackerman");
    CHECK(theme != nullptr);
    if (!theme) return;
    const std::string colorsText = theme->colors;
    const std::string shellText = "[font]\nbase-size = 15 # big\n[controls]\nnormal-border = \"rgba(1, 2, 3, 0.5) 45deg\"\n"
                                  "[popups]\nborder = \"hyprland.active-border\"\nborder-width = 3\n"
                                  "[hyprland]\nactive-border = \"popups.border\"\n";
    uint32_t state = 12345;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int round = 0; round < 1000; ++round) {
        std::string colors = colorsText;
        std::string shell = shellText;
        for (int k = 0; k < 1 + int(next() % 8); ++k) {
            colors[next() % colors.size()] = static_cast<char>(next());
            shell[next() % shell.size()] = static_cast<char>(next());
        }
        Toml colorsToml;
        CHECK(rgui::ui::ParseColorsToml(colors, colorsToml));
        const Palette palette = rgui::ui::ResolvePalette(colorsToml, round % 2 == 0);
        const std::string& mode = palette.Get("mode");
        CHECK(!mode.empty());
        CHECK_EQ(palette.dark, mode != "light");
        Toml shellToml;
        CHECK(rgui::ui::ParseToml(shell, shellToml));
        for (const auto& [key, value] : shellToml.values) {
            for (char c : key) CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                     c == '_' || c == '-' || c == '.');
            CHECK(value.find('\n') == std::string::npos);
        }
        rgui::ui::TokenSources sources;
        sources.themeShell = &shellToml;
        const auto t = rgui::ui::BuildTokens(palette, sources);
        CHECK(t.font.body >= 1);
        CHECK(t.spacing.controlHeight >= 0);
    }
}
