// SPDX-License-Identifier: Apache-2.0
// UI tokens from Omarchy's shell.toml. The defaults are default/themed/shell.toml.tpl of
// omacom/omarchy at commit 246f33a2fef66411c990860d88e0823ae83987fc, rendered with the theme's
// palette the way bin/omarchy-theme-set-templates renders it; the values are read the way
// shell/Commons/Style.qml, Color.qml and Border.qml of the same commit read them. Copyright (c)
// David Heinemeier Hansson, MIT License: see third_party/omarchy/LICENSE.
#include "ui/tokens.h"

#include "common/exact_math.h"

#include <string>
#include <string_view>

namespace rgui::ui {

namespace {

// Every value of shell.toml.tpl, as the template writes it.
struct DefaultValue {
    const char* key;
    const char* value;
};
constexpr DefaultValue kTemplateValues[] = {
    {"bar.background", "{{ background }}"},
    {"bar.background-alpha", "1.0"},
    {"bar.text", "{{ foreground }}"},
    {"bar.active", "{{ red }}"},
    {"bar.scale-with-font", "true"},
    {"bar.size-horizontal", "26"},
    {"bar.size-vertical", "28"},
    {"hyprland.active-border", "{{ shell_gradient hyprland_active_border accent }}"},
    {"hyprland.active-border-foreground", "{{ shell_gradient hyprland_active_border foreground }}"},
    {"controls.normal-color", "{{ foreground }}"},
    {"controls.normal-fill-alpha", "0.04"},
    {"controls.normal-border", "{{ foreground }}"},
    {"controls.normal-border-width", "1"},
    {"controls.normal-border-alpha", "0.4"},
    {"controls.hover-cursor-color", "{{ foreground }}"},
    {"controls.hover-cursor-fill-alpha", "0.08"},
    {"controls.hover-cursor-border", "{{ foreground }}"},
    {"controls.hover-cursor-border-width", "1"},
    {"controls.hover-cursor-border-alpha", "0.25"},
    {"controls.focus-color", "{{ foreground }}"},
    {"controls.focus-fill-alpha", "0.08"},
    {"controls.focus-border", "{{ foreground }}"},
    {"controls.focus-border-width", "1"},
    {"controls.focus-border-alpha", "0.25"},
    {"controls.selected-color", "{{ foreground }}"},
    {"controls.selected-fill-alpha", "0.18"},
    {"controls.selected-border", "{{ foreground }}"},
    {"controls.selected-border-width", "0"},
    {"controls.selected-border-alpha", "1.0"},
    {"controls.pressed-fill-alpha", "0.22"},
    {"controls.selection-fill-alpha", "0.35"},
    {"spacing.scale", "1.0"},
    {"spacing.scale-with-font", "true"},
    {"font.base-size", "12"},
    {"popups.background", "{{ background }}"},
    {"popups.background-alpha", "1.0"},
    {"popups.text", "{{ foreground }}"},
    {"popups.border", "hyprland.active-border"},
    {"popups.border-alpha", "1.0"},
    {"tooltip.background", "{{ background }}"},
    {"tooltip.background-alpha", "0.97"},
    {"tooltip.text", "{{ foreground }}"},
    {"tooltip.border", "hyprland.active-border-foreground"},
    {"tooltip.border-alpha", "1.0"},
    {"notifications.background", "{{ background }}"},
    {"notifications.background-alpha", "1.0"},
    {"notifications.text", "{{ foreground }}"},
    {"notifications.border", "hyprland.active-border"},
    {"notifications.border-alpha", "1.0"},
    {"notifications.countdown", "{{ accent }}"},
    {"launcher.background", "{{ background }}"},
    {"launcher.background-alpha", "0.95"},
    {"launcher.text", "{{ foreground }}"},
    {"launcher.border", "hyprland.active-border-foreground"},
    {"launcher.border-alpha", "1.0"},
    {"launcher.scrim", "{{ background }}"},
    {"launcher.scrim-alpha", "0.5"},
    {"launcher.selected-background", "{{ foreground }}"},
    {"launcher.selected-background-alpha", "0.08"},
    {"launcher.selected-text", "{{ accent }}"},
    {"launcher.selected-border", "hyprland.active-border-foreground"},
    {"launcher.selected-border-alpha", "0.25"},
    {"menu.background", "{{ background }}"},
    {"menu.background-alpha", "1.0"},
    {"menu.text", "{{ foreground }}"},
    {"menu.border", "hyprland.active-border-foreground"},
    {"menu.border-alpha", "1.0"},
    {"menu.scrim", "{{ background }}"},
    {"menu.scrim-alpha", "0.5"},
    {"menu.selected-background", "{{ foreground }}"},
    {"menu.selected-background-alpha", "0.08"},
    {"menu.selected-text", "{{ accent }}"},
    {"menu.selected-border", "hyprland.active-border-foreground"},
    {"menu.selected-border-alpha", "0.25"},
    {"polkit.background", "{{ background }}"},
    {"polkit.background-alpha", "1.0"},
    {"polkit.text", "{{ foreground }}"},
    {"polkit.text-error", "{{ red }}"},
    {"polkit.border", "hyprland.active-border"},
    {"polkit.border-error", "{{ red }}"},
    {"polkit.border-alpha", "1.0"},
    {"polkit.scrim", "{{ background }}"},
    {"polkit.scrim-alpha", "0.5"},
    {"polkit.accent", "{{ accent }}"},
    {"lock.background", "{{ background }}"},
    {"lock.background-alpha", "0.8"},
    {"lock.text", "{{ foreground }}"},
    {"lock.placeholder", "{{ mix foreground background 34% }}"},
    {"lock.text-error", "{{ red }}"},
    {"lock.border", "hyprland.active-border"},
    {"lock.border-active", "hyprland.active-border"},
    {"lock.border-error", "{{ red }}"},
    {"lock.border-alpha", "1.0"},
    {"lock.selection", "{{ accent }}"},
    {"lock.selection-alpha", "0.45"},
    {"image-picker.text", "{{ foreground }}"},
    {"image-picker.selected-border", "{{ accent }}"},
    {"image-picker.selected-border-alpha", "1.0"},
    {"image-picker.unselected-border", "{{ foreground }}"},
    {"image-picker.unselected-border-alpha", "0.28"},
};

// The template's commented per-token values, in px at base-size 12 and scale 1. They are the
// scale's starting points (Style.qml's fallbacks), not overrides: an override set in a shell.toml
// pins a token, these scale with [spacing] scale and [font] base-size.
struct ScaledToken {
    const char* key;
    double px;
    int Spacing::*field;
};
constexpr ScaledToken kSpacingTokens[] = {
    {"xxs", 2, &Spacing::xxs},
    {"xs", 3, &Spacing::xs},
    {"sm", 4, &Spacing::sm},
    {"md", 6, &Spacing::md},
    {"lg", 8, &Spacing::lg},
    {"xl", 10, &Spacing::xl},
    {"xxl", 12, &Spacing::xxl},
    {"xxxl", 14, &Spacing::xxxl},
    {"huge", 18, &Spacing::huge},
    {"control-gap", 8, &Spacing::controlGap},
    {"control-padding-x", 10, &Spacing::controlPaddingX},
    {"control-padding-y", 6, &Spacing::controlPaddingY},
    {"input-padding-y", 7, &Spacing::inputPaddingY},
    {"control-height", 28, &Spacing::controlHeight},
    {"popup-row-height", 28, &Spacing::popupRowHeight},
    {"row-gap", 8, &Spacing::rowGap},
    {"row-padding-x", 12, &Spacing::rowPaddingX},
    {"label-gap", 4, &Spacing::labelGap},
    {"panel-gap", 14, &Spacing::panelGap},
    {"panel-padding", 18, &Spacing::panelPadding},
    {"popup-padding", 14, &Spacing::popupPadding},
    {"dropdown-width", 240, &Spacing::dropdownWidth},
    {"searchable-dropdown-width", 260, &Spacing::searchableDropdownWidth},
    {"number-field-width", 120, &Spacing::numberFieldWidth},
    {"searchable-popup-min-height", 220, &Spacing::searchablePopupMinHeight},
};

// [font]: each size is round(base-size x factor) (Style.qml's fontPx; at base-size 12 these give
// the template's commented 10, 11, 12, 13, 14, 16, 24, 28 and 18).
struct FontToken {
    const char* key;
    double factor;
    int FontSizes::*field;
};
constexpr FontToken kFontTokens[] = {
    {"caption", 0.833, &FontSizes::caption},
    {"body-small", 0.917, &FontSizes::bodySmall},
    {"body", 1.0, &FontSizes::body},
    {"subtitle", 1.083, &FontSizes::subtitle},
    {"title", 1.167, &FontSizes::title},
    {"heading", 1.333, &FontSizes::heading},
    {"display", 2.0, &FontSizes::display},
    {"display-large", 2.333, &FontSizes::displayLarge},
    {"icon-large", 1.5, &FontSizes::iconLarge},
};

// The template's commented `border-width = 2` of [popups] is the shell's fallback
// max(1, space(2)); tooltips fall back to 1 (Bar.qml).
constexpr double kSurfaceBorderPx = 2;

// sRGB to linear light for each 8-bit value, ((v / 255 + 0.055) / 1.055)^2.4 or v / 255 / 12.92
// (WCAG 2 relative luminance), precomputed because the toolkit makes no libm calls.
constexpr double kSrgbToLinear[256] = {
    0.000000000, 0.000303527, 0.000607054, 0.000910581, 0.001214108, 0.001517635, 0.001821162, 0.002124689,
    0.002428216, 0.002731743, 0.003035270, 0.003346536, 0.003676507, 0.004024717, 0.004391442, 0.004776953,
    0.005181517, 0.005605392, 0.006048833, 0.006512091, 0.006995410, 0.007499032, 0.008023193, 0.008568126,
    0.009134059, 0.009721217, 0.010329823, 0.010960094, 0.011612245, 0.012286488, 0.012983032, 0.013702083,
    0.014443844, 0.015208514, 0.015996293, 0.016807376, 0.017641954, 0.018500220, 0.019382361, 0.020288563,
    0.021219010, 0.022173885, 0.023153366, 0.024157632, 0.025186860, 0.026241222, 0.027320892, 0.028426040,
    0.029556834, 0.030713444, 0.031896033, 0.033104767, 0.034339807, 0.035601315, 0.036889450, 0.038204372,
    0.039546235, 0.040915197, 0.042311411, 0.043735029, 0.045186204, 0.046665086, 0.048171824, 0.049706566,
    0.051269458, 0.052860647, 0.054480276, 0.056128490, 0.057805430, 0.059511238, 0.061246054, 0.063010018,
    0.064803267, 0.066625939, 0.068478170, 0.070360096, 0.072271851, 0.074213568, 0.076185381, 0.078187422,
    0.080219820, 0.082282707, 0.084376212, 0.086500462, 0.088655586, 0.090841711, 0.093058963, 0.095307467,
    0.097587347, 0.099898728, 0.102241733, 0.104616484, 0.107023103, 0.109461711, 0.111932428, 0.114435374,
    0.116970668, 0.119538428, 0.122138772, 0.124771818, 0.127437680, 0.130136477, 0.132868322, 0.135633330,
    0.138431615, 0.141263291, 0.144128471, 0.147027266, 0.149959790, 0.152926152, 0.155926464, 0.158960835,
    0.162029376, 0.165132195, 0.168269400, 0.171441101, 0.174647404, 0.177888416, 0.181164244, 0.184474995,
    0.187820772, 0.191201683, 0.194617830, 0.198069320, 0.201556254, 0.205078736, 0.208636870, 0.212230757,
    0.215860500, 0.219526200, 0.223227957, 0.226965874, 0.230740049, 0.234550582, 0.238397574, 0.242281122,
    0.246201327, 0.250158285, 0.254152094, 0.258182853, 0.262250658, 0.266355605, 0.270497791, 0.274677312,
    0.278894263, 0.283148740, 0.287440838, 0.291770650, 0.296138271, 0.300543794, 0.304987314, 0.309468923,
    0.313988713, 0.318546778, 0.323143209, 0.327778098, 0.332451536, 0.337163615, 0.341914425, 0.346704056,
    0.351532600, 0.356400144, 0.361306780, 0.366252596, 0.371237680, 0.376262123, 0.381326011, 0.386429434,
    0.391572478, 0.396755231, 0.401977780, 0.407240212, 0.412542613, 0.417885071, 0.423267670, 0.428690497,
    0.434153636, 0.439657174, 0.445201195, 0.450785783, 0.456411023, 0.462077000, 0.467783796, 0.473531496,
    0.479320183, 0.485149940, 0.491020850, 0.496932995, 0.502886458, 0.508881321, 0.514917665, 0.520995573,
    0.527115126, 0.533276404, 0.539479489, 0.545724461, 0.552011402, 0.558340390, 0.564711506, 0.571124829,
    0.577580440, 0.584078418, 0.590618841, 0.597201788, 0.603827339, 0.610495571, 0.617206562, 0.623960392,
    0.630757136, 0.637596874, 0.644479682, 0.651405637, 0.658374817, 0.665387298, 0.672443157, 0.679542470,
    0.686685312, 0.693871761, 0.701101892, 0.708375780, 0.715693501, 0.723055129, 0.730460740, 0.737910409,
    0.745404210, 0.752942217, 0.760524505, 0.768151147, 0.775822218, 0.783537792, 0.791297940, 0.799102738,
    0.806952258, 0.814846572, 0.822785754, 0.830769877, 0.838799012, 0.846873232, 0.854992608, 0.863157213,
    0.871367119, 0.879622397, 0.887923118, 0.896269353, 0.904661174, 0.913098652, 0.921581856, 0.930110858,
    0.938685728, 0.947306537, 0.955973353, 0.964686248, 0.973445290, 0.982250550, 0.991102097, 1.000000000,
};

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// A leading decimal number: optional sign, digits, optional fraction. `consumed` is how many
// characters it used (0 when there is no number).
double LeadingNumber(std::string_view s, size_t& consumed, bool allowFraction) {
    size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) ++i;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        negative = s[i] == '-';
        ++i;
    }
    double value = 0;
    bool digits = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        if (value < 1e12) value = value * 10 + (s[i] - '0');
        digits = true;
    }
    if (allowFraction && i < s.size() && s[i] == '.') {
        size_t j = i + 1;
        double scale = 0.1;
        bool fraction = false;
        for (; j < s.size() && s[j] >= '0' && s[j] <= '9'; ++j) {
            value += (s[j] - '0') * scale;
            scale *= 0.1;
            fraction = true;
        }
        if (fraction || digits) i = j;
        digits = digits || fraction;
    }
    consumed = digits ? i : 0;
    return negative ? -value : value;
}

// JavaScript's parseInt(v, 10) and parseFloat(v): the leading number, if any.
std::optional<double> ParseIntJs(std::string_view s) {
    size_t used = 0;
    const double v = LeadingNumber(s, used, false);
    return used ? std::optional<double>(v) : std::nullopt;
}
std::optional<double> ParseFloatJs(std::string_view s) {
    size_t used = 0;
    const double v = LeadingNumber(s, used, true);
    return used ? std::optional<double>(v) : std::nullopt;
}
// JavaScript's Number(v) for a non-empty value: the whole string must be the number.
std::optional<double> NumberJs(std::string_view s) {
    s = Trim(s);
    size_t used = 0;
    const double v = LeadingNumber(s, used, true);
    return used && used == s.size() ? std::optional<double>(v) : std::nullopt;
}

// Math.round: half up.
int RoundJs(double v) { return static_cast<int>(RayoMd::ExactMath::Floor(v + 0.5)); }

double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Style.qml boolToken.
bool BoolToken(const std::string* value, bool fallback) {
    if (!value) return fallback;
    const std::string s = Lower(Trim(*value));
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return fallback;
}

bool IsAngle(std::string_view token) {
    if (token.size() < 4 || token.substr(token.size() - 3) != "deg") return false;
    token.remove_suffix(3);
    if (!token.empty() && token.front() == '-') token.remove_prefix(1);
    size_t used = 0;
    LeadingNumber(token, used, true);
    return used != 0 && used == token.size() && token.front() != '+';
}

// --- the template renderer of bin/omarchy-theme-set-templates ---------------------------------

struct Renderer {
    const Palette& palette;

    const std::string* Value(std::string_view key) const {
        const auto it = palette.values.find(key);
        return it == palette.values.end() ? nullptr : &it->second;
    }

    // resolve_theme_ref: the palette value of `ref` (even empty), else of `fallback`, else the
    // fallback itself, else the ref.
    std::string ResolveRef(std::string_view ref, std::string_view fallback) const {
        if (const std::string* v = Value(ref)) return *v;
        if (!fallback.empty()) {
            if (const std::string* v = Value(fallback)) return *v;
            return std::string(fallback);
        }
        return std::string(ref);
    }

    // shell_gradient_value: the colours of a gradient spec, each a palette key or a colour, then
    // its angle.
    std::string ShellGradient(std::string_view key, std::string_view fallback) const {
        const std::string spec = ResolveRef(key, fallback);
        std::string colors;
        std::string angle;
        bool any = false;
        size_t i = 0;
        const std::string_view s = spec;
        while (i < s.size()) {
            while (i < s.size() && IsSpace(s[i])) ++i;
            const size_t start = i;
            while (i < s.size() && !IsSpace(s[i])) ++i;
            const std::string_view part = s.substr(start, i - start);
            if (part.empty()) break;
            if (IsAngle(part)) {
                angle.assign(part.substr(0, part.size() - 3));
                continue;
            }
            const std::string* named = Value(part);
            if (any) colors += ' ';
            colors += named ? *named : std::string(part);
            any = true;
        }
        if (!any) return spec;
        if (!angle.empty()) colors += " " + angle + "deg";
        return colors;
    }

    // One {{ ... }} span; false when the renderer would leave it in place.
    bool Token(std::string_view content, std::string& out) const {
        std::string_view words[4];
        size_t count = 0;
        size_t i = 0;
        while (i < content.size()) {
            while (i < content.size() && IsSpace(content[i])) ++i;
            const size_t start = i;
            while (i < content.size() && !IsSpace(content[i])) ++i;
            if (i == start) break;
            if (count == 4) return false;
            words[count++] = content.substr(start, i - start);
        }
        if (count == 1) {
            if (const std::string* v = Value(words[0])) {
                out = *v;
                return true;
            }
            return false;
        }
        if (count >= 2 && words[0] == "shell_gradient") {
            out = ShellGradient(words[1], count >= 3 ? words[2] : std::string_view());
            return true;
        }
        if (count == 4 && words[0] == "mix") {
            const std::string* start = Value(words[1]);
            const std::string* end = Value(words[2]);
            const auto isHex6 = [](const std::string* v) {
                if (!v || v->size() != 7 || (*v)[0] != '#') return false;
                for (size_t k = 1; k < 7; ++k) {
                    const char c = (*v)[k];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
                }
                return true;
            };
            if (!isHex6(start) || !isHex6(end)) return false;
            std::string_view amountText = words[3];
            const bool percent = !amountText.empty() && amountText.back() == '%';
            if (percent) amountText.remove_suffix(1);
            const auto amount = ParseFloatJs(amountText);
            if (!amount) return false;
            out = MixColorText(*start, *end, percent ? *amount / 100 : *amount);
            return true;
        }
        return false;
    }

    std::string Render(std::string_view line) const {
        std::string out;
        while (true) {
            const size_t open = line.find("{{");
            if (open == std::string_view::npos) break;
            const size_t close = line.find("}}", open + 2);
            if (close == std::string_view::npos) break;
            std::string value;
            if (Token(line.substr(open + 2, close - open - 2), value)) {
                out.append(line.substr(0, open));
                out += value;
                line.remove_prefix(close + 2);
            } else {
                out.append(line.substr(0, open + 1));
                line.remove_prefix(open + 1);
            }
        }
        out.append(line);
        return out;
    }
};

// --- shell value readers (Style.qml, Color.qml, Border.qml) -----------------------------------

struct Shell {
    const Toml& values;
    const Palette& palette;

    const std::string* Raw(std::string_view key) const {
        const std::string* v = values.Find(key);
        return v && !Trim(*v).empty() ? v : nullptr;
    }

    double Alpha(std::string_view key, double fallback) const {
        const std::string* v = Raw(key);
        if (!v) return fallback;
        const auto n = NumberJs(*v);
        return n ? Clamp01(*n) : fallback;
    }

    // Style.qml resolveStateColor / Border.qml cssColor: a palette role or a colour.
    std::optional<Rgba> Role(std::string_view token) const {
        const std::string role = Lower(Trim(token));
        if (role == "foreground" || role == "text") return palette.Foreground();
        if (role == "accent") return palette.Accent();
        if (role == "urgent") return palette.Red();
        if (role == "muted") return palette.Muted();
        if (role == "background") return palette.Background();
        if (role == "transparent") return Rgba{0, 0, 0, 0};
        return ParseColor(token);
    }

    // Color.qml flatColor: the first colour of a value, following `section.key` references.
    std::optional<Rgba> Flat(std::string_view value) const {
        std::string token(value);
        for (int depth = 0; depth < 16; ++depth) {
            const std::string role = Lower(Trim(token));
            const std::string* next = values.Find(role);
            if (!next || *next == token || role.find('.') == std::string::npos) break;
            token = *next;
        }
        // A gradient's first colour.
        std::string_view first = Trim(token);
        while (!first.empty()) {
            size_t end = 0;
            while (end < first.size() && !IsSpace(first[end])) ++end;
            const std::string_view part = first.substr(0, end);
            if (!IsAngle(part)) {
                // ParseColor takes the first colour of the whole remainder, parentheses intact.
                if (const auto role = Role(part)) return role;
                return ParseColor(first);
            }
            first = Trim(first.substr(end));
        }
        return std::nullopt;
    }

    // Color.qml composed(): the colour of `key` with its alpha replaced by `<alphaKey>`.
    Rgba Composed(std::string_view key, std::string_view alphaKey, Rgba fallback, double alphaFallback) const {
        const std::string* v = Raw(key);
        const auto color = v ? Flat(*v) : std::nullopt;
        Rgba out = color ? *color : fallback;
        out.a = 255;
        return out.WithAlpha(Alpha(alphaKey, alphaFallback));
    }

    Rgba Text(std::string_view key, Rgba fallback) const {
        const std::string* v = Raw(key);
        const auto color = v ? Flat(*v) : std::nullopt;
        return color ? *color : fallback;
    }

    // A border width (the first number of a CSS-style width list), else `fallback`.
    int Width(std::string_view key, int fallback) const {
        const std::string* v = Raw(key);
        if (!v) return fallback;
        const auto n = ParseFloatJs(*v);
        if (!n) return fallback;
        return *n > 0 ? RoundJs(*n) : 0;
    }
};

ControlState BuildControl(const Shell& shell, const std::string& prefix, Rgba color, double fillFallback,
                          double borderAlphaFallback, int widthFallback, double& borderAlphaOut, int& widthOut) {
    ControlState state;
    state.color = color;
    state.fillAlpha = shell.Alpha("controls." + prefix + "-fill-alpha", fillFallback);
    borderAlphaOut = shell.Alpha("controls." + prefix + "-border-alpha", borderAlphaFallback);
    // Style.qml: Math.max(0, Math.round(styleNum(<prefix>-border-width, fallback))).
    widthOut = widthFallback;
    if (const std::string* w = shell.Raw("controls." + prefix + "-border-width")) {
        if (const auto n = NumberJs(*w)) widthOut = *n > 0 ? RoundJs(*n) : 0;
    }
    state.borderWidth = widthOut;
    // Border.qml controlSpec: <prefix>-border (a colour, role or gradient), else the state colour;
    // its alpha times <prefix>-border-alpha.
    Rgba border = color;
    if (const std::string* b = shell.Raw("controls." + prefix + "-border")) {
        if (const auto parsed = shell.Flat(*b)) border = *parsed;
    }
    state.border = border.WithAlpha(borderAlphaOut);
    return state;
}

double RelativeLuminance(Rgba c) {
    return 0.2126 * kSrgbToLinear[c.r] + 0.7152 * kSrgbToLinear[c.g] + 0.0722 * kSrgbToLinear[c.b];
}

} // namespace

double ContrastRatio(Rgba a, Rgba b) {
    const double la = RelativeLuminance(a);
    const double lb = RelativeLuminance(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

Toml MergedShellValues(const Palette& palette, const TokenSources& sources) {
    Toml merged;
    const Renderer renderer{palette};
    for (const DefaultValue& entry : kTemplateValues) merged.values[entry.key] = renderer.Render(entry.value);
    // Color.qml mergeShell: the theme's values, then the user's, key by key.
    for (const Toml* layer : {sources.themeShell, sources.userShell}) {
        if (!layer) continue;
        for (const auto& [key, value] : layer->values) merged.values[key] = value;
    }
    return merged;
}

Tokens BuildTokens(const Palette& palette, const TokenSources& sources) {
    const Toml merged = MergedShellValues(palette, sources);
    const Shell shell{merged, palette};
    Tokens t;
    t.dark = palette.dark;

    // [font] (Style.qml applyShellValues, fontPx, fontToken).
    int base = 12;
    if (const std::string* v = shell.Raw("font.base-size")) {
        if (const auto n = ParseIntJs(*v)) base = *n < 1 ? 1 : (*n > 4096 ? 4096 : static_cast<int>(*n));
    }
    t.font.baseSize = base;
    const auto fontOverride = [&](const char* key, int fallback) {
        const std::string* v = shell.Raw(std::string("font.") + key);
        if (!v) return fallback;
        const auto n = ParseIntJs(*v);
        return n && *n > 0 ? static_cast<int>(*n > 4096 ? 4096 : *n) : fallback;
    };
    for (const FontToken& token : kFontTokens) {
        const int scaled = RoundJs(base * token.factor);
        t.font.*token.field = fontOverride(token.key, scaled < 1 ? 1 : scaled);
    }
    t.font.iconSmall = fontOverride("icon-small", t.font.bodySmall);
    t.font.icon = fontOverride("icon", t.font.title);

    // [spacing] (Style.qml space, spacingToken).
    double scale = 1.0;
    if (const std::string* v = shell.Raw("spacing.scale")) {
        const auto n = ParseFloatJs(*v);
        scale = n && *n >= 0 ? *n : 1.0;
    }
    const bool scaleWithFont = BoolToken(shell.Raw("spacing.scale-with-font"), true);
    const double fontScale = base / 12.0 < 1.0 / 12 ? 1.0 / 12 : base / 12.0;
    const double effective = scale * (scaleWithFont ? fontScale : 1.0);
    const auto space = [effective](double px) {
        const double n = px * effective;
        if (!(n > 0)) return 0;
        const int r = RoundJs(n);
        return r < 1 ? 1 : r;
    };
    t.spacing.hairline = space(1);
    for (const ScaledToken& token : kSpacingTokens) {
        int value = space(token.px);
        if (const std::string* v = shell.Raw(std::string("spacing.") + token.key)) {
            const auto n = ParseFloatJs(*v);
            if (n && *n >= 0) value = RoundJs(*n > 1e6 ? 1e6 : *n);
        }
        t.spacing.*token.field = value;
    }

    // [controls] (Style.qml state tokens, Border.qml controlSpec).
    const auto stateColor = [&](const char* key, std::string_view fallbackToken, Rgba fallback) {
        const std::string* v = shell.Raw(std::string("controls.") + key);
        const std::string_view token = v ? Trim(*v) : fallbackToken;
        const auto color = shell.Role(token);
        return color ? *color : fallback;
    };
    const Rgba fg = palette.Foreground();
    const Rgba normalColor = stateColor("normal-color", "foreground", fg);
    const Rgba hoverColor = stateColor("hover-cursor-color", "foreground", fg);
    const Rgba selectedColor = stateColor("selected-color", "foreground", fg);
    Rgba focusColor = hoverColor;
    if (const std::string* v = shell.Raw("controls.focus-color")) {
        const std::string role = Lower(Trim(*v));
        if (role != "hover" && role != "hover-cursor" && role != "inherit") {
            focusColor = stateColor("focus-color", "", hoverColor);
        }
    }
    const Rgba pressedColor = stateColor("pressed-color", "", hoverColor);
    const Rgba selectionColor = stateColor("selection-color", "foreground", fg);

    double normalBorderAlpha = 0, hoverBorderAlpha = 0, ignoredAlpha = 0;
    int normalWidth = 0, hoverWidth = 0, ignoredWidth = 0;
    t.normal = BuildControl(shell, "normal", normalColor, 0.04, 0.4, 1, normalBorderAlpha, normalWidth);
    t.hoverCursor = BuildControl(shell, "hover-cursor", hoverColor, 0.08, 0.25, normalWidth, hoverBorderAlpha, hoverWidth);
    t.focus = BuildControl(shell, "focus", focusColor, t.hoverCursor.fillAlpha, hoverBorderAlpha, hoverWidth,
                           ignoredAlpha, ignoredWidth);
    t.selected = BuildControl(shell, "selected", selectedColor, 0.18, 1.0, 0, ignoredAlpha, ignoredWidth);
    t.pressedFill = pressedColor.WithAlpha(shell.Alpha("controls.pressed-fill-alpha", 0.22));
    t.selectionFill = selectionColor.WithAlpha(shell.Alpha("controls.selection-fill-alpha", 0.35));

    // Surfaces (Color.qml popups, tooltip, menu; widths from Border.qml surfaceSpec callers).
    const Rgba bg = palette.Background();
    const Rgba accent = palette.Accent();
    const int surfaceBorder = space(kSurfaceBorderPx) < 1 ? 1 : space(kSurfaceBorderPx);
    t.popups.background = shell.Composed("popups.background", "popups.background-alpha", bg, 1.0);
    t.popups.text = shell.Text("popups.text", fg);
    t.popups.border = shell.Composed("popups.border", "popups.border-alpha", accent, 1.0);
    t.popups.borderWidth = shell.Width("popups.border-width", surfaceBorder);
    t.tooltip.background = shell.Composed("tooltip.background", "tooltip.background-alpha", bg, 1.0);
    t.tooltip.text = shell.Text("tooltip.text", fg);
    t.tooltip.border = shell.Composed("tooltip.border", "tooltip.border-alpha", fg, 1.0);
    t.tooltip.borderWidth = shell.Width("tooltip.border-width", 1);
    t.menu.background = shell.Composed("menu.background", "menu.background-alpha", bg, 1.0);
    t.menu.text = shell.Text("menu.text", fg);
    t.menu.border = shell.Composed("menu.border", "menu.border-alpha", fg, 1.0);
    t.menu.borderWidth = shell.Width("menu.border-width", surfaceBorder);
    t.menu.scrim = shell.Composed("menu.scrim", "menu.scrim-alpha", bg, 0.5);
    t.menu.selectedBackground = shell.Composed("menu.selected-background", "menu.selected-background-alpha", fg, 0.08);
    t.menu.selectedText = shell.Text("menu.selected-text", accent);
    t.menu.selectedBorder = shell.Composed("menu.selected-border", "menu.selected-border-alpha", fg, 0.0);

    // Radius: Hyprland's decoration:rounding, else the bundled theme's, else 0.
    t.radius = sources.hyprlandRounding ? *sources.hyprlandRounding : sources.themeRounding;
    if (t.radius < 0) t.radius = 0;
    t.controlRadius = t.radius < 4 ? t.radius : 4;

    // Text scale: an Omarchy shell.toml's base-size already carries the user's text size
    // (omarchy-display-text-size sets both), so the system factor applies only without one.
    if (sources.themeShell || sources.userShell) {
        t.textScale = 1.0;
    } else {
        const double s = sources.systemTextScale;
        t.textScale = !(s >= 0.5) ? 0.5 : (s > 3.0 ? 3.0 : s);
    }

    // The derived colours, one row per role with its source.
    const Rgba foregroundOnAccent = fg;
    DerivedColors& c = t.colors;
    c.windowBackground = bg;                                        // Omawrite: the window is `background`
    c.text = fg;                                                    // Omawrite: text is `foreground`
    c.mutedText = Mix(bg, fg, 0.62);                                // hype src/apptheme.cpp:62 (muted)
    c.previewBackground = palette.DarkBackground();                 // ours: the preview pane sits a step darker
    c.hairline = fg.WithAlpha(0.12);                                // ours: separators and page outlines
    c.selection = palette.Selection();                              // omawrite src/Main.qml:22 (themeSelection)
    c.selectedText = palette.SelectionForeground();                 // omawrite src/Main.qml:23 (selection_foreground)
    c.caret = palette.Cursor();                                     // Omarchy's cursor = bright_foreground
    c.accent = accent;                                              // hype src/apptheme.cpp:57 (accent)
    c.accentHover = Mix(accent, fg, 0.15);                          // hype src/apptheme.cpp:68 (accentHover)
    c.accentPressed = Mix(accent, bg, 0.15);                        // ours: the mirror of accentHover
    c.textOnAccent = ContrastRatio(bg, accent) > ContrastRatio(foregroundOnAccent, accent)
                         ? bg
                         : foregroundOnAccent;                      // omasnap: the better WCAG contrast
    c.error = palette.Red();                                        // hype src/apptheme.cpp:70 (error = red)
    c.warning = palette.Yellow();                                   // ours: ANSI yellow
    c.success = palette.Green();                                    // ours: ANSI green
    c.scrim = bg.WithAlpha(0.5);                                    // shell.toml.tpl [menu] scrim-alpha 0.5
    c.scrollbarThumb = fg.WithAlpha(0.25);                          // ours
    c.scrollbarThumbActive = fg.WithAlpha(0.4);                     // ours: hovered or dragged
    return t;
}

} // namespace rgui::ui
