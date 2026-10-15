// SPDX-License-Identifier: Apache-2.0
// The colors.toml reader and resolver are a port of bin/omarchy-theme-color of omacom/omarchy at
// commit 246f33a2fef66411c990860d88e0823ae83987fc (parse_colors_file, mix_color,
// resolve_theme_colors, resolve_theme_mode), Copyright (c) David Heinemeier Hansson, MIT License:
// see third_party/omarchy/LICENSE. The tests compare every key with the script's --all output.
#include "ui/theme.h"

#include "common/exact_math.h"
#include "common/file_io.h"
#include "gfx/surface.h"

#include <cstdio>

namespace rgui::ui {

namespace {

bool IsSpace(char c) {
    // bash [:space:] in the C locale.
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

bool IsAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool IsKeyChar(char c) { return IsAlnum(c) || c == '_' || c == '-'; }

// The value charset of parse_colors_file: [a-zA-Z0-9#(),._+/%\ -].
bool IsValueChar(char c) {
    if (IsAlnum(c)) return true;
    switch (c) {
    case '#': case '(': case ')': case ',': case '.': case '_': case '+': case '/': case '%':
    case ' ': case '-':
        return true;
    default:
        return false;
    }
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool IsQuote(char c) { return c == '"' || c == '\''; }

// A `[section]` line, optionally followed by a comment (shell/Commons/Color.qml parseShell).
bool SectionHeader(std::string_view line, std::string_view& name) {
    if (line.empty() || line.front() != '[') return false;
    const size_t close = line.find(']');
    if (close == std::string_view::npos || close < 2) return false;
    const std::string_view inner = line.substr(1, close - 1);
    for (char c : inner) {
        if (!IsKeyChar(c)) return false;
    }
    const std::string_view rest = Trim(line.substr(close + 1));
    if (!rest.empty() && rest.front() != '#') return false;
    name = inner;
    return true;
}

uint8_t ClampByte(long v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

uint8_t AlphaByte(double alpha) {
    if (!(alpha > 0)) return 0;
    if (alpha >= 1) return 255;
    return ClampByte(RayoMd::ExactMath::LRound(alpha * 255.0));
}

bool AllHex(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (HexDigit(c) < 0) return false;
    }
    return true;
}

uint8_t HexByte(std::string_view s, size_t at) {
    return static_cast<uint8_t>(HexDigit(s[at]) * 16 + HexDigit(s[at + 1]));
}

// A non-negative decimal number "12" or "0.5"; false for anything else.
bool ParseDecimal(std::string_view s, double& out) {
    if (s.empty()) return false;
    double value = 0;
    size_t i = 0;
    bool digits = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        if (value < 1e9) value = value * 10 + (s[i] - '0');
        digits = true;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        double scale = 0.1;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            value += (s[i] - '0') * scale;
            scale *= 0.1;
            digits = true;
        }
    }
    if (!digits || i != s.size()) return false;
    out = value;
    return true;
}

// The first token of a gradient string: tokens are split at whitespace outside parentheses, and
// angle tokens ("45deg", "-12.5deg") are skipped.
std::string_view FirstColorToken(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && IsSpace(s[i])) ++i;
        const size_t start = i;
        int depth = 0;
        while (i < s.size() && (depth > 0 || !IsSpace(s[i]))) {
            if (s[i] == '(') ++depth;
            if (s[i] == ')' && depth > 0) --depth;
            ++i;
        }
        const std::string_view token = s.substr(start, i - start);
        if (token.empty()) break;
        std::string_view number = token;
        if (number.size() > 3 && number.substr(number.size() - 3) == "deg") {
            number.remove_suffix(3);
            if (!number.empty() && number.front() == '-') number.remove_prefix(1);
            double ignored = 0;
            if (ParseDecimal(number, ignored)) continue;
        }
        return token;
    }
    return {};
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// awk's hex_value: index("0123456789abcdef", tolower(char)) - 1, where gawk's index of the empty
// string (substr past the end) is 1.
int AwkHexValue(std::string_view hex, size_t at) {
    if (at >= hex.size()) return 0;
    return HexDigit(hex[at]);
}

int AwkHexPair(std::string_view hex, size_t at) {
    return AwkHexValue(hex, at) * 16 + AwkHexValue(hex, at + 1);
}

void AppendAwkHex(std::string& out, double channel) {
    // int() truncates toward zero; gawk prints a negative integer for %x as its 64-bit two's
    // complement.
    const long long value = static_cast<long long>(RayoMd::ExactMath::Trunc(channel));
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%02llx", static_cast<unsigned long long>(value));
    out += buffer;
}

} // namespace

Rgba Rgba::WithAlpha(double alpha) const {
    Rgba out = *this;
    out.a = static_cast<uint8_t>(gfx::Mul255(a, AlphaByte(alpha)));
    return out;
}

uint32_t Rgba::Premultiplied() const { return gfx::Premultiply(r, g, b, a); }

const std::string* Toml::Find(std::string_view key) const {
    const auto it = values.find(key);
    return it == values.end() ? nullptr : &it->second;
}

bool ParseToml(std::string_view text, Toml& out, bool sections) {
    if (text.size() > kMaxTomlBytes) return false;
    std::string section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;

        std::string_view name;
        if (sections && SectionHeader(Trim(line), name)) {
            section.assign(name);
            section += '.';
            continue;
        }

        // IFS='=' read -r key value: the key ends at the first '=', the value is the rest.
        const size_t equals = line.find('=');
        const std::string_view rawKey = line.substr(0, equals);
        std::string_view value = equals == std::string_view::npos ? std::string_view() : line.substr(equals + 1);

        std::string key;
        for (char c : rawKey) {
            if (c != '"' && c != '\'' && c != ' ') key += c;
        }
        if (key.empty() || key.front() == '#') continue;

        size_t quote = std::string_view::npos;
        for (size_t i = 0; i < value.size(); ++i) {
            if (IsQuote(value[i])) {
                quote = i;
                break;
            }
        }
        if (quote != std::string_view::npos) {
            value.remove_prefix(quote + 1);
            for (size_t i = 0; i < value.size(); ++i) {
                if (IsQuote(value[i])) {
                    value = value.substr(0, i);
                    break;
                }
            }
        } else {
            // shell.toml (Color.qml parseShell) allows a comment after an unquoted value.
            if (sections) value = value.substr(0, value.find('#'));
            value = Trim(value);
        }

        bool valid = true;
        for (char c : key) valid = valid && IsKeyChar(c);
        for (char c : value) valid = valid && IsValueChar(c);
        if (!valid) continue;

        out.values[sections ? section + key : key].assign(value);
    }
    return true;
}

bool ParseColorsToml(std::string_view text, Toml& out) {
    if (text.size() > kMaxTomlBytes) return false;
    const size_t lastNewline = text.rfind('\n');
    return ParseToml(lastNewline == std::string_view::npos ? std::string_view() : text.substr(0, lastNewline + 1),
                     out, false);
}

bool ReadTomlFile(const std::filesystem::path& path, Toml& out, bool sections) {
    std::string text;
    if (!RayoMd::File::ReadAll(path, kMaxTomlBytes, text)) return false;
    return sections ? ParseToml(text, out, true) : ParseColorsToml(text, out);
}

std::optional<Rgba> ParseColor(std::string_view text) {
    const std::string_view s = FirstColorToken(Trim(text));
    if (s.empty()) return std::nullopt;

    if (s.front() == '#') {
        const std::string_view hex = s.substr(1);
        if (!AllHex(hex)) return std::nullopt;
        if (hex.size() == 3) {
            const auto twice = [](char c) { return static_cast<uint8_t>(HexDigit(c) * 17); };
            return Rgba{twice(hex[0]), twice(hex[1]), twice(hex[2]), 255};
        }
        if (hex.size() == 6 || hex.size() == 8) {
            return Rgba{HexByte(hex, 0), HexByte(hex, 2), HexByte(hex, 4),
                        hex.size() == 8 ? HexByte(hex, 6) : uint8_t(255)};
        }
        return std::nullopt;
    }

    if (s.size() == 10 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && AllHex(s.substr(2))) {
        return Rgba{HexByte(s, 4), HexByte(s, 6), HexByte(s, 8), HexByte(s, 2)};
    }

    const size_t open = s.find('(');
    if (open == std::string_view::npos || s.back() != ')') return std::nullopt;
    const std::string_view function = s.substr(0, open);
    const bool rgb = EqualsNoCase(function, "rgb");
    if (!rgb && !EqualsNoCase(function, "rgba")) return std::nullopt;
    std::string inner;
    for (char c : s.substr(open + 1, s.size() - open - 2)) {
        if (!IsSpace(c)) inner += c;
    }

    if (inner.find(',') == std::string::npos) {
        // Hyprland's rgb(rrggbb) and rgba(rrggbbaa).
        if (!AllHex(inner) || (inner.size() != 6 && inner.size() != 8)) return std::nullopt;
        return Rgba{HexByte(inner, 0), HexByte(inner, 2), HexByte(inner, 4),
                    inner.size() == 8 ? HexByte(inner, 6) : uint8_t(255)};
    }

    std::string_view parts[4];
    size_t count = 0;
    std::string_view rest = inner;
    while (true) {
        if (count == 4) return std::nullopt;
        const size_t comma = rest.find(',');
        parts[count++] = rest.substr(0, comma);
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    if (count != 3 && count != 4) return std::nullopt;
    uint8_t channels[3];
    for (size_t i = 0; i < 3; ++i) {
        if (parts[i].empty() || parts[i].size() > 4) return std::nullopt;
        long value = 0;
        for (char c : parts[i]) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + (c - '0');
        }
        channels[i] = ClampByte(value);
    }
    uint8_t alpha = 255;
    if (count == 4) {
        double a = 0;
        if (!ParseDecimal(parts[3], a)) return std::nullopt;
        alpha = AlphaByte(a);
    }
    return Rgba{channels[0], channels[1], channels[2], alpha};
}

std::string MixColorText(std::string_view start, std::string_view end, double amount) {
    if (!start.empty() && start.front() == '#') start.remove_prefix(1);
    if (!end.empty() && end.front() == '#') end.remove_prefix(1);
    if (amount > 1) amount = amount / 100;
    if (amount < 0) amount = 0;
    if (amount > 1) amount = 1;
    std::string out = "#";
    for (size_t at = 0; at < 6; at += 2) {
        AppendAwkHex(out, AwkHexPair(start, at) * (1 - amount) + AwkHexPair(end, at) * amount + 0.5);
    }
    return out;
}

Rgba Mix(Rgba a, Rgba b, double t) {
    const auto channel = [t](uint8_t x, uint8_t y) {
        return ClampByte(RayoMd::ExactMath::LRound(x + (static_cast<double>(y) - x) * t));
    };
    return Rgba{channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), a.a};
}

const std::string& Palette::Get(std::string_view key) const {
    static const std::string empty;
    const auto it = values.find(key);
    return it == values.end() ? empty : it->second;
}

Rgba Palette::Color(std::string_view key, Rgba fallback) const {
    const auto color = ParseColor(Get(key));
    return color ? *color : fallback;
}

// The fallbacks are Tokyo Night's, Omarchy's default theme.
Rgba Palette::Background() const { return Color("background", Rgba{0x1a, 0x1b, 0x26, 255}); }
Rgba Palette::DarkBackground() const { return Color("dark_background", Background()); }
Rgba Palette::Foreground() const { return Color("foreground", Rgba{0xa9, 0xb1, 0xd6, 255}); }
Rgba Palette::Accent() const { return Color("accent", Rgba{0x7a, 0xa2, 0xf7, 255}); }
Rgba Palette::Selection() const { return Color("selection", Rgba{0x29, 0x2e, 0x42, 255}); }
Rgba Palette::SelectionForeground() const { return Color("selection_foreground", Foreground()); }
Rgba Palette::Cursor() const { return Color("cursor", Foreground()); }
Rgba Palette::Muted() const { return Color("muted", Foreground()); }
Rgba Palette::Red() const { return Color("red", Rgba{0xf7, 0x76, 0x8e, 255}); }
Rgba Palette::Yellow() const { return Color("yellow", Rgba{0xe0, 0xaf, 0x68, 255}); }
Rgba Palette::Green() const { return Color("green", Rgba{0x9e, 0xce, 0x6a, 255}); }

Palette ResolvePalette(const Toml& colors, bool lightModeFile) {
    Palette palette;
    auto& c = palette.values;
    for (const auto& [key, value] : colors.values) c[key] = value;

    const auto get = [&c](const char* key) -> std::string {
        const auto it = c.find(key);
        return it == c.end() ? std::string() : it->second;
    };
    const auto has = [&c](const char* key) {
        const auto it = c.find(key);
        return it != c.end() && !it->second.empty();
    };
    // ${A:-B}: A unless it is unset or empty.
    const auto either = [&](const char* key, const std::string& fallback) {
        return has(key) ? get(key) : fallback;
    };
    // alias_theme_color: [[ ${X[key]} ]] || X[key]="${X[fallback]}" (sets the key, maybe empty).
    const auto alias = [&](const char* key, const char* fallback) {
        if (!has(key)) c[key] = get(fallback);
    };
    const auto mix = [&](const char* key, const char* start, const char* end, double amount) {
        if (!has(key)) c[key] = MixColorText(get(start), end, amount);
    };

    static const char* const kLegacyPalette[][2] = {
        {"background", "bg"},           {"dark_background", "dark_bg"},
        {"darker_background", "darker_bg"}, {"lighter_background", "lighter_bg"},
        {"foreground", "fg"},           {"dark_foreground", "dark_fg"},
        {"light_foreground", "light_fg"}, {"bright_foreground", "bright_fg"},
    };
    for (const auto& pair : kLegacyPalette) alias(pair[0], pair[1]);

    // Themes generated before the semantic palette may only define ANSI names.
    if (!has("background")) c["background"] = get("color0");
    if (!has("foreground")) c["foreground"] = get("color7");
    if (has("background")) c["color0"] = get("background");
    if (has("foreground")) c["color7"] = get("foreground");

    static const char* const kLegacyAnsi[][2] = {
        {"red", "color1"},         {"green", "color2"},         {"yellow", "color3"},
        {"blue", "color4"},        {"magenta", "color5"},       {"cyan", "color6"},
        {"bright_red", "color9"},  {"bright_green", "color10"}, {"bright_yellow", "color11"},
        {"bright_blue", "color12"}, {"bright_magenta", "color13"}, {"bright_cyan", "color14"},
    };
    for (const auto& pair : kLegacyAnsi) alias(pair[0], pair[1]);
    alias("magenta", "purple");
    alias("bright_magenta", "bright_purple");

    if (!has("light_foreground")) c["light_foreground"] = either("color7", get("foreground"));
    if (!has("bright_foreground")) c["bright_foreground"] = either("color15", get("foreground"));
    c["cursor"] = get("bright_foreground");
    if (!has("lighter_background")) c["lighter_background"] = either("color0", get("background"));
    if (!has("dark_foreground")) c["dark_foreground"] = either("color8", get("foreground"));
    if (!has("muted")) c["muted"] = either("color8", get("dark_foreground"));
    if (!has("selection")) {
        c["selection"] = either("selection_background", either("color8", either("color0", get("background"))));
    }
    if (!has("selection_background")) c["selection_background"] = get("selection");
    if (!has("selection_foreground")) c["selection_foreground"] = get("bright_foreground");
    if (!has("orange")) c["orange"] = get("yellow");
    mix("brown", "orange", "#000000", 0.5);

    // Auto-derive shades from base accents when not defined and not aliased from colorN.
    mix("dark_background", "background", "#000000", 0.25);
    mix("darker_background", "background", "#000000", 0.5);
    mix("bright_red", "red", "#ffffff", 0.2);
    mix("bright_yellow", "yellow", "#ffffff", 0.2);
    mix("bright_green", "green", "#ffffff", 0.2);
    mix("bright_cyan", "cyan", "#ffffff", 0.2);
    mix("bright_blue", "blue", "#ffffff", 0.2);
    mix("bright_magenta", "magenta", "#ffffff", 0.2);
    alias("purple", "magenta");
    alias("bright_purple", "bright_magenta");

    // Keep semantic themes compatible with consumers of the legacy ANSI names.
    static const char* const kAnsi[][2] = {
        {"color0", "background"},   {"color1", "red"},           {"color2", "green"},
        {"color3", "yellow"},       {"color4", "blue"},          {"color5", "magenta"},
        {"color6", "cyan"},         {"color7", "foreground"},    {"color8", "muted"},
        {"color9", "bright_red"},   {"color10", "bright_green"}, {"color11", "bright_yellow"},
        {"color12", "bright_blue"}, {"color13", "bright_magenta"}, {"color14", "bright_cyan"},
        {"color15", "bright_foreground"},
    };
    for (const auto& pair : kAnsi) alias(pair[0], pair[1]);

    // And with consumers of the short palette names.
    for (const auto& pair : kLegacyPalette) {
        if (has(pair[0])) c[pair[1]] = get(pair[0]);
    }

    // resolve_theme_mode: the mode key, the legacy theme_type, a light.mode file, then the
    // background's R+G+B against 382, then dark.
    if (!has("mode")) c["mode"] = get("theme_type");
    if (!has("mode")) {
        const std::string background = get("background");
        if (lightModeFile) {
            c["mode"] = "light";
        } else if (background.size() == 7 && background[0] == '#' && AllHex(std::string_view(background).substr(1))) {
            const int sum = HexByte(background, 1) + HexByte(background, 3) + HexByte(background, 5);
            c["mode"] = sum > 382 ? "light" : "dark";
        } else {
            c["mode"] = "dark";
        }
    }
    c["theme_type"] = get("mode");
    palette.dark = get("mode") != "light";
    return palette;
}

const BundledTheme* FindBundledTheme(std::string_view slug) {
    for (const BundledTheme& theme : kBundledThemes) {
        if (slug == theme.slug) return &theme;
    }
    return nullptr;
}

Palette BundledPalette(const BundledTheme& theme) {
    Toml colors;
    ParseColorsToml(theme.colors, colors);
    return ResolvePalette(colors, theme.lightModeFile);
}

} // namespace rgui::ui
